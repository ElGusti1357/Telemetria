#include "sd_log.h"
#include "config.h"
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

static bool         _ready = false;          // tarjeta montada y utilizable
static bool         _wantSd = false;         // sdBegin() ya fue llamado: si la tarjeta falta, se reintenta montarla
static int8_t       _cs = -1;
static uint32_t     lastRemountMs = 0;
static bool         remountMsgShown = false;
static bool         lostReported = false;    // ya se aviso la perdida de la tarjeta (se rearma al recuperarla)
static bool         lostAlertPending = false;
static SdLogEventFn _evFn = nullptr;

void sdLogSetEventHandler(SdLogEventFn fn) { _evFn = fn; }

static void emit(const String &event, const String &detail, bool isError) {
  if (_evFn) _evFn(event, detail, isError);
}

// Eventos que ocurren mientras la tarjeta no esta (retirada, sin insertar al arrancar): se guardan
// en RAM (los ultimos SDLOG_RAM_LINES) y se vuelcan al log cuando la tarjeta vuelve.
static String   ramBuf[SDLOG_RAM_LINES];
static int      ramHead = 0, ramCount = 0;
static uint32_t ramDropped = 0;             // eventos que no entraron en la memoria (se descartaron los mas viejos)

static void ramPush(const String &line) {
  if (ramCount == SDLOG_RAM_LINES) {         // memoria llena: se descarta el mas viejo
    ramHead = (ramHead + 1) % SDLOG_RAM_LINES;
    ramCount--;
    ramDropped++;
  }
  ramBuf[(ramHead + ramCount) % SDLOG_RAM_LINES] = line;
  ramCount++;
}

static void logInit();      // mas abajo

// Monta la tarjeta. Sirve tanto para el arranque como para volver a montarla tras retirarla y reinsertarla.
static bool mountCard() {
  SD.end();                                  // libera un montaje anterior (no hace nada si no habia)
  bool ok = SD.begin(_cs, SPI, 4000000, SDLOG_MOUNT);
  if (ok && SD.cardType() == CARD_NONE) { SD.end(); ok = false; }
  return ok;
}

bool sdBegin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs) {
  _cs = cs;
  _wantSd = true;                            // desde ahora, si la tarjeta falta se reintenta (ver sdLogMaintain)
  lastRemountMs = millis();
  SPI.begin(sck, miso, mosi, cs);
  _ready = mountCard();
  if (_ready) logInit();
  return _ready;
}

bool sdIsReady() { return _ready; }

// =====================================================================
//  Archivos genericos
// =====================================================================
bool sdAppendLine(const char *path, const String &line) {
  if (!_ready) return false;
  File f = SD.open(path, FILE_APPEND);
  if (!f && !SD.exists(path)) {
    f = SD.open(path, FILE_WRITE);  // algunas versiones no crean el archivo con append
  }
  if (!f) {
    Serial.printf("[SD] No se pudo abrir %s para escribir\n", path);
    return false;
  }
  size_t n = f.println(line);
  f.close();                       // cerrar = datos realmente guardados
  if (n == 0) {
    Serial.printf("[SD] Fallo la escritura en %s\n", path);
    return false;
  }
  return true;
}

bool sdWriteFile(const char *path, const String &content) {
  if (!_ready) return false;
  File f = SD.open(path, FILE_WRITE);   // FILE_WRITE trunca el archivo
  if (!f) return false;
  size_t n = f.print(content);
  f.close();
  return n == content.length();
}

bool sdReadFile(const char *path, String &out, size_t maxBytes) {
  out = "";
  if (!_ready) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  size_t sz = f.size();
  bool truncated = false;
  if (sz > maxBytes) {
    f.seek(sz - maxBytes);
    truncated = true;
  }
  out.reserve(sz > maxBytes ? maxBytes : sz);
  while (f.available()) out += (char)f.read();
  f.close();

  if (truncated) {                 // descartar la primera linea (queda cortada)
    int nl = out.indexOf('\n');
    if (nl >= 0) out = out.substring(nl + 1);
  }
  return true;
}

bool sdDeleteFile(const char *path) {
  if (!_ready) return false;
  return SD.remove(path);
}

bool sdFileExists(const char *path) {
  if (!_ready) return false;
  return SD.exists(path);
}

