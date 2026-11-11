#include "commands.h"
#include "config.h"
#include "modem.h"
#include "gps.h"
#include "time_sync.h"
#include "sd_log.h"
#include "alerts.h"
#include "io_expander.h"
#include "ethernet_w5500.h"
#include "sms_pdu.h"          // smsUtf8Truncate
#include <vector>

// =====================================================================
//  Utilidades
// =====================================================================
static bool restartRequested = false;

bool cmdRestartPending() {
  bool r = restartRequested;
  restartRequested = false;
  return r;
}

static bool isSms(const CmdContext &c) { return c.source == CMD_FROM_SMS; }
static String who(const CmdContext &c) { return isSms(c) ? c.sender : String("consola"); }

// Primera palabra de s; el resto (sin espacios de los extremos) queda en rest
static String firstToken(const String &s, String &rest) {
  String t = s;
  t.trim();
  int sp = -1;
  for (size_t i = 0; i < t.length(); i++) {
    if (t[i] == ' ' || t[i] == '\t') { sp = (int)i; break; }
  }
  if (sp < 0) { rest = ""; return t; }
  rest = t.substring(sp + 1);
  rest.trim();
  return t.substring(0, sp);
}

static String upper(String s) { s.toUpperCase(); return s; }

// Bits de una mascara como texto, en grupos de 5 ("00010 00000 1"): el primer caracter es la 1
static String bitsText(uint64_t mask, int n) {
  String s;
  for (int i = 0; i < n; i++) {
    if (i > 0 && i % 5 == 0) s += ' ';
    s += ((mask >> i) & 1) ? '1' : '0';
  }
  return s;
}

static int popcount64(uint64_t v) { return __builtin_popcountll(v); }

static const char *ethStateText(W5500State s) {
  switch (s) {
    case W5500_NO_HARDWARE: return "chip no detectado";
    case W5500_LINK_DOWN:   return "sin cable";
    case W5500_NO_IP:       return "sin IP";
    case W5500_READY:       return "listo";
  }
  return "?";
}

// ---------- Permisos ----------
static bool consoleOnly(const CmdContext &ctx, Print &out) {
  if (ctx.source == CMD_FROM_CONSOLE) return true;
  out.print("Solo por consola\n");
  return false;
}

// Exige administrador y, por SMS, el PIN (la consola siempre puede)
static bool needAdmin(const CmdContext &ctx, Print &out) {
  if (ctx.source == CMD_FROM_CONSOLE) return true;
  if (ctx.role != PHONE_ADMIN) { out.print("Solo administradores\n"); return false; }
  if (!ctx.pinOk) {
    out.print(ctx.pinLocked ? "Administracion bloqueada temporalmente\n" : "Requiere PIN al inicio: <PIN> <comando>\n");
    return false;
  }
  return true;
}

typedef void (*CmdFn)(const String &args, Print &out, const CmdContext &ctx);
enum CmdLevel { CMD_ANY, CMD_ADMIN, CMD_CONSOLE };

// =====================================================================
//  Comandos de consulta (CMD_ANY)
// =====================================================================
static void cmdAyuda(const String &, Print &out, const CmdContext &ctx) {
  if (isSms(ctx)) {
    out.print("ESTADO ENTRADAS SALIDAS GPS HORA LOG [n] SD ETH\n"
              "Admin (PIN al inicio): SALIDA n 0|1, LOGDEL, REINICIAR, HORA SYNC, NUM LISTA|AGREGAR n [ADMIN]|BORRAR n\n");
    return;
  }
  out.print("Comandos (consola y SMS):\n"
            "  ESTADO | ENTRADAS | SALIDAS | GPS | HORA | LOG [n] | SD | ETH\n"
            "Administracion (por SMS: administrador + PIN al inicio, ej. \"1234 LOGDEL\"):\n"
            "  SALIDA <n> <0|1> (o OUT) | LOGDEL | REINICIAR | HORA SYNC | NUM LISTA|AGREGAR|BORRAR\n"
            "Solo consola:\n"
            "  IO | EVENTO <txt> | ERROR <txt> | ETH TEST <host> [puerto] | SMS <texto> | AT...\n");
}

