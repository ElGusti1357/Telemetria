#include <Arduino.h>
#include "config.h"              // define los pines y TINY_GSM_* ANTES de incluir TinyGSM
#include <TinyGsmClient.h>       // Hay que usar TinyGSM-fork de lewisxhe
#include "modem.h"
#include "sms_pdu.h"

#ifndef TINY_GSM_FORK_LIBRARY
  #error "Falta TinyGSM-fork de lewisxhe: https://github.com/lewisxhe/TinyGSM-fork"
#endif

//#define DUMP_AT_COMMANDS
#ifdef DUMP_AT_COMMANDS
  #include <StreamDebugger.h>
  static StreamDebugger debugger(SerialAT, Serial);
  static TinyGsm modem(debugger);
#else
  static TinyGsm modem(SerialAT);
#endif

// 0 = los SMS recibidos se muestran en UTF-8 real (acentos, ñ, emojis)
// 1 = acentos/ñ salen como letras simples y el resto como [U+XXXX] (para consolas sin UTF-8)
#define CONSOLE_ASCII_ONLY 0

// Largo maximo de una linea del modem. Un SMS en modo PDU llega como una sola linea
// hexadecimal de hasta ~360 caracteres. Protege la RAM si el UART emite ruido sin fin de linea.
#define MODEM_RX_LINE_MAX 512

static ModemEventFn _evt = nullptr;
static bool _ready = false;

// ---------- Recepcion de SMS ----------
static String rxLine;
static bool waitingSmsBody = false;      // true tras "+CMT:": la linea siguiente es el PDU en hexadecimal

static void emit(const String &event, const String &detail = "", bool isError = false) {
  if (_evt) _evt(event, detail, isError);
}

void modemSetEventHandler(ModemEventFn fn) { _evt = fn; }

static ModemSmsFn _smsFn = nullptr;
void modemSetSmsHandler(ModemSmsFn fn) { _smsFn = fn; }

static void printIncomingSms(const SmsDeliver &sms) {
  String text = CONSOLE_ASCII_ONLY ? smsUtf8ToAscii(sms.text) : sms.text;
  Serial.println();
  Serial.println("========== MENSAJE RECIBIDO ==========");
  Serial.println("Hay un mensaje nuevo!");
  Serial.print("De    : "); Serial.println(sms.sender);
  Serial.print("Fecha : "); Serial.println(sms.timestamp);
  if (sms.isPart) Serial.printf("Parte : %u de %u (mensaje largo)\n", (unsigned)sms.partNo, (unsigned)sms.partTotal);
  Serial.print("Texto : "); Serial.println(text);
  Serial.println("======================================");
  emit("SMS_RX", sms.sender + ": " + smsUtf8Truncate(sms.text, 80));
  if (_smsFn) _smsFn(sms.sender, sms.text, sms.isPart);       // control por SMS (solo encola)
}

static bool isHexLine(const String &s) {
  if (s.length() < 20 || (s.length() % 2) != 0) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

// Procesa una linea completa que llego del modem
static void handleModemLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (waitingSmsBody) {
    waitingSmsBody = false;
    if (isHexLine(line)) {
      // La linea siguiente a +CMT es el SMS completo en formato PDU (hexadecimal)
      SmsDeliver sms;
      if (smsParseDeliver(line, sms)) {
        printIncomingSms(sms);
      } else {
        Serial.println("SMS recibido en un formato PDU no soportado:");
        Serial.println(line);
        emit("SMS_RX_ERROR", "PDU no soportado", false);
      }
      return;
    }
    // No era el PDU esperado: se sigue procesando la linea normalmente
  }

  // Modo PDU: "+CMT: [<alpha>],<largo>" y en la linea siguiente el PDU
  if (line.startsWith("+CMT:")) {
    waitingSmsBody = true;
    return;
  }

  // +MSTK = comandos SIM Toolkit de la SIM (ruido normal del operador), se ignoran
  if (line.startsWith("+MSTK")) return;

  // Cualquier otra respuesta del modem se muestra como debug
  Serial.println(line);
}

