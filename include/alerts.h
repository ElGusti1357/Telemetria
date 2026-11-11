#pragma once
/*
  Alertas de error por SMS.

  - alertError(msg): registra el error en el log (SD) y lo encola para SMS.
  - alertFatal(msg): igual, pero ademas reinicia el ESP32 tras unos segundos.
  - alertsProcess(): llamar en loop(); envia la cola cuando el modem esta listo.

  Limites para no inundar de SMS (ver config.h):
   * minimo ALERT_MIN_INTERVAL_MS entre SMS; los errores pendientes viajan juntos
   * el mismo error no se repite durante ALERT_DEDUP_MS

  Si el error impide enviar SMS (modem sin AT, SIM, sin red), queda pendiente
  en memoria RTC (sobrevive al reinicio, NO a un corte de energia) y se envia
  apenas el modem se registra en la red, como "Reinicio previo: ...".
*/
#include <Arduino.h>

void alertsBegin(const String &destination);
void alertError(const String &msg);
void alertFatal(const String &msg);   // no retorna: reinicia el ESP32
void alertsProcess();