size_t sdFileSize(const char *path) {
  if (!_ready) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

void sdInfo(uint32_t &totalMB, uint32_t &usedMB) {
  totalMB = usedMB = 0;
  if (!_ready) return;
  totalMB = (uint32_t)(SD.totalBytes() / (1024ULL * 1024ULL));
  usedMB  = (uint32_t)(SD.usedBytes()  / (1024ULL * 1024ULL));
}

int sdUsedPercent() {
  if (!_ready) return -1;
  uint64_t total = SD.totalBytes();
  if (total == 0) return -1;
  return (int)((SD.usedBytes() * 100ULL) / total);
}

// =====================================================================
//  Hora
// =====================================================================
// El reloj del ESP32 arranca en 1970; se considera valido si es posterior a 2024-01-01
bool sdLogIsTimeSynced() { return time(nullptr) > 1704067200; }

String sdLogNow() {
  if (!sdLogIsTimeSynced()) {
    return "T+" + String((unsigned long)(millis() / 1000)) + "s";
  }
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);              // aplica la zona horaria definida con TZ
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(buf);
}

// =====================================================================
//  Log circular por segmentos
// =====================================================================
#define SEG_HEADER "fecha_hora;evento;detalle"

struct SegInfo { uint32_t seq; size_t size; };

static size_t   segBytes = 256UL * 1024UL;   // tamano de cada segmento (se calcula en logInit)
static FILE    *curFp = nullptr;             // segmento actual: se mantiene ABIERTO entre eventos
static uint32_t oldestSeq = 1;     // segmento mas viejo que existe
static uint32_t curSeq = 1;        // segmento donde se escribe
static size_t   curBytes = 0;      // bytes ya escritos en el segmento actual
static bool     curExists = false; // el archivo del segmento actual ya fue creado
static String   recycleNote;       // aviso pendiente de "se borraron segmentos"
static bool     fullAlertPending = false;
static bool     fullReported = false;
static bool     inEvent = false;   // evita recursion cuando el manejador de eventos vuelve a loguear

static void segPath(uint32_t seq, char *buf, size_t n) {
  snprintf(buf, n, "%s/e%07lu.csv", SDLOG_DIR, (unsigned long)seq);
}

// Abrir/cerrar el segmento actual. Se mantiene abierto: en FAT cada open() recorre la carpeta y
// recorre la cadena de clusters del archivo hasta el final, y eso crece con la cantidad de archivos
// y con el tamano del segmento. Abriendolo una sola vez, escribir cuesta lo mismo siempre.
//
// La escritura usa fwrite/fsync (API estandar) y no File::println/flush de Arduino, porque estas
// ultimas NO informan si el dato llego a la tarjeta: con la SD retirada o llena el error pasaba
// inadvertido. Aqui un fallo de fopen, fwrite o fsync se detecta en el acto.
static void closeCur() {
  if (curFp) { fclose(curFp); curFp = nullptr; }
}

static bool openCur() {
  if (curFp) return true;
  char path[64];
  snprintf(path, sizeof(path), "%s%s/e%07lu.csv", SDLOG_MOUNT, SDLOG_DIR, (unsigned long)curSeq);
  curFp = fopen(path, "a");                  // "a" crea el archivo si no existe
  if (!curFp) return false;
  setvbuf(curFp, NULL, _IONBF, 0);           // sin buffer intermedio: un fallo se ve en la propia escritura
  return true;
}

// Escribe una linea y la confirma en la tarjeta (fsync). Devuelve false si cualquier paso falla.
static bool appendCur(const String &line) {
  if (!openCur()) return false;
  String data = line + "\r\n";
  size_t n = fwrite(data.c_str(), 1, data.length(), curFp);
  if (n != data.length() || fsync(fileno(curFp)) != 0) { closeCur(); return false; }
  return true;
}

// Tamano de los segmentos: fijo si SDLOG_SEGMENT_BYTES > 0; si no, segun la capacidad, de modo que
// al llegar al limite haya ~SDLOG_TARGET_SEGMENTS archivos (multiplo de 64 KB, entre MIN y MAX)
static size_t computeSegmentBytes() {
  if (SDLOG_SEGMENT_BYTES > 0) return (size_t)SDLOG_SEGMENT_BYTES;
  uint64_t total = SD.totalBytes();
  uint64_t b = total * (uint64_t)SDLOG_MAX_USED_PERCENT / 100ULL / (uint64_t)SDLOG_TARGET_SEGMENTS;
  b = (b / 65536ULL) * 65536ULL;
  if (b < (uint64_t)SDLOG_SEGMENT_MIN_BYTES) b = SDLOG_SEGMENT_MIN_BYTES;
  if (b > (uint64_t)SDLOG_SEGMENT_MAX_BYTES) b = SDLOG_SEGMENT_MAX_BYTES;
  return (size_t)b;
}