// ---------- Arranque del modem (con limites) ----------
static bool startModem() {
  for (int round = 0; round < MODEM_START_ROUNDS; round++) {
    for (int i = 0; i < 10; i++) {
      if (modem.testAT(1000)) { Serial.println(); return true; }
      Serial.print(".");
    }
    Serial.println();
    Serial.printf("Sin respuesta del modem (ronda %d/%d), pulso de PWRKEY...\n", round + 1, MODEM_START_ROUNDS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(1000);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
  }
  return false;
}

// Espera SIM lista. Intenta desbloquear UNA sola vez (un PIN erroneo repetido bloquea la SIM).
static bool waitSimReady(uint32_t timeoutMs) {
  bool unlockTried = false;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    SimStatus sim = modem.getSimStatus();
    if (sim == SIM_READY) {
      Serial.println("SIM card online");
      return true;
    }
    if (sim == SIM_LOCKED) {
      if (strlen(SIM_PIN) == 0) {
        Serial.println("La SIM esta bloqueada y SIM_PIN esta vacio");
        return false;
      }
      if (unlockTried) {
        Serial.println("La SIM sigue bloqueada tras el desbloqueo: PIN incorrecto?");
        return false;
      }
      Serial.println("La SIM esta bloqueada. Desbloqueando...");
      modem.simUnlock(SIM_PIN);
      unlockTried = true;
      delay(2000);
      continue;
    }
    delay(1000);
  }
  Serial.println("Timeout esperando la SIM");
  return false;
}

// Devuelve 1 = registrado, 0 = timeout, -1 = rechazado
static int waitRegistration(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    RegStatus status = modem.getRegistrationStatus();
    switch (status) {
      case REG_OK_HOME:
        Serial.println("Registro OK");
        return 1;
      case REG_OK_ROAMING:
        Serial.println("Registro OK (roaming)");
        return 1;
      case REG_DENIED:
        Serial.println("Registro rechazado, revisar APN / SIM");
        return -1;
      case REG_UNREGISTERED:
      case REG_SEARCHING:
        Serial.printf("[%lu] Signal Quality:%d\n", millis() / 1000, modem.getSignalQuality());
        break;
      default:
        Serial.printf("Registration Status:%d\n", status);
        break;
    }
    delay(1000);
  }
  return 0;
}

ModemStatus modemInit() {
  _ready = false;

  // DTR en bajo: evita que quede flotante (en bajo el modem no debe entrar en sleep por DTR)
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  // Buffer de recepcion mas grande (por defecto 256 bytes). Si el loop() se bloquea unos
  // segundos (DHCP, escritura en la SD, envio de SMS), un SMS largo no se pierde.
  // Debe llamarse ANTES de begin().
  SerialAT.setRxBufferSize(1024);
  SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  // Reset y encendido
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
  digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL); delay(2600);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

  pinMode(BOARD_PWRKEY_PIN, OUTPUT);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH);
  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);

  Serial.println("Start modem...");
  if (!startModem()) return MODEM_ERR_NO_AT;
  emit("MODEM_OK");

  if (!waitSimReady(SIM_TIMEOUT_MS)) return MODEM_ERR_SIM;
  emit("SIM_OK");

  if (!modem.setNetworkMode(MODEM_NETWORK_AUTO)) {
    Serial.println("Set network mode failed!");
    emit("RED_MODO_FALLO", "no se pudo fijar el modo de red", true);
  }
  Serial.print("Current network mode : ");
  Serial.println(modem.getNetworkModeString());

#ifdef NETWORK_APN
  Serial.printf("Set network apn : %s\n", NETWORK_APN);
  modem.sendAT(GF("+CGDCONT=1,\"IP\",\""), NETWORK_APN, "\"");
  if (modem.waitResponse() != 1) {
    Serial.println("Set network apn error !");
    emit("APN_FALLO", NETWORK_APN, true);
  }
#endif

  Serial.println("Esperando registro en la red...");
  int reg = waitRegistration(REG_TIMEOUT_MS);
  if (reg < 0) return MODEM_ERR_REG_DENIED;
  if (reg == 0) return MODEM_ERR_REG_TIMEOUT;
  emit("RED_OK", "signal=" + String(modem.getSignalQuality()));
  Serial.println();

  // ---------- Configurar SMS ----------
  modem.sendAT("E0");                   // sin eco de comandos
  if (modem.waitResponse() != 1) {
    // Solo se registra en el log (no es motivo de SMS): los parsers buscan el ':' y toleran el eco
    emit("MODEM_ECO_FALLO", "no se pudo desactivar el eco", false);
  }
  // Modo PDU: la codificacion (GSM 7 bits o UCS2) la decide smsBuildSubmit() al enviar y
  // smsParseDeliver() al recibir, sin depender de AT+CSCS (que en modo texto distorsiona la ñ y los acentos)
  modem.sendAT("+CMGF=0");
  if (modem.waitResponse() != 1) {
    Serial.println("No se pudo activar el modo PDU (CMGF=0)");
    emit("SMS_CONFIG_FALLO", "CMGF=0 (modo PDU)", true);
  }
  // Entrega directa de los SMS entrantes por el puerto serie (+CMT)
  modem.sendAT("+CNMI=2,2,0,0,0");
  if (modem.waitResponse() != 1) {
    Serial.println("No se pudo configurar la recepcion de SMS (CNMI)");
    emit("SMS_CONFIG_FALLO", "CNMI", true);
  } else {
    Serial.println("Recepcion de SMS habilitada");
  }

  // Hora de la red celular (NITZ): con CTZU=1 el modem actualiza su reloj al registrarse.
  // Si no esta soportado se ignora: la hora por red es solo uno de los metodos de sincronizacion.
  modem.sendAT("+CTZU=1");
  modem.waitResponse();

  _ready = true;
  return MODEM_STATUS_OK;
}

