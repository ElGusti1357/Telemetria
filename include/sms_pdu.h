#pragma once
/*
  Codec de SMS en modo PDU (3GPP TS 23.040 / 23.038). No toca el hardware.

  Envio: elige solo la codificacion, igual que un celular:
    - GSM de 7 bits si TODO el texto entra (incluye n con tilde, abrir/cerrar interrogacion y
      exclamacion, vocales con acento grave, @ $ _ y, con 2 septetos, ^ { } \ [ ] ~ | euro)
    - UCS2 (UTF-16) si no entra (por ejemplo o/a/i/u con acento agudo, emojis)
  Los textos largos se parten en varias partes concatenadas (153 septetos / 67 caracteres UCS2).

  Recepcion: decodifica GSM7, UCS2 y 8 bits, con o sin encabezado de concatenacion.

  Todo el texto de la API es UTF-8.
*/
#include <Arduino.h>
#include <vector>

enum SmsEncoding { SMS_ENC_GSM7, SMS_ENC_UCS2 };

#define SMS_MAX_PARTS 10      // maximo de partes de un mensaje largo

struct SmsPart {
  String   pdu;       // PDU completa en hexadecimal (empieza con "00" = SMSC por defecto)
  uint16_t tpduLen;   // valor que se pasa a AT+CMGS= (largo del TPDU, sin el byte del SMSC)
};

// Arma las PDU de envio. Devuelve false si el destino o el texto no son validos,
// o si el texto necesita mas de SMS_MAX_PARTS partes.
bool smsBuildSubmit(const String &dest, const String &textUtf8,
                    std::vector<SmsPart> &parts, SmsEncoding &enc, uint8_t concatRef);

struct SmsDeliver {
  String  sender;       // "+5491154052645" o remitente alfanumerico
  String  timestamp;    // "aa/mm/dd,hh:mm:ss+zz" (zz en cuartos de hora, como en modo texto)
  String  text;         // UTF-8
  bool    isPart;       // true si es una parte de un mensaje largo
  uint16_t concatRef;
  uint8_t partNo, partTotal;
};

// Decodifica una PDU recibida (+CMT) en hexadecimal. Devuelve false si no es un SMS-DELIVER valido.
bool smsParseDeliver(const String &hexPdu, SmsDeliver &out);

// ---------- Utilidades de texto ----------
// Recorta a maxBytes sin cortar un caracter UTF-8 a la mitad
String smsUtf8Truncate(const String &s, size_t maxBytes);
// Reemplaza acentos/n con tilde por letras simples y lo demas por [U+XXXX] (para consolas sin UTF-8)
String smsUtf8ToAscii(const String &s);
