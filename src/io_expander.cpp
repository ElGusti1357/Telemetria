#include "io_expander.h"
#include "config.h"
#include <Wire.h>

// ---------- Registros del MCP23017 (modo BANK=0) ----------
// Cada registro tiene par A/B en direcciones consecutivas; con SEQOP=0 (por defecto)
// el puntero avanza solo, por lo que se leen/escriben A y B en una sola transaccion.
#define REG_IODIR   0x00    // 1 = entrada
#define REG_IPOL    0x02    // 1 = lectura invertida
#define REG_DEFVAL  0x06    // sin uso (no usamos interrupciones): se usa como "firma" para detectar resets
#define REG_IOCON   0x0A
#define REG_GPPU    0x0C    // 1 = pull-up interno de 100k
#define REG_GPIO    0x12    // lectura de pines
#define REG_OLAT    0x14    // latch de salidas

#define IO_SIGNATURE 0xA5

// Si se sube IO_CHIPS en config.h por encima de lo que soportan las tablas, falla al compilar
// (en vez de ignorar en silencio los pines que no entran)
static_assert(IO_CHIPS * 16 <= IO_MAX_CH, "IO_CHIPS * 16 supera IO_MAX_CH (io_expander.h)");

static const uint8_t     addrs[IO_CHIPS]   = IO_I2C_ADDRS;
static const char *const mapRows[IO_CHIPS] = IO_MAP;

// ---------- Estado ----------
struct Chip {
  bool     online;
  uint8_t  failCount;
  uint32_t lastRetryMs;
  uint16_t dir;       // IODIR deseado (1 = entrada)
  uint16_t pullup;    // GPPU deseado
  uint16_t ipol;      // IPOL deseado
  uint16_t olat;      // estado deseado de las salidas
  uint16_t inMask;    // pines que son entradas logicas
  uint16_t outMask;   // pines que son salidas logicas
  uint16_t gpio;      // ultima lectura cruda
};
static Chip chips[IO_CHIPS];

struct Chan { uint8_t chip; uint8_t pin; };
static Chan    inCh[IO_MAX_CH];
static Chan    outCh[IO_MAX_CH];
static uint8_t nIn = 0, nOut = 0;
static bool    inStable[IO_MAX_CH];
static uint8_t inCount[IO_MAX_CH];

static IoInputFn _inFn = nullptr;
static IoEventFn _evFn = nullptr;

void ioSetInputHandler(IoInputFn fn) { _inFn = fn; }
void ioSetEventHandler(IoEventFn fn) { _evFn = fn; }

static void emit(const String &event, const String &detail, bool isError) {
  if (_evFn) _evFn(event, detail, isError);
}

static String addrText(uint8_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%02X", addrs[c]);
  return String(buf);
}

// ---------- I2C ----------
static bool writeReg8(uint8_t c, uint8_t reg, uint8_t v) {
  Wire.beginTransmission(addrs[c]);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

static bool writeReg16(uint8_t c, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addrs[c]);
  Wire.write(reg);
  Wire.write((uint8_t)(v & 0xFF));     // puerto A
  Wire.write((uint8_t)(v >> 8));       // puerto B
  return Wire.endTransmission() == 0;
}

static bool readReg8(uint8_t c, uint8_t reg, uint8_t &v) {
  Wire.beginTransmission(addrs[c]);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addrs[c], (uint8_t)1) != 1) return false;
  v = Wire.read();
  return true;
}

static bool readReg16(uint8_t c, uint8_t reg, uint16_t &v) {
  Wire.beginTransmission(addrs[c]);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addrs[c], (uint8_t)2) != 2) return false;
  uint8_t a = Wire.read();
  uint8_t b = Wire.read();
  v = (uint16_t)a | ((uint16_t)b << 8);
  return true;
}

// Escribe toda la configuracion del chip. Devuelve false si no responde.
static bool configureChip(uint8_t c) {
  Chip &ch = chips[c];

  Wire.beginTransmission(addrs[c]);
  if (Wire.endTransmission() != 0) return false;       // no responde

  bool ok = true;
  ok &= writeReg8 (c, REG_IOCON, 0x00);                // BANK=0, direccion auto-incremental
  ok &= writeReg16(c, REG_OLAT,  ch.olat);             // 1ro el estado de las salidas...
  ok &= writeReg16(c, REG_IPOL,  ch.ipol);
  ok &= writeReg16(c, REG_GPPU,  ch.pullup);
  ok &= writeReg16(c, REG_IODIR, ch.dir);              // ...y recien al final los pines pasan a salida
  ok &= writeReg8 (c, REG_DEFVAL, IO_SIGNATURE);       // firma para detectar resets
  return ok;
}

