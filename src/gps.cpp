#include "gps.h"

static Stream *_at = nullptr;

// Envía "AT" + cmd (cmd ej: "+CGNSSINFO") y acumula la respuesta.
// Devuelve true si el módem respondió OK.
static bool sendAT(const String &cmd, String &resp, uint32_t timeoutMs = 2000) {
  if (!_at) return false;
  resp = "";
  _at->print("AT" + cmd + "\r\n");
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (_at->available()) resp += (char)_at->read();
    if (resp.indexOf("\r\nOK\r\n") >= 0 || resp.endsWith("OK\r\n")) return true;
    if (resp.indexOf("ERROR") >= 0) return false;
    delay(5);
  }
  return false;
}

// Campo n (0,1,2...) de una cadena separada por comas
static String field(const String &s, int n) {
  int start = 0;
  for (int i = 0; i < n; i++) {
    int c = s.indexOf(',', start);
    if (c < 0) return "";
    start = c + 1;
  }
  int e = s.indexOf(',', start);
  String f = s.substring(start, e < 0 ? s.length() : e);
  f.trim();
  return f;
}

// ddmm.mmmmmm -> grados decimales
static double nmeaToDegrees(const String &raw) {
  double v = raw.toDouble();
  int deg = (int)(v / 100.0);
  return deg + (v - deg * 100.0) / 60.0;
}

// Lee y parsea AT+CGNSSINFO. Respuesta:
// +CGNSSINFO: <mode>,<GPS-SVs>,<GLONASS-SVs>,<BEIDOU-SVs>,<lat>,<N/S>,<lon>,<E/W>,
//             <date ddmmyy>,<time hhmmss.s>,<alt>,<speed knots>,<course>,<PDOP>,<HDOP>,<VDOP>
static bool readInfo(GpsPosition *pos, GpsTime *t) {
  String resp;
  if (!sendAT("+CGNSSINFO", resp, 3000)) return false;

  int i = resp.indexOf("+CGNSSINFO:");
  if (i < 0) return false;
  int e = resp.indexOf('\n', i);
  String p = resp.substring(i + 11, e < 0 ? resp.length() : e);
  p.trim();

  // Sin fix: campos de posición vacíos
  String latS = field(p, 4), lonS = field(p, 6);
  String dateS = field(p, 8), timeS = field(p, 9);
  if (latS.length() == 0 || lonS.length() == 0 || dateS.length() < 6 || timeS.length() < 6) return false;

  if (pos) {
    pos->latitude  = nmeaToDegrees(latS);
    if (field(p, 5) == "S") pos->latitude = -pos->latitude;
    pos->longitude = nmeaToDegrees(lonS);
    if (field(p, 7) == "W") pos->longitude = -pos->longitude;
    pos->altitude   = field(p, 10).toFloat();
    pos->speedKmh   = field(p, 11).toFloat() * 1.852f;   // nudos -> km/h
    pos->course     = field(p, 12).toFloat();
    pos->hdop       = field(p, 14).toFloat();
    pos->satellites = field(p, 1).toInt() + field(p, 2).toInt() + field(p, 3).toInt();
  }
  if (t) {
    t->day    = dateS.substring(0, 2).toInt();
    t->month  = dateS.substring(2, 4).toInt();
    t->year   = 2000 + dateS.substring(4, 6).toInt();
    t->hour   = timeS.substring(0, 2).toInt();
    t->minute = timeS.substring(2, 4).toInt();
    t->second = timeS.substring(4, 6).toInt();
  }
  return true;
}

bool gpsBegin(Stream &at) {
  _at = &at;
  String resp;

  // ¿Ya está encendido?
  if (sendAT("+CGNSSPWR?", resp) && resp.indexOf("+CGNSSPWR: 1") >= 0) return true;

  if (!sendAT("+CGNSSPWR=1", resp, 3000)) {
    // Algunos firmwares usan +CGPS
    if (!sendAT("+CGPS=1", resp, 3000)) return false;
  }
  delay(500);   // el GNSS tarda un instante en arrancar
  return true;
}

void gpsEnd() {
  String resp;
  if (!sendAT("+CGNSSPWR=0", resp)) sendAT("+CGPS=0", resp);
}

bool gpsGetPosition(GpsPosition &pos) { return readInfo(&pos, nullptr); }
bool gpsGetTime(GpsTime &t)           { return readInfo(nullptr, &t); }
bool gpsGetFix(GpsPosition &pos, GpsTime &t) { return readInfo(&pos, &t); }

bool gpsWaitForFix(uint32_t timeoutMs, GpsPosition *pos, GpsTime *t) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (readInfo(pos, t)) return true;
    delay(1000);
  }
  return false;
}

time_t gpsTimeToEpoch(const GpsTime &t, int offsetSeconds) {
  // Algoritmo de días desde 1970-01-01 (calendario gregoriano)
  int y = t.year - (t.month <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (t.month + (t.month > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (time_t)(days * 86400L + t.hour * 3600L + t.minute * 60L + t.second + offsetSeconds);
}

String gpsTimeToString(const GpsTime &t, int offsetSeconds) {
  time_t e = gpsTimeToEpoch(t, offsetSeconds);
  struct tm tmv;
  gmtime_r(&e, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(buf);
}
