#pragma once
/*
  Sincronizacion de fecha y hora del ESP32 con tres metodos, en el orden de TIMESYNC_ORDER:
    G = GPS                       (modulo gps)
    N = NTP por internet          (modulo ethernet_w5500)
    C = hora de la red celular    (modulo modem, AT+CCLK? / NITZ)
  Si un metodo falla se pasa al siguiente, hasta lograr sincronizar.

  Politica de tiempos:
    - Tras una sincronizacion exitosa, la proxima ronda es en TIMESYNC_PERIOD_MS (12 h).
    - Si fallan los tres metodos, reintenta a TIMESYNC_RETRY_MIN_MS duplicando el intervalo
      hasta TIMESYNC_RETRY_MAX_MS, y sigue asi hasta lograrlo.
    - Si pasan TIMESYNC_ALERT_AFTER_MS de retraso sin sincronizar, avisa con un evento de error.

  Llamar a timeSyncBegin() en setup() y a timeSyncTask() en cada vuelta de loop().
*/
#include <Arduino.h>
#include <time.h>

enum TimeSource {
  TIME_SRC_NONE = 0,
  TIME_SRC_GPS,
  TIME_SRC_NTP,
  TIME_SRC_CELL      // red celular (NITZ)
};

// event: "HORA_SYNC" (primera vez desde el arranque), "HORA_RESYNC", "HORA_SIN_SINCRONIZAR" (error)
typedef void (*TimeSyncEventFn)(const String &event, const String &detail, bool isError);
void timeSyncSetEventHandler(TimeSyncEventFn fn);

// Fija la zona horaria del ESP32 (formato POSIX, ej: "<-03>3" = UTC-3)
void timeSyncBegin(const char *posixTz);

// portIdle: true si el puerto del modem esta libre (ver modemIsIdle()). Si no lo esta, la
// ronda se posterga a la proxima vuelta (para no pisar un SMS entrante).
void timeSyncTask(bool portIdle);

// Fuerza una ronda de sincronizacion en la proxima vuelta de loop()
void timeSyncNow();

bool       timeSyncIsValid();          // el reloj tiene una fecha valida (2024 en adelante)
TimeSource timeSyncLastSource();       // metodo de la ultima sincronizacion (TIME_SRC_NONE si no hubo)
uint32_t   timeSyncAgeSeconds();       // segundos desde la ultima sincronizacion (0xFFFFFFFF si no hubo)
const char *timeSourceName(TimeSource s);
void       timeSyncPrintStatus(Print &out);