static void cmdEstado(const String &, Print &out, const CmdContext &) {
  out.printf("Hora: %s%s\n", sdLogNow().c_str(), timeSyncIsValid() ? "" : " (sin sincronizar)");
  int sq = modemSignalQuality();
  if (sq >= 0 && sq <= 31) out.printf("Senal modem: %d/31\n", sq);
  else                     out.print("Senal modem: ?\n");
  GpsPosition pos;
  GpsTime t;
  out.print(gpsGetFix(pos, t) ? "GPS: con fix\n" : "GPS: sin fix\n");
  out.printf("Ethernet: %s", ethStateText(w5500State()));
  if (w5500IsReady()) out.printf(" %s", w5500LocalIP().toString().c_str());
  out.print("\n");
  if (sdIsReady()) out.printf("SD: %d%% usada\n", sdUsedPercent());
  else             out.print("SD: NO DISPONIBLE\n");
  out.printf("Entradas activas: %d/%u | Salidas activas: %d/%u\n",
             popcount64(ioReadInputs()), (unsigned)ioInputCount(),
             popcount64(ioReadOutputs()), (unsigned)ioOutputCount());
}

static void cmdEntradas(const String &, Print &out, const CmdContext &) {
  out.printf("Entradas (1..%u): %s\n", (unsigned)ioInputCount(), bitsText(ioReadInputs(), ioInputCount()).c_str());
}

static void cmdSalidas(const String &, Print &out, const CmdContext &) {
  out.printf("Salidas (1..%u): %s\n", (unsigned)ioOutputCount(), bitsText(ioReadOutputs(), ioOutputCount()).c_str());
}

static void cmdGps(const String &, Print &out, const CmdContext &ctx) {
  if (!modemIsIdle()) { out.print("Modem ocupado, reintentar\n"); return; }
  GpsPosition pos;
  GpsTime t;
  if (!gpsGetFix(pos, t)) { out.print("Sin fix GPS todavia\n"); return; }
  if (isSms(ctx)) {
    out.printf("GPS %.6f,%.6f | %.0fm | %.0fkm/h | %u sats\n", pos.latitude, pos.longitude,
               pos.altitude, pos.speedKmh, (unsigned)pos.satellites);
    out.printf("https://maps.google.com/?q=%.6f,%.6f\n", pos.latitude, pos.longitude);
    out.printf("Hora %s\n", gpsTimeToString(t, TZ_OFFSET_SEC).c_str());
    return;
  }
  out.printf("Lat: %.6f  Lon: %.6f  Alt: %.1f m\n", pos.latitude, pos.longitude, pos.altitude);
  out.printf("Vel: %.1f km/h  Sats: %u  HDOP: %.1f\n", pos.speedKmh, (unsigned)pos.satellites, pos.hdop);
  out.printf("UTC: %s | Argentina: %s\n", gpsTimeToString(t).c_str(), gpsTimeToString(t, TZ_OFFSET_SEC).c_str());
}

static void cmdHora(const String &args, Print &out, const CmdContext &ctx) {
  if (upper(args) == "SYNC") {
    if (!needAdmin(ctx, out)) return;
    timeSyncNow();
    out.print("Sincronizacion de hora solicitada (se hace en la proxima vuelta)\n");
    return;
  }
  timeSyncPrintStatus(out);
}

static void cmdLog(const String &args, Print &out, const CmdContext &ctx) {
  if (!sdIsReady()) { out.print("SD no disponible\n"); return; }
  String tail;
  if (!isSms(ctx)) {
    if (sdLogReadTail(tail, 3000)) {
      out.print("----- ultimos eventos -----\n");
      out.print(tail);
      out.print("\n---------------------------\n");
    } else {
      out.print("El log esta vacio o no se pudo leer. Probar el comando SD\n");
    }
    return;
  }

  // Por SMS: solo las ultimas n lineas (3 por defecto, maximo 10), recortadas
  int n = args.toInt();
  if (n < 1) n = 3;
  if (n > 10) n = 10;
  if (!sdLogReadTail(tail, 2500)) { out.print("Log vacio\n"); return; }
  std::vector<String> lines;
  size_t p = 0;
  while (p < tail.length()) {
    int e = tail.indexOf('\n', p);
    if (e < 0) e = (int)tail.length();
    String l = tail.substring(p, (size_t)e);
    l.trim();
    if (l.length() > 0 && !l.startsWith("fecha_hora;")) lines.push_back(l);
    p = (size_t)e + 1;
  }
  size_t from = lines.size() > (size_t)n ? lines.size() - (size_t)n : 0;
  for (size_t i = from; i < lines.size(); i++) out.printf("%s\n", smsUtf8Truncate(lines[i], 90).c_str());
}

