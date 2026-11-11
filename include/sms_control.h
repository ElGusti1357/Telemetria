#pragma once
/*
  Control del equipo por SMS.

  Flujo: el modem avisa de cada SMS recibido (smsControlOnReceived) -> si el remitente esta en la
  agenda se pone en una cola -> smsControlProcess() (desde loop(), con el modem libre) lo analiza,
  ejecuta el comando con el interprete de commands.cpp y responde AL REMITENTE.

  Seguridad:
    - Los remitentes que no estan en la agenda se ignoran sin responder (no se gasta saldo ni se
      confirma que el equipo existe); se anota en el log, a lo sumo una vez por minuto.
    - Los comandos de administracion exigen que el remitente sea ADMIN y que el mensaje empiece
      con el PIN (SMSCMD_PIN): "1234 SALIDA 5 1". El remitente de un SMS se puede falsificar, por
      eso el numero solo no alcanza.
    - SMSCMD_PIN_MAX_FAILS PIN incorrectos seguidos bloquean la administracion por SMS un rato
      y generan una alerta.
    - Tope de respuestas por hora (SMSCMD_MAX_REPLIES_HOUR): corta bucles entre dos equipos que
      se contestan y limita el gasto.
    - El PIN nunca se escribe en el log.
    - Los mensajes largos (varias partes) no se procesan como comandos.
*/
#include <Arduino.h>

// Lo llama el modem por cada SMS recibido (solo encola; no ejecuta nada)
void smsControlOnReceived(const String &sender, const String &text, bool isPart);

// Llamar en cada vuelta de loop(): procesa un mensaje pendiente si el modem esta libre
void smsControlProcess();
