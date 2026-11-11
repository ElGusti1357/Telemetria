#include "time_sync.h"
#include "config.h"
#include "gps.h"
#include "modem.h"
#include "ethernet_w5500.h"
#include <sys/time.h>

#define MIN_VALID_EPOCH 1704067200LL      // 2024-01-01: cualquier hora anterior se considera invalida
#define MAX_VALID_EPOCH 4102444800LL      // 2100-01-01

static TimeSyncEventFn _evFn = nullptr;
void timeSyncSetEventHandler(TimeSyncEventFn fn) { _evFn = fn; }

static void emit(const String &event, const String &detail, bool isError) {
  if (_evFn) _evFn(event, detail, isError);
}

// ---------- Estado ----------
static TimeSource lastSource = TIME_SRC_NONE;
static bool       everSynced = false;        // hubo al menos una sincronizacion desde el arranque
static uint32_t   lastSyncMs = 0;
static bool       started = false;           // ya se hizo la primera ronda
static bool       forceNow = false;
static uint32_t   nextTryMs = 0;
static uint32_t   retryMs = TIMESYNC_RETRY_MIN_MS;
static bool       failReported = false;

const char *timeSourceName(TimeSource s) {
  switch (s) {
    case TIME_SRC_GPS:  return "GPS";
    case TIME_SRC_NTP:  return "NTP";
    case TIME_SRC_CELL: return "red celular";
    default:            return "ninguna";
  }
}

static bool validEpoch(int64_t e) { return e >= MIN_VALID_EPOCH && e < MAX_VALID_EPOCH; }

bool timeSyncIsValid() { return validEpoch((int64_t)time(nullptr)); }

void timeSyncBegin(const char *posixTz) {
  setenv("TZ", posixTz, 1);
  tzset();
}

// ---------- Metodos ----------
// Cada uno devuelve true y completa sec/usec (UTC) si obtuvo una hora valida; si no, 'why' dice el motivo.

static bool tryGps(int64_t &sec, uint32_t &usec, String &why) {
  GpsTime t;
  if (!gpsGetTime(t)) { why = "sin fix"; return false; }
  int64_t e = (int64_t)gpsTimeToEpoch(t, 0);
  if (!validEpoch(e)) { why = "hora no valida"; return false; }
  sec = e;
  usec = 0;                                  // el GPS entrega segundos enteros
  return true;
}

static bool tryNtp(int64_t &sec, uint32_t &usec, String &why) {
  if (!w5500IsReady()) { why = "Ethernet sin red"; return false; }
  static const char *const servers[] = TIMESYNC_NTP_SERVERS;
  for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
    time_t s;
    uint32_t u;
    if (w5500NtpGetTime(servers[i], s, u) && validEpoch((int64_t)s)) {
      sec = (int64_t)s;
      usec = u;
      return true;
    }
  }
  why = "ningun servidor NTP respondio";
  return false;
}

// "aa/mm/dd,hh:mm:ss+zz" (zz en cuartos de hora, es la hora LOCAL de la red) -> epoch UTC
static bool parseCclk(const String &raw, int64_t &utc) {
  int yy, mo, dd, hh, mi, ss, tzq = 0;
  char sign = '+';
  int n = sscanf(raw.c_str(), "%d/%d/%d,%d:%d:%d%c%d", &yy, &mo, &dd, &hh, &mi, &ss, &sign, &tzq);
  if (n < 6) return false;
  if (n < 8) { sign = '+'; tzq = 0; }
  if (yy < 0 || yy > 99 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
      hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) return false;

  GpsTime g;
  g.year = 2000 + yy; g.month = mo; g.day = dd; g.hour = hh; g.minute = mi; g.second = ss;
  int64_t local = (int64_t)gpsTimeToEpoch(g, 0);                 // la hora local, leida como si fuera UTC
  int64_t offset = (sign == '-' ? -1 : 1) * (int64_t)tzq * 900;  // desfase de la red respecto de UTC
  utc = local - offset;
  return validEpoch(utc);
}

static bool tryCell(int64_t &sec, uint32_t &usec, String &why) {
  if (!modemIsReady()) { why = "modem no listo"; return false; }
  String raw;
  if (!modemGetNetworkTime(raw)) { why = "el modem no entrego la hora"; return false; }
  int64_t utc;
  if (!parseCclk(raw, utc)) { why = "la red no informo una hora valida (" + raw + ")"; return false; }
  sec = utc;
  usec = 0;
  return true;
}