const char *modemStatusText(ModemStatus s) {
  switch (s) {
    case MODEM_STATUS_OK:      return "OK";
    case MODEM_ERR_NO_AT:      return "El modem no responde a comandos AT";
    case MODEM_ERR_SIM:        return "SIM no disponible o bloqueada";
    case MODEM_ERR_REG_DENIED: return "Registro en la red rechazado (revisar APN / SIM)";
    case MODEM_ERR_REG_TIMEOUT:return "Timeout esperando registro en la red";
  }
  return "Error desconocido";
}

bool modemIsReady() { return _ready; }

int modemSignalQuality() { return modem.getSignalQuality(); }

// Envia un SMS en modo PDU. Como un celular: GSM 7 bits si el texto entra (incluye la ñ),
// UCS2 si no (acentos agudos, emojis). Los textos largos se parten en varias partes.
bool modemSendSms(const String &dest, const String &text) {
  static uint8_t concatRef = 0;
  std::vector<SmsPart> parts;
  SmsEncoding enc;

  if (!smsBuildSubmit(dest, text, parts, enc, ++concatRef)) {
    emit("SMS_TX_FALLO", dest + ": destino invalido, texto vacio o demasiado largo");
    return false;
  }
  Serial.printf("SMS: %s, %u parte(s)\n", enc == SMS_ENC_GSM7 ? "GSM 7 bits" : "UCS2", (unsigned)parts.size());

  for (size_t i = 0; i < parts.size(); i++) {
    String which = "parte " + String((unsigned)(i + 1)) + "/" + String((unsigned)parts.size());

    modem.sendAT(GF("+CMGS="), parts[i].tpduLen);
    if (modem.waitResponse(5000L, GF(">")) != 1) {
      emit("SMS_TX_FALLO", dest + ": el modem no acepto el envio (" + which + ")");
      return false;
    }
    SerialAT.print(parts[i].pdu);
    SerialAT.write((uint8_t)0x1A);                  // Ctrl+Z: fin del mensaje
    SerialAT.flush();
    if (modem.waitResponse(60000L, GF("\r\n+CMGS:")) != 1) {
      emit("SMS_TX_FALLO", dest + ": sin confirmacion de la red (" + which + ")");
      return false;
    }
    modem.waitResponse();                           // "OK" final
  }

  String extra = parts.size() > 1 ? " [" + String((unsigned)parts.size()) + " partes]" : "";
  emit("SMS_TX", dest + ": " + smsUtf8Truncate(text, 80) + extra);
  return true;
}

void modemProcess() {
  // Leer del modem linea por linea
  while (SerialAT.available()) {
    char c = SerialAT.read();
    if (c == '\n') {
      handleModemLine(rxLine);
      rxLine = "";
    } else if (c != '\r') {
      if (rxLine.length() < MODEM_RX_LINE_MAX) rxLine += c;   // limite de seguridad
    }
  }
}

bool modemIsIdle() {
  return !waitingSmsBody && rxLine.length() == 0 && !SerialAT.available();
}

bool modemGetNetworkTime(String &raw) {
  raw = "";
  modem.sendAT(GF("+CCLK?"));
  if (modem.waitResponse(2000L, GF("+CCLK: \"")) != 1) return false;
  raw = SerialAT.readStringUntil('"');       // aa/mm/dd,hh:mm:ss+zz
  modem.waitResponse();                      // "OK" final
  raw.trim();
  return raw.length() >= 17;
}

void modemSendRawAT(const String &cmd) {
  SerialAT.print(cmd);
  SerialAT.print("\r\n");
}

Stream &modemSerial() { return SerialAT; }
