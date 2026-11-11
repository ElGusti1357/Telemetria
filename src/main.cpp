/*
  LilyGO T-A7670 (ESP32 + A7670) - SMS + GPS + log de eventos en SD + alertas por SMS
  Base: Random Nerd Tutorials (Rui Santos & Sara Santos)

  Modulos:
    config.h   -> pines, APN, destinos, tiempos
    modem.*    -> modem: arranque, SIM, red, SMS
    gps.*      -> GPS: posicion y hora
    time_sync.* -> fecha y hora del ESP32: GPS, NTP o red celular
    sd_log.*   -> tarjeta SD y log de eventos con fecha y hora
    alerts.*   -> errores: log + SMS de aviso
    io_expander.* -> entradas/salidas digitales con MCP23017 (I2C)
    ethernet_w5500.* -> red Ethernet con el modulo W5500 (SPI)
    phonebook.* -> numeros autorizados a mandar comandos por SMS (memoria permanente)
    commands.*  -> interprete de comandos (consola y SMS)
    sms_control.* -> recibe SMS, autoriza al remitente, ejecuta el comando y responde
*/
#include <Arduino.h>
#include "config.h"
#include "modem.h"
#include "gps.h"
#include "time_sync.h"
#include "sd_log.h"
#include "alerts.h"
#include "io_expander.h"
#include "ethernet_w5500.h"
#include "phonebook.h"
#include "commands.h"
#include "sms_control.h"

// Eventos que informan los modulos (modem, IO): los errores van a alertas (log + SMS),
// el resto al log
void onModuleEvent(const String &event, const String &detail, bool isError) {
  if (isError) {
    alertError(detail.length() ? event + ": " + detail : event);
  } else {
    sdLogEvent(event, detail);
  }
}

// Cambio (ya filtrado) de una entrada digital
void onInputChange(uint8_t input, bool state) {
  sdLogEvent("ENTRADA", "IN" + String(input) + "=" + (state ? "1" : "0"));
}

// ---------- Consola ----------
// Los comandos viven en commands.cpp y se comparten con el control por SMS. Escribir AYUDA para la lista.
#define CONSOLE_LINE_MAX 256     // largo maximo de una linea de consola
String txLine;

void handleConsoleLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  CmdContext ctx;                // consola: siempre con permiso de administrador, sin PIN
  cmdExecute(line, Serial, ctx);
}

void setup() {
  Serial.begin(115200);
  delay(2000);                // tiempo para abrir el monitor serie
  Serial.println("Init...");
  timeSyncBegin(TZ_POSIX);
  timeSyncSetEventHandler(onModuleEvent);
  w5500PrepareBus();      // CS del W5500 y de la SD en alto antes de usar el bus SPI compartido

  // Alimentacion de la placa (modem y SD)
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);

  // ---------- SD (no es fatal si falla) ----------
  sdLogSetEventHandler(onModuleEvent);   // avisos del log circular (borrado de segmentos, tarjeta llena)
  bool sdOk = sdBegin(BOARD_SCK_PIN, BOARD_MISO_PIN, BOARD_MOSI_PIN, BOARD_SD_CS_PIN);
  Serial.println(sdOk ? "SD OK" : "SD no disponible: los eventos solo se mostraran por consola");
  sdLogEvent("BOOT", "inicio del sistema");

  // ---------- Alertas (recupera errores pendientes de antes de un reinicio) ----------
  alertsBegin(ALERT_DESTINATION);
  if (!sdOk) alertError("SD no disponible");

  // ---------- Agenda de numeros autorizados (control por SMS) ----------
  if (phonebookBegin()) Serial.printf("Agenda: %d numero(s) autorizado(s)\n", phoneCount());
  else                  alertError("No se pudo abrir la agenda de numeros autorizados (NVS)");

  // ---------- Entradas/salidas (MCP23017) ----------
  // Se inician antes que el modem: sus salidas quedan en estado seguro (apagadas) desde el arranque
  ioSetEventHandler(onModuleEvent);
  ioSetInputHandler(onInputChange);
  ioBegin();     // si algun chip falla, avisa por alertError (el SMS sale cuando haya red)

  // ---------- Modem ----------
  modemSetEventHandler(onModuleEvent);
  modemSetSmsHandler(smsControlOnReceived);   // los SMS de numeros autorizados se ejecutan como comandos
  ModemStatus st = modemInit();
  if (st != MODEM_STATUS_OK) alertFatal(modemStatusText(st));   // reinicia el ESP32

  // ---------- GPS ----------
  if (gpsBegin(modemSerial())) {
    Serial.println("GPS encendido (esperando fix, el reloj se sincroniza solo)");
    sdLogEvent("GPS_ON");
  } else {
    Serial.println("No se pudo encender el GPS");
    alertError("GPS: no se pudo encender");
  }

  // ---------- Ethernet (W5500) ----------
  // Va despues del modem: lo critico (alertas por SMS) arranca primero y esto puede tardar
  // unos segundos (enlace + DHCP). Un fallo del chip, del cable o del DHCP se informa por
  // alertError y el SMS sale desde loop(), ya con el modem listo.
  w5500SetEventHandler(onModuleEvent);
  w5500Begin();

  // ---------- Enviar SMS ----------
  Serial.printf("Enviando SMS a %s ...\n", SMS_DESTINATION);
  bool ok = modemSendSms(SMS_DESTINATION, SMS_TEXT);
  Serial.println(ok ? "SMS enviado OK" : "Fallo el envio del SMS");
}

void loop() {
  modemProcess();
  smsControlProcess();    // ejecuta (y responde) un comando recibido por SMS, con el modem libre
  if (cmdRestartPending()) {
    Serial.println("Reinicio pedido: reiniciando...");
    delay(1500);          // da tiempo a que termine de salir la respuesta
    ESP.restart();
  }
  ioUpdate();       // lee entradas (antirrebote) y vigila los MCP23017
  w5500Update();    // vigila cable, DHCP y estado del chip Ethernet

  // Leer lo escrito en la consola, linea por linea
  // Si el monitor no manda fin de linea, se procesa tras 400 ms sin recibir caracteres
  static unsigned long lastConsoleCharMs = 0;
  static bool dropLine = false;      // true mientras se descarta el resto de una linea demasiado larga
  while (Serial.available()) {
    char c = Serial.read();
    lastConsoleCharMs = millis();
    if (c == '\n' || c == '\r') {
      if (!dropLine && txLine.length() > 0) handleConsoleLine(txLine);
      txLine = "";
      dropLine = false;
    } else if (dropLine) {
      // ignorar hasta el fin de linea
    } else if (txLine.length() < CONSOLE_LINE_MAX) {
      txLine += c;
    } else {
      txLine = "";
      dropLine = true;
      Serial.println("[consola] linea demasiado larga, descartada");
    }
  }
  if (millis() - lastConsoleCharMs > 400) {
    dropLine = false;                // monitor sin fin de linea: se recupera tras 400 ms sin datos
    if (txLine.length() > 0) {
      handleConsoleLine(txLine);
      txLine = "";
    }
  }

  // Reloj del ESP32 en hora: GPS, NTP o red celular (el primero que funcione), cada 12 h
  timeSyncTask(modemIsIdle());

  // Si la SD fue retirada (o no estaba al arrancar), reintenta montarla
  sdLogMaintain();

  // Enviar por SMS los errores pendientes
  alertsProcess();

  delay(1);
}