// Cuenta un error I2C; tras IO_FAIL_LIMIT seguidos el chip se da por caido
static void chipError(uint8_t c) {
  Chip &ch = chips[c];
  if (ch.failCount < 255) ch.failCount++;
  if (ch.online && ch.failCount >= IO_FAIL_LIMIT) {
    ch.online = false;
    ch.lastRetryMs = millis();
    emit("IO_CHIP_FALLO", "MCP23017 " + addrText(c) + " no responde", true);
  }
}

// Interpreta IO_MAP y arma las listas de entradas y salidas logicas
static void parseMap() {
  nIn = nOut = 0;
  for (uint8_t c = 0; c < IO_CHIPS; c++) {
    Chip &ch = chips[c];
    ch.dir = 0xFFFF; ch.pullup = 0; ch.ipol = 0; ch.olat = 0;
    ch.inMask = 0; ch.outMask = 0; ch.gpio = 0;

    const char *row = mapRows[c];
    if (strlen(row) != 16) {
      emit("IO_MAPA", "la fila del chip " + String(c) + " no tiene 16 caracteres", true);
    }

    for (uint8_t p = 0; p < 16; p++) {
      char r = (p < strlen(row)) ? row[p] : '-';
      uint16_t bit = (uint16_t)1 << p;
      switch (r) {
        case 'I':
          ch.inMask |= bit;
          if (nIn < IO_MAX_CH) inCh[nIn++] = { c, p };
          break;
        case 'L':
          ch.inMask |= bit;
          ch.pullup |= bit;
          ch.ipol   |= bit;
          if (nIn < IO_MAX_CH) inCh[nIn++] = { c, p };
          break;
        case 'O':
          ch.dir &= ~bit;                      // 0 = salida
          ch.outMask |= bit;
          if (nOut < IO_MAX_CH) outCh[nOut++] = { c, p };
          break;
        default:                               // '-' libre: entrada con pull-up, sin uso
          ch.pullup |= bit;
          break;
      }
    }
  }
}

bool ioBegin() {
  if (IO_RESET_PIN >= 0) {
    pinMode(IO_RESET_PIN, OUTPUT);
    digitalWrite(IO_RESET_PIN, LOW);
    delay(2);
    digitalWrite(IO_RESET_PIN, HIGH);
    delay(2);
  }

  Wire.begin(IO_I2C_SDA, IO_I2C_SCL, IO_I2C_FREQ);
  Wire.setTimeOut(IO_I2C_TIMEOUT_MS);

  parseMap();

  bool allOk = true;
  for (uint8_t c = 0; c < IO_CHIPS; c++) {
    Chip &ch = chips[c];
    ch.failCount = 0;
    ch.lastRetryMs = millis();
    ch.online = configureChip(c);
    if (ch.online) {
      uint16_t v;
      if (readReg16(c, REG_GPIO, v)) ch.gpio = v;
    } else {
      allOk = false;
      emit("IO_CHIP_FALLO", "MCP23017 " + addrText(c) + " no responde al iniciar", true);
    }
  }

  // Estado inicial de las entradas (sin generar eventos)
  for (uint8_t i = 0; i < nIn; i++) {
    inStable[i] = (chips[inCh[i].chip].gpio >> inCh[i].pin) & 1;
    inCount[i] = 0;
  }

  if (nIn != IO_EXPECTED_INPUTS || nOut != IO_EXPECTED_OUTPUTS) {
    emit("IO_MAPA", "el mapa define " + String(nIn) + " entradas y " + String(nOut) +
                    " salidas (se esperaban " + String(IO_EXPECTED_INPUTS) + " y " +
                    String(IO_EXPECTED_OUTPUTS) + ")", true);
  }

  Serial.printf("IO: %u entradas, %u salidas, chips %s\n", nIn, nOut, allOk ? "OK" : "CON FALLAS");
  return allOk;
}

