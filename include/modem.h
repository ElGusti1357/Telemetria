#pragma once
/*
  Modulo del modem A7670: arranque, SIM, red, SMS (envio y recepcion).
  No conoce la SD ni las alertas: avisa de lo que pasa mediante un
  "manejador de eventos" que se registra con modemSetEventHandler().
*/
#include <Arduino.h>

enum ModemStatus {
  MODEM_STATUS_OK = 0,
  MODEM_ERR_NO_AT,         // el modem no responde a comandos AT
  MODEM_ERR_SIM,           // SIM ausente, bloqueada o PIN incorrecto
  MODEM_ERR_REG_DENIED,    // registro rechazado por la red
  MODEM_ERR_REG_TIMEOUT    // no se registro en el tiempo limite
};

// event: nombre corto (ej "SMS_RX"), detail: texto, isError: true si es un error
typedef void (*ModemEventFn)(const String &event, const String &detail, bool isError);

void modemSetEventHandler(ModemEventFn fn);

// Se llama por cada SMS recibido y decodificado (texto en UTF-8). isPart = es una parte de un mensaje largo.
// El manejador debe ser rapido y NO enviar SMS ni consultar al modem desde aqui (esta dentro de la lectura
// del puerto): lo habitual es encolar el mensaje y procesarlo despues desde loop().
typedef void (*ModemSmsFn)(const String &sender, const String &text, bool isPart);
void modemSetSmsHandler(ModemSmsFn fn);

// Enciende el modem, espera SIM y red y configura los SMS. Con limites de tiempo.
ModemStatus modemInit();
const char *modemStatusText(ModemStatus s);

bool modemIsReady();                 // true si modemInit() termino bien
int  modemSignalQuality();

// Envia un SMS. El texto va en UTF-8 (acentos, ñ, emojis). La codificacion se elige sola,
// como en un celular: GSM de 7 bits si todo el texto entra (la ñ entra), UCS2 si no (á, é, ó, emojis).
// Los textos largos se parten en varias partes (hasta SMS_MAX_PARTS).
// Emite el evento SMS_TX o SMS_TX_FALLO.
bool modemSendSms(const String &dest, const String &text);

// Llamar en cada vuelta de loop(): lee el modem y procesa SMS entrantes
void modemProcess();

// true si no hay datos a medio recibir (es seguro consultar al modem por AT)
bool modemIsIdle();

// Hora que la red celular le informo al modem (NITZ), tal como la devuelve AT+CCLK?:
// "aa/mm/dd,hh:mm:ss+zz" (zz = zona horaria en cuartos de hora). Devuelve false si no la entrega.
// Si la red no informo la hora, el modem devuelve una fecha por defecto (anio < 2024):
// quien la use debe validarla.
bool modemGetNetworkTime(String &raw);

// Manda un comando AT tal cual (para la consola)
void modemSendRawAT(const String &cmd);

// Puerto serie del modem (lo usa el modulo GPS)
Stream &modemSerial();
