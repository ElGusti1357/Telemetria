#pragma once
/*
  Modulo de tarjeta SD + log de eventos circular con fecha y hora.
  Usa SD/SPI/FS del framework Arduino-ESP32 (no requiere lib_deps).

  LOG CIRCULAR
    El log se guarda en segmentos dentro de SDLOG_DIR:
        /log/e0000001.csv, /log/e0000002.csv, ...
    - El tamano del segmento se calcula segun la capacidad de la tarjeta para que haya pocos
      archivos (~50): en FAT, abrir un archivo recorre toda la carpeta, asi que muchos archivos
      hacen lento el acceso. Ver SDLOG_TARGET_SEGMENTS en config.h.
    - Se escribe siempre al final del segmento mas nuevo, que se mantiene abierto (con flush en
      cada evento); al llenarse se abre el siguiente.
    - Cuando la tarjeta supera SDLOG_MAX_USED_PERCENT (o queda con menos de SDLOG_MIN_FREE_BYTES
      libres) se borran los segmentos MAS VIEJOS hasta volver por debajo del limite.
    - Nunca se reescriben datos ya grabados: borrar un archivo entero en FAT solo libera sus
      clusters. Quitar "los primeros registros" de un unico archivo obligaria a copiar todo el
      resto, por eso el borrado es de a un segmento. El segmento actual no se borra nunca.
    - Un archivo /eventos.csv de versiones anteriores se incorpora solo como primer segmento.

  TARJETA RETIRADA (hot-plug)
    - Un fallo real de escritura (fopen/fwrite/fsync) que no es por falta de espacio se toma como
      tarjeta perdida: se avisa UNA vez con el evento de error "SD_PERDIDA" (llega por SMS) y se
      desmonta.
    - Mientras falta, los eventos se guardan en memoria (los ultimos SDLOG_RAM_LINES).
    - Cada SDLOG_REMOUNT_MS se intenta montarla de nuevo (tambien si no habia tarjeta al arrancar).
      Al volver se vuelcan al log, en orden, los eventos guardados y se registra "SD_RECUPERADA".

  Hora: se toma del reloj interno del ESP32 (time/localtime_r), que se pone en hora desde
  time_sync (GPS / NTP / red celular). La zona horaria se define en main.cpp (setenv TZ).
  Hasta que el reloj tenga una fecha valida, los eventos se guardan como "T+123s"
  (segundos desde el arranque).
*/
#include <Arduino.h>
#include <time.h>

// Inicializa SPI + SD y el log circular. Devuelve false si no hay tarjeta o falla el montaje; en ese
// caso sdLogMaintain() seguira intentando montarla (por ejemplo si se inserta mas tarde).
bool sdBegin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs);

// Llamar en cada vuelta de loop(): si la tarjeta falta, la remonta cuando vuelve (hot-plug)
void sdLogMaintain();
bool sdIsReady();

// ---------- Archivos genericos ----------
// Agrega una linea al final del archivo (lo crea si no existe)
bool sdAppendLine(const char *path, const String &line);

// Escribe (reemplaza) el contenido completo del archivo
bool sdWriteFile(const char *path, const String &content);

// Lee el archivo. Si es mas grande que maxBytes devuelve solo el final.
bool sdReadFile(const char *path, String &out, size_t maxBytes = 2048);

bool   sdDeleteFile(const char *path);
bool   sdFileExists(const char *path);
size_t sdFileSize(const char *path);

// Capacidad y uso en MB
void sdInfo(uint32_t &totalMB, uint32_t &usedMB);

// Porcentaje usado de la tarjeta (0..100), o -1 si no se puede calcular
int sdUsedPercent();

// ---------- Log de eventos (circular) ----------
// Solo se informa por aqui el error "SD_LLENA": la tarjeta sigue sobre el limite aunque solo queda
// el segmento actual (p. ej. porque otros archivos ocupan el espacio). El reciclado normal
// ("SD_RECICLADO") se registra unicamente como una linea mas del log.
typedef void (*SdLogEventFn)(const String &event, const String &detail, bool isError);
void sdLogSetEventHandler(SdLogEventFn fn);

// true si el reloj del ESP32 ya tiene una fecha valida (sincronizado)
bool sdLogIsTimeSynced();

// Hora actual del log como texto "YYYY-MM-DD HH:MM:SS" o "T+123s"
String sdLogNow();

// Guarda "fecha_hora;evento;detalle" en el segmento actual (y lo muestra por Serial)
bool sdLogEvent(const String &event, const String &detail = "");

// Lee el final del log (hasta maxBytes), cruzando segmentos si hace falta, en orden cronologico
bool sdLogReadTail(String &out, size_t maxBytes = 2048);

// Borra todo el log (todos los segmentos)
bool sdLogClear();

// Tamano de cada segmento en bytes (automatico segun la capacidad, ver config.h)
size_t sdLogSegmentBytes();

// Resumen: segmentos, tamano, rango y uso de la tarjeta
void sdLogPrintStatus(Print &out);