// Prueba los metodos en el orden de TIMESYNC_ORDER hasta que uno funcione
static TimeSource runRound(int64_t &sec, uint32_t &usec) {
  for (const char *o = TIMESYNC_ORDER; *o; o++) {
    TimeSource src;
    bool ok;
    String why;
    switch (*o) {
      case 'G': case 'g': src = TIME_SRC_GPS;  ok = tryGps(sec, usec, why);  break;
      case 'N': case 'n': src = TIME_SRC_NTP;  ok = tryNtp(sec, usec, why);  break;
      case 'C': case 'c': src = TIME_SRC_CELL; ok = tryCell(sec, usec, why); break;
      default: continue;
    }
    if (ok) return src;
    Serial.printf("[hora] %s: %s\n", timeSourceName(src), why.c_str());
  }
  return TIME_SRC_NONE;
}

static void applyTime(int64_t sec, uint32_t usec, TimeSource src) {
  int64_t before = (int64_t)time(nullptr);
  bool hadValid = validEpoch(before);

  struct timeval tv;
  tv.tv_sec = (time_t)sec;
  tv.tv_usec = (suseconds_t)usec;
  settimeofday(&tv, nullptr);

  bool first = !everSynced;
  everSynced = true;
  lastSource = src;
  lastSyncMs = millis();

  String detail = String("fuente=") + timeSourceName(src);
  if (hadValid) {
    char buf[24];
    snprintf(buf, sizeof(buf), " ajuste=%+lds", (long)(sec - before));   // cuanto se corrigio el reloj
    detail += buf;
  }
  emit(first ? "HORA_SYNC" : "HORA_RESYNC", detail, false);
}

// ---------- Tarea periodica ----------
void timeSyncTask(bool portIdle) {
  uint32_t now = millis();
  if (started && !forceNow && (int32_t)(now - nextTryMs) < 0) return;   // todavia no toca
  if (!portIdle) return;                       // modem ocupado: se reintenta en la proxima vuelta

  started = true;
  forceNow = false;

  int64_t sec = 0;
  uint32_t usec = 0;
  TimeSource src = runRound(sec, usec);

  if (src != TIME_SRC_NONE) {
    applyTime(sec, usec, src);
    retryMs = TIMESYNC_RETRY_MIN_MS;
    nextTryMs = millis() + TIMESYNC_PERIOD_MS;
    failReported = false;
    return;
  }

  // Fallaron los tres: reintentar cada vez mas espaciado
  now = millis();
  nextTryMs = now + retryMs;
  retryMs = (retryMs >= TIMESYNC_RETRY_MAX_MS / 2) ? TIMESYNC_RETRY_MAX_MS : retryMs * 2;

  uint32_t late  = everSynced ? (now - lastSyncMs) : now;                       // tiempo sin sincronizar
  uint32_t limit = (everSynced ? TIMESYNC_PERIOD_MS : 0) + TIMESYNC_ALERT_AFTER_MS;
  if (!failReported && late >= limit) {
    failReported = true;
    emit("HORA_SIN_SINCRONIZAR",
         String("no se pudo obtener la hora por ") + TIMESYNC_ORDER + " (GPS=G, NTP=N, red celular=C)", true);
  }
}

void timeSyncNow() { forceNow = true; }

TimeSource timeSyncLastSource() { return lastSource; }

uint32_t timeSyncAgeSeconds() {
  return everSynced ? (millis() - lastSyncMs) / 1000 : 0xFFFFFFFFUL;
}

void timeSyncPrintStatus(Print &out) {
  time_t now = time(nullptr);
  if (timeSyncIsValid()) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    out.printf("Hora local: %s\n", buf);
  } else {
    out.println("Hora local: SIN FECHA VALIDA (sin sincronizar)");
  }

  if (everSynced) {
    uint32_t age = timeSyncAgeSeconds();
    out.printf("Ultima sincronizacion: %s, hace %lu min\n", timeSourceName(lastSource), (unsigned long)(age / 60));
  } else {
    out.println("Ultima sincronizacion: ninguna desde el arranque");
  }

  int32_t wait = started ? (int32_t)(nextTryMs - millis()) / 1000 : 0;
  out.printf("Orden de metodos: %s | proximo intento en %ld s\n", TIMESYNC_ORDER, (long)(wait > 0 ? wait : 0));
}
