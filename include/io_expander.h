#pragma once
/*
  Manejo de hasta N expansores MCP23017 por I2C (ver config.h).

  - Las entradas y salidas se acceden por numero logico (1..N), segun IO_MAP.
  - Las entradas se leen por polling con antirrebote; los cambios se informan
    con un callback (ioSetInputHandler).
  - Los cambios de estado de las salidas se escriben de inmediato.
  - Si un chip deja de responder o se resetea (microcorte), el modulo lo detecta,
    lo reconfigura y restaura las salidas. Avisa por el manejador de eventos.

  Llamar a ioBegin() una vez en setup() y a ioUpdate() en cada vuelta de loop().
*/
#include <Arduino.h>

#define IO_MAX_CH 64        // maximo de entradas (o salidas) logicas: 4 chips x 16 pines

// input: numero de entrada (1..N), state: nuevo valor ya filtrado
typedef void (*IoInputFn)(uint8_t input, bool state);
// event: nombre corto (ej "IO_CHIP_FALLO"), detail: texto, isError: true si es un error
typedef void (*IoEventFn)(const String &event, const String &detail, bool isError);

void ioSetInputHandler(IoInputFn fn);
void ioSetEventHandler(IoEventFn fn);

// Inicializa I2C y los chips. Devuelve true si TODOS los chips respondieron.
bool ioBegin();

// Llamar en loop(): lee entradas, aplica antirrebote, verifica y recupera chips.
void ioUpdate();

// ---------- Cantidades segun IO_MAP ----------
uint8_t ioInputCount();
uint8_t ioOutputCount();

// ---------- Entradas (1..ioInputCount()) ----------
bool     ioReadInput(uint8_t n);    // ultimo valor estable (false si n es invalido)
uint64_t ioReadInputs();            // bit (n-1) = entrada n

// ---------- Salidas (1..ioOutputCount()) ----------
// Devuelven false si el numero es invalido o el chip no responde. En ese caso el
// estado igual se recuerda y se aplica cuando el chip vuelva.
bool     ioWriteOutput(uint8_t n, bool state);
bool     ioToggleOutput(uint8_t n);
bool     ioReadOutput(uint8_t n);   // estado pedido (guardado en memoria)
bool     ioWriteOutputs(uint64_t mask);   // todas a la vez: bit (n-1) = salida n
uint64_t ioReadOutputs();

// ---------- Estado ----------
bool ioChipOnline(uint8_t chip);    // chip 0..IO_CHIPS-1
bool ioAllOnline();
void ioPrintStatus(Print &out);     // resumen legible (para la consola)