static void cmdSd(const String &, Print &out, const CmdContext &ctx) {
  if (!sdIsReady()) { out.print("SD no disponible\n"); return; }
  uint32_t total, used;
  sdInfo(total, used);
  if (isSms(ctx)) {
    out.printf("SD: %d%% usada (%lu de %lu MB)\n", sdUsedPercent(), (unsigned long)used, (unsigned long)total);
    return;
  }
  out.printf("SD OK: %lu MB usados de %lu MB\n", (unsigned long)used, (unsigned long)total);
  sdLogPrintStatus(out);

  // Prueba real: escribir y leer un archivo
  bool w = sdWriteFile("/test.txt", "prueba SD " + String(millis()));
  String back;
  bool r = sdReadFile("/test.txt", back, 100);
  back.trim();
  out.printf("Escritura: %s | Lectura: %s (\"%s\")\n", w ? "OK" : "FALLO", r ? "OK" : "FALLO", back.c_str());
  sdDeleteFile("/test.txt");
}

static void cmdEth(const String &args, Print &out, const CmdContext &ctx) {
  String rest;
  String sub = upper(firstToken(args, rest));
  if (sub == "TEST") {
    if (!consoleOnly(ctx, out)) return;
    char host[64] = "";
    int port = 80;
    // 'rest' conserva mayusculas/minusculas; sin argumentos no hay nada que leer
    int n = sscanf(rest.c_str(), "%63s %d", host, &port);
    if (n >= 1 && port > 0 && port < 65536) {
      out.printf("Probando TCP a %s:%d ...\n", host, port);
      bool ok = w5500TestTcp(host, (uint16_t)port);
      out.print(ok ? "Conexion OK\n" : "Fallo la conexion (o la red no esta lista)\n");
    } else {
      out.print("Uso: ETH TEST <host> [puerto]\n");
    }
    return;
  }
  if (isSms(ctx)) {
    out.printf("Ethernet: %s", ethStateText(w5500State()));
    if (w5500IsReady()) out.printf(" %s", w5500LocalIP().toString().c_str());
    out.print("\n");
    return;
  }
  w5500PrintStatus(out);
}

// =====================================================================
//  Comandos de administracion (CMD_ADMIN)
// =====================================================================
static void cmdSalida(const String &args, Print &out, const CmdContext &ctx) {
  int n = 0, v = -1;
  if (sscanf(args.c_str(), "%d %d", &n, &v) == 2 && (v == 0 || v == 1) && n >= 1 && n <= 255) {
    bool ok = ioWriteOutput((uint8_t)n, v == 1);
    out.printf("Salida %d = %d: %s\n", n, v, ok ? "OK" : "FALLO (numero invalido o chip sin respuesta)");
    if (ok) sdLogEvent("SALIDA", "OUT" + String(n) + "=" + String(v) + " por " + who(ctx));
  } else {
    out.printf("Uso: SALIDA <n> <0|1>   (n = 1..%u)\n", (unsigned)ioOutputCount());
  }
}

static void cmdLogDel(const String &, Print &out, const CmdContext &ctx) {
  bool ok = sdLogClear();
  out.print(ok ? "Log borrado (todos los segmentos)\n" : "No se pudo borrar todo el log\n");
  if (ok) sdLogEvent("LOG_BORRADO", "por " + who(ctx));      // queda constancia de quien lo borro
}

static void cmdReiniciar(const String &, Print &out, const CmdContext &ctx) {
  sdLogEvent("REINICIO_REMOTO", "pedido por " + who(ctx));
  restartRequested = true;
  out.print("Reiniciando...\n");
}

static void cmdNum(const String &args, Print &out, const CmdContext &ctx) {
  String rest;
  String sub = upper(firstToken(args, rest));
  bool protect = isSms(ctx);                 // por SMS no se permite dejar la agenda sin administradores

  if (sub == "" || sub == "LISTA" || sub == "LIST") { phoneList(out); return; }

  if (sub == "AGREGAR" || sub == "ADD") {
    String roleTxt;
    String num = firstToken(rest, roleTxt);
    roleTxt = upper(roleTxt);
    PhoneRole role = PHONE_USER;
    if (roleTxt == "ADMIN") role = PHONE_ADMIN;
    else if (roleTxt != "" && roleTxt != "USUARIO" && roleTxt != "USER") { out.print("Rol: ADMIN o USUARIO\n"); return; }
    String msg;
    bool ok = phoneAdd(num, role, msg, protect);
    out.printf("%s\n", msg.c_str());
    if (ok) sdLogEvent("NUMEROS", msg + " (por " + who(ctx) + ")");
    return;
  }

  if (sub == "BORRAR" || sub == "DEL" || sub == "ELIMINAR") {
    String msg;
    bool ok = phoneRemove(rest, msg, protect);
    out.printf("%s\n", msg.c_str());
    if (ok) sdLogEvent("NUMEROS", msg + " (por " + who(ctx) + ")");
    return;
  }

  out.print("Uso: NUM LISTA | NUM AGREGAR <numero> [ADMIN|USUARIO] | NUM BORRAR <numero|posicion>\n");
}

