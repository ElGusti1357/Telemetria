#include "alerts.h"
#include "config.h"
#include "modem.h"
#include "sd_log.h"

// ---------- Cola de errores pendientes ----------
struct AlertItem {
  String msg;     // mensaje limpio (sirve para detectar repetidos)
  String full;    // "[fecha hora] mensaje"
};

static AlertItem pending[ALERT_QUEUE_MAX];
static int       pendingCount = 0;

// Errores enviados hace poco (para no repetirlos)
struct RecentItem { String msg; uint32_t ms; bool used; };
static RecentItem recent[ALERT_QUEUE_MAX];

static String   _dest;
static uint32_t lastSendMs = 0;
static bool     haveSent   = false;

// ---------- Memoria RTC: sobrevive a ESP.restart() ----------
#define RTC_MAGIC 0xA1E27001UL
static RTC_NOINIT_ATTR uint32_t rtcMagic;
static RTC_NOINIT_ATTR char     rtcPending[ALERT_MAX_SMS_CHARS + 1];

// El SMS usa el juego de caracteres GSM: solo se permite ASCII imprimible
static String toAscii(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\r' || c == '\n') out += ' ';
    else if (c < 32 || c > 126) out += '?';
    else out += (char)c;
  }
  return out;
}

static bool wasSentRecently(const String &msg) {
  for (int i = 0; i < ALERT_QUEUE_MAX; i++) {
    if (recent[i].used && recent[i].msg == msg && millis() - recent[i].ms < ALERT_DEDUP_MS) return true;
  }
  return false;
}

static void markSent(const String &msg) {
  // Usa un lugar libre; si no hay, reemplaza el registro de mayor antiguedad
  int slot = 0;
  uint32_t maxAge = 0;
  for (int i = 0; i < ALERT_QUEUE_MAX; i++) {
    if (!recent[i].used) { slot = i; break; }
    uint32_t age = millis() - recent[i].ms;
    if (age > maxAge) { maxAge = age; slot = i; }
  }
  recent[slot].msg  = msg;
  recent[slot].ms   = millis();
  recent[slot].used = true;
}

static void enqueue(const String &rawMsg) {
  String msg = toAscii(rawMsg);
  if (wasSentRecently(msg)) return;
  for (int i = 0; i < pendingCount; i++) {
    if (pending[i].msg == msg) return;                 // ya esta en la cola
  }
  if (pendingCount >= ALERT_QUEUE_MAX) {               // cola llena: descartar el mas viejo
    for (int i = 1; i < pendingCount; i++) pending[i - 1] = pending[i];
    pendingCount--;
  }
  pending[pendingCount].msg  = msg;
  pending[pendingCount].full = "[" + sdLogNow() + "] " + msg;
  pendingCount++;
}

// Arma el texto del SMS con los errores que entren y envia. Devuelve true si se envio.
static bool sendPending() {
  if (pendingCount == 0 || _dest.length() == 0) return false;

  String text = "ALERTA: ";
  int used = 0;
  for (int i = 0; i < pendingCount; i++) {
    String add = (used ? " | " : "") + pending[i].full;
    if (text.length() + add.length() > ALERT_MAX_SMS_CHARS) {
      if (used > 0) break;                              // el resto va en el proximo SMS
      add = add.substring(0, ALERT_MAX_SMS_CHARS - text.length());
    }
    text += add;
    used++;
  }

  bool ok = modemSendSms(_dest, text);
  lastSendMs = millis();
  haveSent = true;

  if (ok) {
    for (int i = 0; i < used; i++) markSent(pending[i].msg);
    for (int i = used; i < pendingCount; i++) pending[i - used] = pending[i];
    pendingCount -= used;
  }
  return ok;
}

// Guarda los errores pendientes en memoria RTC para enviarlos tras el reinicio
// El error fatal (fatalClean) se guarda PRIMERO y, si no entra completo, se trunca:
// es el motivo del reinicio y el que mas importa conservar.
static void savePendingToRtc(const String &fatalClean) {
  const int budget = ALERT_MAX_SMS_CHARS - 20;     // deja lugar al prefijo "Reinicio previo: "
  String joined;

  for (int i = 0; i < pendingCount; i++) {
    if (pending[i].msg == fatalClean) {
      joined = pending[i].full;
      if ((int)joined.length() > budget) joined = joined.substring(0, budget);
      break;
    }
  }
  for (int i = 0; i < pendingCount; i++) {
    if (pending[i].msg == fatalClean) continue;    // ya esta al principio
    String add = (joined.length() ? " | " : "") + pending[i].full;
    if ((int)(joined.length() + add.length()) > budget) break;
    joined += add;
  }
  if (joined.length() == 0) return;

  memset(rtcPending, 0, sizeof(rtcPending));
  joined.toCharArray(rtcPending, sizeof(rtcPending));
  rtcMagic = RTC_MAGIC;
}

// ---------- API ----------
void alertsBegin(const String &destination) {
  _dest = destination;

  // ¿Quedo un error pendiente de antes del reinicio?
  if (rtcMagic == RTC_MAGIC) {
    rtcPending[sizeof(rtcPending) - 1] = 0;
    String prev(rtcPending);
    rtcMagic = 0;
    if (prev.length() > 0) {
      sdLogEvent("REINICIO_PREVIO", prev);
      enqueue("Reinicio previo: " + prev);
    }
  }
}

void alertError(const String &msg) {
  sdLogEvent("ERROR", msg);
  enqueue(msg);
}

void alertFatal(const String &msg) {
  Serial.printf("\nERROR FATAL: %s\n", msg.c_str());
  sdLogEvent("ERROR_FATAL", msg);
  enqueue(msg);

  // Si el modem esta operativo, intentar avisar ya mismo (bloqueante)
  if (modemIsReady()) sendPending();

  // Lo que no se pudo enviar queda guardado para despues del reinicio
  if (pendingCount > 0) savePendingToRtc(toAscii(msg));

  Serial.printf("Reiniciando en %d s...\n", RESTART_WAIT_SEC);
  delay(RESTART_WAIT_SEC * 1000UL);
  ESP.restart();
}

void alertsProcess() {
  if (pendingCount == 0) return;
  if (!modemIsReady() || !modemIsIdle()) return;
  if (haveSent && millis() - lastSendMs < ALERT_MIN_INTERVAL_MS) return;
  sendPending();
}