void ioUpdate() {
  uint32_t now = millis();

  // 1) Reintentar chips caidos
  for (uint8_t c = 0; c < IO_CHIPS; c++) {
    Chip &ch = chips[c];
    if (!ch.online && now - ch.lastRetryMs >= IO_RETRY_MS) {
      ch.lastRetryMs = now;
      if (configureChip(c)) {
        ch.online = true;
        ch.failCount = 0;
        emit("IO_CHIP_RECUPERADO", "MCP23017 " + addrText(c) + " vuelve a responder", false);
      }
    }
  }

  // 2) Verificar que ningun chip se haya reseteado (microcorte): la firma debe seguir
  static uint32_t lastVerify = 0;
  if (now - lastVerify >= IO_VERIFY_MS) {
    lastVerify = now;
    for (uint8_t c = 0; c < IO_CHIPS; c++) {
      Chip &ch = chips[c];
      if (!ch.online) continue;
      uint8_t sig;
      if (!readReg8(c, REG_DEFVAL, sig)) {
        chipError(c);
      } else if (sig != IO_SIGNATURE) {
        emit("IO_CHIP_RESET", "MCP23017 " + addrText(c) + " se reseteo, se reconfigura", true);
        if (!configureChip(c)) chipError(c);
      } else {
        ch.failCount = 0;
      }
    }
  }

  // 3) Leer entradas cada IO_POLL_MS
  static uint32_t lastPoll = 0;
  if (now - lastPoll < IO_POLL_MS) return;
  lastPoll = now;

  for (uint8_t c = 0; c < IO_CHIPS; c++) {
    Chip &ch = chips[c];
    if (!ch.online || ch.inMask == 0) continue;
    uint16_t v;
    if (!readReg16(c, REG_GPIO, v)) { chipError(c); continue; }
    ch.gpio = v;
    ch.failCount = 0;
  }

  // 4) Antirrebote: un cambio se acepta tras IO_DEBOUNCE_SAMPLES lecturas iguales seguidas
  for (uint8_t i = 0; i < nIn; i++) {
    const Chip &ch = chips[inCh[i].chip];
    if (!ch.online) continue;
    bool raw = (ch.gpio >> inCh[i].pin) & 1;
    if (raw == inStable[i]) {
      inCount[i] = 0;
    } else if (++inCount[i] >= IO_DEBOUNCE_SAMPLES) {
      inStable[i] = raw;
      inCount[i] = 0;
      if (_inFn) _inFn(i + 1, raw);
    }
  }
}

// ---------- Cantidades ----------
uint8_t ioInputCount()  { return nIn; }
uint8_t ioOutputCount() { return nOut; }

// ---------- Entradas ----------
bool ioReadInput(uint8_t n) {
  if (n < 1 || n > nIn) return false;
  return inStable[n - 1];
}

uint64_t ioReadInputs() {
  uint64_t m = 0;
  for (uint8_t i = 0; i < nIn; i++) if (inStable[i]) m |= (uint64_t)1 << i;
  return m;
}

// ---------- Salidas ----------
// Escribe el latch completo del chip (A y B en una sola transaccion)
static bool applyOlat(uint8_t c) {
  Chip &ch = chips[c];
  if (!ch.online) return false;
  if (!writeReg16(c, REG_OLAT, ch.olat)) { chipError(c); return false; }
  ch.failCount = 0;
  return true;
}

bool ioWriteOutput(uint8_t n, bool state) {
  if (n < 1 || n > nOut) return false;
  const Chan &o = outCh[n - 1];
  uint16_t bit = (uint16_t)1 << o.pin;
  if (state) chips[o.chip].olat |= bit;
  else       chips[o.chip].olat &= ~bit;
  return applyOlat(o.chip);
}

bool ioToggleOutput(uint8_t n) {
  if (n < 1 || n > nOut) return false;
  return ioWriteOutput(n, !ioReadOutput(n));
}

bool ioReadOutput(uint8_t n) {
  if (n < 1 || n > nOut) return false;
  const Chan &o = outCh[n - 1];
  return (chips[o.chip].olat >> o.pin) & 1;
}

bool ioWriteOutputs(uint64_t mask) {
  for (uint8_t i = 0; i < nOut; i++) {
    uint16_t bit = (uint16_t)1 << outCh[i].pin;
    if ((mask >> i) & 1) chips[outCh[i].chip].olat |= bit;
    else                 chips[outCh[i].chip].olat &= ~bit;
  }
  bool ok = true;
  for (uint8_t c = 0; c < IO_CHIPS; c++) {
    if (chips[c].outMask) ok &= applyOlat(c);          // un solo write por chip
  }
  return ok;
}

uint64_t ioReadOutputs() {
  uint64_t m = 0;
  for (uint8_t i = 0; i < nOut; i++) if (ioReadOutput(i + 1)) m |= (uint64_t)1 << i;
  return m;
}

// ---------- Estado ----------
bool ioChipOnline(uint8_t chip) { return chip < IO_CHIPS && chips[chip].online; }

bool ioAllOnline() {
  for (uint8_t c = 0; c < IO_CHIPS; c++) if (!chips[c].online) return false;
  return true;
}

void ioPrintStatus(Print &out) {
  for (uint8_t c = 0; c < IO_CHIPS; c++) {
    out.printf("Chip %u (%s): %s\n", c, addrText(c).c_str(), chips[c].online ? "ONLINE" : "SIN RESPUESTA");
  }
  out.printf("Entradas (%u), de la 1 a la %u: ", nIn, nIn);
  for (uint8_t i = 0; i < nIn; i++) out.print(inStable[i] ? '1' : '0');
  out.println();
  out.printf("Salidas  (%u), de la 1 a la %u: ", nOut, nOut);
  for (uint8_t i = 0; i < nOut; i++) out.print(ioReadOutput(i + 1) ? '1' : '0');
  out.println();
}