// =====================================================================
//  Comandos solo de consola (CMD_CONSOLE)
// =====================================================================
static void cmdIo(const String &, Print &out, const CmdContext &) { ioPrintStatus(out); }

static void cmdEvento(const String &args, Print &out, const CmdContext &) {
  if (args.length() == 0) { out.print("Uso: EVENTO <texto>\n"); return; }
  sdLogEvent("MANUAL", args);
}

static void cmdError(const String &args, Print &out, const CmdContext &) {
  if (args.length() == 0) { out.print("Uso: ERROR <texto>\n"); return; }
  alertError(args);
}

static void cmdSmsSend(const String &args, Print &out, const CmdContext &) {
  if (args.length() == 0) { out.print("Uso: SMS <texto>\n"); return; }
  if (!modemIsIdle()) { out.print("Modem ocupado, reintentar\n"); return; }
  out.printf("Enviando SMS a %s: %s\n", SMS_DESTINATION, args.c_str());
  bool ok = modemSendSms(SMS_DESTINATION, args);
  out.print(ok ? "SMS enviado OK\n" : "Fallo el envio del SMS\n");
}

// =====================================================================
//  Tabla de comandos y despacho
// =====================================================================
struct CmdDef { const char *name; CmdLevel level; CmdFn fn; };

static const CmdDef CMDS[] = {
  { "AYUDA",     CMD_ANY,     cmdAyuda },
  { "HELP",      CMD_ANY,     cmdAyuda },
  { "?",         CMD_ANY,     cmdAyuda },
  { "ESTADO",    CMD_ANY,     cmdEstado },
  { "STATUS",    CMD_ANY,     cmdEstado },
  { "ENTRADAS",  CMD_ANY,     cmdEntradas },
  { "SALIDAS",   CMD_ANY,     cmdSalidas },
  { "GPS",       CMD_ANY,     cmdGps },
  { "HORA",      CMD_ANY,     cmdHora },
  { "LOG",       CMD_ANY,     cmdLog },
  { "SD",        CMD_ANY,     cmdSd },
  { "ETH",       CMD_ANY,     cmdEth },
  { "RED",       CMD_ANY,     cmdEth },
  { "SALIDA",    CMD_ADMIN,   cmdSalida },
  { "OUT",       CMD_ADMIN,   cmdSalida },
  { "LOGDEL",    CMD_ADMIN,   cmdLogDel },
  { "REINICIAR", CMD_ADMIN,   cmdReiniciar },
  { "RESTART",   CMD_ADMIN,   cmdReiniciar },
  { "NUM",       CMD_ADMIN,   cmdNum },
  { "IO",        CMD_CONSOLE, cmdIo },
  { "EVENTO",    CMD_CONSOLE, cmdEvento },
  { "ERROR",     CMD_CONSOLE, cmdError },
  { "SMS",       CMD_CONSOLE, cmdSmsSend },
};

static bool levelOk(CmdLevel level, const CmdContext &ctx, Print &out) {
  if (ctx.source == CMD_FROM_CONSOLE) return true;
  switch (level) {
    case CMD_ANY:     return ctx.role != PHONE_NONE;
    case CMD_ADMIN:   return needAdmin(ctx, out);
    case CMD_CONSOLE: return consoleOnly(ctx, out);
  }
  return false;
}

bool cmdExecute(const String &lineIn, Print &out, const CmdContext &ctx) {
  String line = lineIn;
  line.trim();
  if (line.length() == 0) return true;

  // Comando AT crudo al modem: solo por consola (por SMS podria reconfigurar el modem)
  if (upper(line).startsWith("AT")) {
    if (consoleOnly(ctx, out)) modemSendRawAT(line);
    return true;
  }

  String args;
  String name = upper(firstToken(line, args));
  for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
    if (name == CMDS[i].name) {
      if (!levelOk(CMDS[i].level, ctx, out)) return true;      // ya explico el motivo
      CMDS[i].fn(args, out, ctx);
      return true;
    }
  }
  out.printf("Comando desconocido: %s\nEnvie AYUDA\n", name.substring(0, 20).c_str());
  return false;
}
