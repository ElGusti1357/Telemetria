#include "sms_control.h"
#include "config.h"
#include "commands.h"
#include "phonebook.h"
#include "modem.h"
#include "sd_log.h"
#include "alerts.h"
#include "sms_pdu.h"          // smsUtf8Truncate

// =====================================================================
//  Cola de mensajes pendientes
// =====================================================================
struct Msg { String sender; String text; };
static Msg  queue_[SMSCMD_QUEUE];
static int  qHead = 0, qCount = 0;

// =====================================================================
//  Remitentes no autorizados: se anotan a lo sumo una vez por minuto
// =====================================================================
static uint32_t lastUnauthMs = 0;
static bool     haveUnauth = false;
static int      suppressed = 0;

static void noteUnauthorized(const String &sender) {
  uint32_t now = millis();
  if (haveUnauth && now - lastUnauthMs < 60000UL) { suppressed++; return; }
  String d = sender;
  if (suppressed > 0) d += " (+" + String(suppressed) + " mas en el ultimo minuto)";
  suppressed = 0;
  haveUnauth = true;
  lastUnauthMs = now;
  sdLogEvent("SMS_NO_AUTORIZADO", d);
}

// =====================================================================
//  PIN y bloqueo por intentos fallidos
// =====================================================================
static int      pinFails = 0;
static uint32_t pinWindowStart = 0;
static bool     lockActive = false;
static uint32_t lockUntil = 0;

static bool pinLocked(uint32_t now) {
  if (lockActive && (int32_t)(now - lockUntil) >= 0) lockActive = false;     // se cumplio el bloqueo
  return lockActive;
}

static void registerPinFailure(const String &sender, uint32_t now) {
  if (pinFails == 0 || now - pinWindowStart > SMSCMD_PIN_WINDOW_MS) { pinFails = 0; pinWindowStart = now; }
  pinFails++;
  if (pinFails >= SMSCMD_PIN_MAX_FAILS) {
    pinFails = 0;
    lockActive = true;
    lockUntil = now + SMSCMD_PIN_LOCK_MS;
    alertError("SMS: PIN incorrecto repetido (ultimo desde " + sender + "): administracion por SMS bloqueada " +
               String((unsigned long)(SMSCMD_PIN_LOCK_MS / 60000UL)) + " min");
  }
}

// Si el mensaje empieza con el PIN lo quita y pinOk = true. Si empieza con un numero de 4 a 8 digitos
// distinto del PIN, se lo toma como un PIN equivocado. Sin PIN configurado, pinOk = true siempre.
static void stripPin(String &text, bool &pinOk, bool &wrongPin) {
  pinOk = false;
  wrongPin = false;
  if (strlen(SMSCMD_PIN) == 0) { pinOk = true; return; }

  int sp = -1;
  for (size_t i = 0; i < text.length(); i++) {
    if (text[i] == ' ' || text[i] == '\t') { sp = (int)i; break; }
  }
  String tok = sp < 0 ? text : text.substring(0, (size_t)sp);
  String rest = sp < 0 ? String("") : text.substring((size_t)sp + 1);
  rest.trim();

  bool digits = tok.length() > 0;
  for (size_t i = 0; i < tok.length() && digits; i++) if (tok[i] < '0' || tok[i] > '9') digits = false;

  if (tok == SMSCMD_PIN) { pinOk = true; text = rest; }
  else if (digits && tok.length() >= 4 && tok.length() <= 8) { wrongPin = true; text = rest; }
}

// =====================================================================
//  Respuestas: con tope por hora
// =====================================================================
static uint32_t replyTimes[SMSCMD_MAX_REPLIES_HOUR];
static int      replyIdx = 0, replyN = 0;
static bool     limitLogged = false;

static bool replyAllowed(uint32_t now) {
  int recent = 0;
  for (int i = 0; i < replyN; i++) if (now - replyTimes[i] < 3600000UL) recent++;
  return recent < SMSCMD_MAX_REPLIES_HOUR;
}

static void noteReply(uint32_t now) {
  replyTimes[replyIdx] = now;
  replyIdx = (replyIdx + 1) % SMSCMD_MAX_REPLIES_HOUR;
  if (replyN < SMSCMD_MAX_REPLIES_HOUR) replyN++;
}

static void reply(const String &to, const String &raw) {
  String t;
  for (size_t i = 0; i < raw.length(); i++) if (raw[i] != '\r') t += raw[i];
  t.trim();
  if (t.length() == 0) t = "OK";
  if (t.length() > SMSCMD_REPLY_MAX_CHARS) t = smsUtf8Truncate(t, SMSCMD_REPLY_MAX_CHARS - 3) + "...";

  uint32_t now = millis();
  if (!replyAllowed(now)) {
    if (!limitLogged) {
      limitLogged = true;
      sdLogEvent("SMS_CMD_LIMITE", "tope de " + String(SMSCMD_MAX_REPLIES_HOUR) + " respuestas por hora: no se contesta a " + to);
    }
    return;
  }
  limitLogged = false;
  noteReply(now);
  modemSendSms(to, t);
}

// =====================================================================
//  Procesamiento de un mensaje
// =====================================================================
static void handle(const String &sender, const String &raw) {
  PhoneRole role = phoneRoleOf(sender);
  if (role == PHONE_NONE) return;            // pudo borrarse de la agenda mientras esperaba en la cola

  String text = raw;
  text.trim();
  bool pinOk, wrongPin;
  stripPin(text, pinOk, wrongPin);

  uint32_t now = millis();
  bool locked = pinLocked(now);

  if (wrongPin) {
    if (locked) return;                      // bloqueado: ni siquiera se contesta
    sdLogEvent("SMS_PIN_ERROR", sender);
    registerPinFailure(sender, now);
    reply(sender, "PIN incorrecto");
    return;
  }
  bool pinUsed = pinOk && strlen(SMSCMD_PIN) > 0;
  if (pinUsed && !locked) pinFails = 0;      // PIN correcto: se limpia el contador
  if (locked) pinOk = false;                 // durante el bloqueo ni el PIN correcto habilita administrar

  if (text.length() == 0) text = "AYUDA";    // mando solo el PIN (o un mensaje vacio)

  // Se anota el pedido SIN el PIN
  sdLogEvent("SMS_CMD", sender + ": " + smsUtf8Truncate(text, 60) + (pinUsed ? " [PIN]" : ""));

  StringPrint out;
  CmdContext ctx;
  ctx.source = CMD_FROM_SMS;
  ctx.role = role;
  ctx.pinOk = pinOk;
  ctx.pinLocked = locked;
  ctx.sender = sender;
  cmdExecute(text, out, ctx);
  reply(sender, out.text);
}

// =====================================================================
//  API
// =====================================================================
void smsControlOnReceived(const String &sender, const String &text, bool isPart) {
  if (isPart) return;                        // un comando cabe en un SMS: las partes de mensajes largos se ignoran
  if (phoneRoleOf(sender) == PHONE_NONE) { noteUnauthorized(sender); return; }
  if (qCount >= SMSCMD_QUEUE) {
    sdLogEvent("SMS_CMD_COLA_LLENA", "descartado un mensaje de " + sender);
    return;
  }
  queue_[(qHead + qCount) % SMSCMD_QUEUE] = { sender, text };
  qCount++;
}

void smsControlProcess() {
  if (qCount == 0) return;
  if (!modemIsReady() || !modemIsIdle()) return;     // se espera a que el modem este libre

  Msg m = queue_[qHead];
  qHead = (qHead + 1) % SMSCMD_QUEUE;
  qCount--;
  handle(m.sender, m.text);
}
