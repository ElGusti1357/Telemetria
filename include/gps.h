#pragma once
/*
  Módulo GPS para LilyGO T-A7670 (A7670G / A7670E con GNSS)
  Usa comandos AT directos (AT+CGNSSPWR / AT+CGNSSINFO), por lo que no
  depende de la versión de TinyGSM-fork.
*/
#include <Arduino.h>
#include <time.h>

struct GpsPosition {
  double  latitude;    // grados decimales (negativo = Sur)
  double  longitude;   // grados decimales (negativo = Oeste)
  float   altitude;    // metros
  float   speedKmh;    // km/h
  float   course;      // grados (rumbo)
  float   hdop;        // precisión horizontal (menor = mejor)
  uint8_t satellites;  // satélites usados (GPS + GLONASS + BeiDou)
};

struct GpsTime {       // Hora UTC entregada por el GPS
  uint16_t year;
  uint8_t  month, day, hour, minute, second;
};

// Inicializa el GPS. 'at' es el Serial conectado al módem (SerialAT).
bool gpsBegin(Stream &at);

// Apaga el GPS (ahorra energía)
void gpsEnd();

// Obtiene la posición actual. Devuelve false si todavía no hay fix.
bool gpsGetPosition(GpsPosition &pos);

// Obtiene la fecha/hora UTC del GPS. Devuelve false si todavía no hay fix.
bool gpsGetTime(GpsTime &t);

// Obtiene ambas en una sola consulta.
bool gpsGetFix(GpsPosition &pos, GpsTime &t);

// Espera (bloqueante) hasta tener fix o hasta que pase timeoutMs.
bool gpsWaitForFix(uint32_t timeoutMs, GpsPosition *pos = nullptr, GpsTime *t = nullptr);

// Convierte GpsTime (UTC) a epoch Unix. offsetSeconds: zona horaria,
// por ejemplo Argentina (UTC-3) = -3 * 3600.
time_t gpsTimeToEpoch(const GpsTime &t, int offsetSeconds = 0);

// Devuelve "YYYY-MM-DD HH:MM:SS" (con el offset aplicado)
String gpsTimeToString(const GpsTime &t, int offsetSeconds = 0);