size_t sdLogSegmentBytes() { return segBytes; }

// "e0000012.csv" (con o sin ruta, mayusculas o minusculas) -> 12
static bool parseSegName(const char *full, uint32_t &seq) {
  const char *name = strrchr(full, '/');
  name = name ? name + 1 : full;
  if (strlen(name) != 12) return false;
  if (name[0] != 'e' && name[0] != 'E') return false;
  uint32_t v = 0;
  for (int i = 1; i <= 7; i++) {
    if (name[i] < '0' || name[i] > '9') return false;
    v = v * 10 + (uint32_t)(name[i] - '0');
  }
  if (strcasecmp(name + 8, ".csv") != 0) return false;
  seq = v;
  return true;
}

// Lista los segmentos existentes, de mas viejo a mas nuevo
static void listSegments(std::vector<SegInfo> &out) {
  out.clear();
  File dir = SD.open(SDLOG_DIR);
  if (!dir) return;
  if (!dir.isDirectory()) { dir.close(); return; }
  File f = dir.openNextFile();
  while (f) {
    uint32_t seq;
    if (!f.isDirectory() && parseSegName(f.name(), seq)) {
      SegInfo si = { seq, (size_t)f.size() };
      out.push_back(si);
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  std::sort(out.begin(), out.end(), [](const SegInfo &a, const SegInfo &b) { return a.seq < b.seq; });
}

// ¿Hay que liberar espacio? (uso sobre el limite, o muy poco espacio libre)
static bool needSpace() {
  uint64_t total = SD.totalBytes();
  if (total == 0) return false;               // no se puede calcular: no borrar a ciegas
  uint64_t used = SD.usedBytes();
  if (used * 100ULL >= total * (uint64_t)SDLOG_MAX_USED_PERCENT) return true;
  return (total > used) && (total - used) < (uint64_t)SDLOG_MIN_FREE_BYTES;
}

// Borra el segmento mas viejo (nunca el actual). Solo libera sus clusters: no copia ni reescribe datos.
static bool deleteOldestSegment(size_t &freedBytes) {
  if (oldestSeq >= curSeq) return false;
  char path[40];
  segPath(oldestSeq, path, sizeof(path));
  if (SD.exists(path)) {
    size_t sz = sdFileSize(path);
    if (!SD.remove(path)) return false;       // no se pudo borrar: no insistir
    freedBytes += sz;
  }
  oldestSeq++;
  return true;
}

// Borra segmentos viejos mientras haga falta espacio
static void trimToLimit() {
  int removed = 0;
  size_t freed = 0;
  while (oldestSeq < curSeq && needSpace()) {
    if (!deleteOldestSegment(freed)) break;
    removed++;
  }
  if (removed > 0) {
    char buf[112];
    snprintf(buf, sizeof(buf), "borrados %d segmento(s) viejo(s) (%lu KB), SD al %d%%",
             removed, (unsigned long)(freed / 1024), sdUsedPercent());
    recycleNote = String(buf);
  }

  // Sigue sobre el limite aunque solo queda el segmento actual: algo mas ocupa la tarjeta
  if (oldestSeq >= curSeq && needSpace()) {
    if (!fullReported) { fullReported = true; fullAlertPending = true; }
  } else if (!needSpace()) {
    fullReported = false;
  }
}

static void logInit() {
  uint32_t t0 = millis();
  closeCur();
  segBytes = computeSegmentBytes();
  if (!SD.exists(SDLOG_DIR)) SD.mkdir(SDLOG_DIR);

  std::vector<SegInfo> segs;
  listSegments(segs);

  // Incorporar el archivo unico de versiones anteriores como primer segmento
  if (SD.exists("/eventos.csv") && segs.empty()) {
    char first[40];
    segPath(1, first, sizeof(first));
    if (SD.rename("/eventos.csv", first)) {
      Serial.println("[SD] /eventos.csv incorporado como primer segmento del log");
      listSegments(segs);
    }
  }

  if (segs.empty()) {
    oldestSeq = curSeq = 1;
    curBytes = 0;
    curExists = false;
  } else {
    oldestSeq = segs.front().seq;
    curSeq    = segs.back().seq;
    curBytes  = segs.back().size;
    curExists = true;
  }
  fullReported = false;

  Serial.printf("SD: %lu MB, usada %d%% | log: %u segmento(s) de %lu KB (arranque del log: %lu ms)\n",
                (unsigned long)(SD.totalBytes() / (1024ULL * 1024ULL)), sdUsedPercent(),
                (unsigned)segs.size(), (unsigned long)(segBytes / 1024), (unsigned long)(millis() - t0));
  trimToLimit();     // la tarjeta puede haber arrancado ya sobre el limite
}

// Escribe una linea en el segmento actual (rotando y recuperando espacio si hace falta)
static bool writeLine(const String &line) {
  size_t need = line.length() + 2;           // println agrega \r\n

  // ¿Entra en el segmento actual? Si no, se cierra y se abre el siguiente
  if (curExists && curBytes > 0 && curBytes + need > segBytes) {
    closeCur();
    curSeq++;
    curBytes = 0;
    curExists = false;
    trimToLimit();                           // liberar espacio ANTES de crear el nuevo segmento
  }

  for (int attempt = 0; attempt < 3; attempt++) {
    bool ok = true;
    if (!curExists) {
      ok = appendCur(SEG_HEADER);            // cada segmento empieza con su encabezado
      if (ok) { curBytes += strlen(SEG_HEADER) + 2; curExists = true; }
    }
    if (ok) ok = appendCur(line);
    if (ok) { curBytes += need; return true; }

    // Fallo (tarjeta llena, retirada o error transitorio): cerrar para reabrir en el proximo intento.
    // Si el segundo intento tambien falla y la tarjeta esta llena, liberar el segmento mas viejo.
    closeCur();
    size_t freed = 0;
    if (attempt == 1 && oldestSeq < curSeq && needSpace() && deleteOldestSegment(freed)) {
      char buf[96];
      snprintf(buf, sizeof(buf), "tarjeta llena al escribir: borrado 1 segmento viejo (%lu KB)",
               (unsigned long)(freed / 1024));
      recycleNote = String(buf);
    }
  }
  Serial.println("[SD] No se pudo escribir en el log");
  return false;
}

// Mensaje de error y avisos pendientes; se llaman con el estado del log ya consistente
static void flushNotices() {
  if (recycleNote.length() > 0) {
    String n = recycleNote;
    recycleNote = "";
    String line = sdLogNow() + ";SD_RECICLADO;" + n;
    Serial.println("[LOG] " + line);
    if (_ready) writeLine(line);             // solo queda en el log (no pasa por el manejador de eventos)
  }
  if (lostAlertPending) {
    lostAlertPending = false;
    emit("SD_PERDIDA", String("la tarjeta SD dejo de responder (retirada o falla): los eventos se guardan en memoria y se "
                              "reintenta montarla cada ") + String((unsigned long)(SDLOG_REMOUNT_MS / 1000)) + " s", true);
  }
  if (fullAlertPending) {
    fullAlertPending = false;
    char buf[112];
    snprintf(buf, sizeof(buf), "la tarjeta esta al %d%% con solo el segmento actual: liberar espacio", sdUsedPercent());
    emit("SD_LLENA", String(buf), true);
  }
}

// Evita que el texto rompa el formato del CSV
static String sanitize(String s) {
  s.replace('\r', ' ');
  s.replace('\n', ' ');
  s.replace(';', ',');
  return s;
}

// La tarjeta dejo de responder: se cierra todo, se desmonta y se espera a que vuelva
static void markLost() {
  closeCur();
  SD.end();
  _ready = false;
  lastRemountMs = millis();
  remountMsgShown = false;
  if (!lostReported) { lostReported = true; lostAlertPending = true; }   // se avisa una sola vez por perdida
}

static void runNotices() {
  if (inEvent) return;
  inEvent = true;              // el manejador puede volver a loguear: se permite, sin repetir los avisos
  flushNotices();
  inEvent = false;
}

bool sdLogEvent(const String &event, const String &detail) {
  String line = sdLogNow() + ";" + sanitize(event) + ";" + sanitize(detail);
  Serial.println("[LOG] " + line);

  if (!_wantSd) return false;                // la SD nunca se inicializo
  if (!_ready) {                             // tarjeta ausente: se guarda en memoria hasta recuperarla
    ramPush(line);
    return false;
  }

  bool ok = writeLine(line);
  if (!ok && !needSpace()) {
    // No es falta de espacio (eso ya se trata en writeLine): la tarjeta dejo de responder
    ramPush(line);
    markLost();
  }
  runNotices();
  return ok;
}

// Llamar en cada vuelta de loop(): si la tarjeta falta (retirada o no insertada al arrancar), intenta
// volver a montarla cada SDLOG_REMOUNT_MS. Al lograrlo vuelca al log los eventos guardados en memoria.
void sdLogMaintain() {
  if (!_wantSd || _ready) return;
  uint32_t now = millis();
  if (now - lastRemountMs < SDLOG_REMOUNT_MS) return;
  lastRemountMs = now;

  if (!mountCard()) {
    if (!remountMsgShown) {
      remountMsgShown = true;
      Serial.printf("[SD] sin tarjeta: se reintenta montarla cada %lu s\n", (unsigned long)(SDLOG_REMOUNT_MS / 1000));
    }
    return;
  }

  // Volvio: se rearma el estado del log y se vuelcan, en orden, los eventos guardados en memoria
  _ready = true;
  logInit();
  int saved = 0;
  uint32_t dropped = ramDropped;
  while (ramCount > 0) {
    if (!writeLine(ramBuf[ramHead])) {       // fallo de nuevo: se vuelve a esperar (lo pendiente sigue en memoria)
      markLost();
      runNotices();
      return;
    }
    ramHead = (ramHead + 1) % SDLOG_RAM_LINES;
    ramCount--;
    saved++;
  }
  ramDropped = 0;
  lostReported = false;                      // recuperada: la proxima perdida se avisa de nuevo

  char buf[112];
  snprintf(buf, sizeof(buf), "SD de nuevo disponible: %d evento(s) guardados desde memoria, %lu perdidos",
           saved, (unsigned long)dropped);
  String line = sdLogNow() + ";SD_RECUPERADA;" + String(buf);
  Serial.println("[LOG] " + line);
  writeLine(line);
  runNotices();
}

bool sdLogReadTail(String &out, size_t maxBytes) {
  out = "";
  if (!_ready) return false;
  closeCur();                                // se reabre solo en la proxima escritura

  std::vector<SegInfo> segs;
  listSegments(segs);
  if (segs.empty()) return false;

  size_t remaining = maxBytes;
  bool any = false;
  for (size_t i = segs.size(); i-- > 0 && remaining > 0; ) {
    char path[40];
    segPath(segs[i].seq, path, sizeof(path));
    String chunk;
    if (!sdReadFile(path, chunk, remaining)) break;
    bool cut = segs[i].size > remaining;            // este segmento quedo cortado: no seguir hacia atras
    if (!cut && chunk.startsWith("fecha_hora;")) {  // sin el encabezado de cada segmento
      int nl = chunk.indexOf('\n');
      chunk = (nl >= 0) ? chunk.substring(nl + 1) : String("");
    }
    out = chunk + out;
    any = true;
    if (cut) break;
    remaining -= segs[i].size;
  }
  return any;
}

bool sdLogClear() {
  if (!_ready) return false;
  closeCur();
  std::vector<SegInfo> segs;
  listSegments(segs);
  bool ok = true;
  for (size_t i = 0; i < segs.size(); i++) {
    char path[40];
    segPath(segs[i].seq, path, sizeof(path));
    ok &= SD.remove(path);
  }
  if (SD.exists("/eventos.csv")) ok &= SD.remove("/eventos.csv");
  oldestSeq = curSeq = 1;
  curBytes = 0;
  curExists = false;
  fullReported = false;
  return ok;
}

void sdLogPrintStatus(Print &out) {
  if (!_ready) {
    out.printf("Log: SD NO DISPONIBLE (se reintenta cada %lu s) | %d evento(s) en memoria, %lu perdidos\n",
               (unsigned long)(SDLOG_REMOUNT_MS / 1000), ramCount, (unsigned long)ramDropped);
    return;
  }
  closeCur();
  std::vector<SegInfo> segs;
  listSegments(segs);
  size_t total = 0;
  for (size_t i = 0; i < segs.size(); i++) total += segs[i].size;
  out.printf("Log circular: %u segmento(s), %lu KB en total", (unsigned)segs.size(), (unsigned long)(total / 1024));
  if (!segs.empty()) out.printf(" (e%07lu a e%07lu)", (unsigned long)segs.front().seq, (unsigned long)segs.back().seq);
  out.println();
  out.printf("Segmentos de %lu KB%s | se borra lo mas viejo al pasar %d%% de uso | uso actual: %d%%\n",
             (unsigned long)(segBytes / 1024), SDLOG_SEGMENT_BYTES > 0 ? " (fijo)" : " (automatico)",
             SDLOG_MAX_USED_PERCENT, sdUsedPercent());
}
