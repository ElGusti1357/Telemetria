#pragma once
/*
  Interprete de comandos, compartido por la consola (puerto serie) y los SMS.

  Cada comando escribe su respuesta en un Print: en la consola es Serial; por SMS es un texto
  (StringPrint) que luego se manda de vuelta al remitente. Cuando el pedido viene por SMS las
  respuestas son mas compactas.

  Niveles de permiso de cada comando:
    CMD_ANY      cualquier numero autorizado (y la consola)
    CMD_ADMIN    solo un administrador; por SMS ademas el mensaje debe empezar con el PIN
    CMD_CONSOLE  solo por la consola (puerto serie): comandos AT crudos, envio libre de SMS, etc.
*/
#include <Arduino.h>
#include "phonebook.h"

enum CmdSource { CMD_FROM_CONSOLE, CMD_FROM_SMS };

struct CmdContext {
  CmdSource source = CMD_FROM_CONSOLE;
  PhoneRole role = PHONE_ADMIN;      // por SMS: rol del remitente (la consola es siempre administrador)
  bool      pinOk = true;            // por SMS: el mensaje traia el PIN correcto
  bool      pinLocked = false;       // por SMS: la administracion esta bloqueada por PIN incorrectos
  String    sender;                  // por SMS: numero del remitente
};

// Print que acumula lo escrito en un String (para armar la respuesta de un SMS)
class StringPrint : public Print {
 public:
  String text;
  size_t write(uint8_t c) override { text += (char)c; return 1; }
  size_t write(const uint8_t *buf, size_t n) override {
    for (size_t i = 0; i < n; i++) text += (char)buf[i];
    return n;
  }
};

// Ejecuta una linea de comando y escribe la respuesta en 'out'.
// Devuelve false si el comando no existe (igual escribe un mensaje de ayuda).
bool cmdExecute(const String &line, Print &out, const CmdContext &ctx);

// true (una sola vez) si algun comando pidio reiniciar el equipo. main.cpp reinicia despues de responder.
bool cmdRestartPending();
