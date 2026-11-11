#pragma once
/*
  Soporte para el modulo Ethernet W5500 (USR-ES1 / Starware) por SPI.
  Usa la libreria "Ethernet" de Arduino (arduino-libraries/Ethernet >= 2.0).

  - Arranque no bloqueante en lo posible: detecta el chip, espera el enlace y obtiene IP
    (DHCP o IP fija, segun config.h).
  - w5500Update() vigila el cable (enlace), renueva el DHCP y detecta si el chip
    se reseteo (por ejemplo por una caida de 3.3V), reiniciandolo solo.
  - Avisa lo que pasa mediante un manejador de eventos (igual que modem e io_expander).

  Para usar sockets despues de w5500IsReady() alcanza con usar EthernetClient /
  EthernetServer / EthernetUDP de la libreria Ethernet.
*/
#include <Arduino.h>
#include <IPAddress.h>

enum W5500State {
  W5500_NO_HARDWARE,   // el chip no responde por SPI
  W5500_LINK_DOWN,     // chip OK, sin cable / sin enlace
  W5500_NO_IP,         // hay enlace pero todavia no hay IP
  W5500_READY          // enlace + IP: se puede usar la red
};

// event: nombre corto (ej "ETH_LINK_DOWN"), detail: texto, isError: true si es un error
typedef void (*W5500EventFn)(const String &event, const String &detail, bool isError);
void w5500SetEventHandler(W5500EventFn fn);

// Dejar el CS del W5500 (y el de la SD) en alto. Llamar al INICIO de setup(), antes de
// usar el bus SPI, para que un CS flotante no interfiera con la SD.
void w5500PrepareBus();

// Detecta el chip, espera el enlace y obtiene IP.
// Devuelve true si el CHIP fue detectado (el enlace/IP pueden quedar pendientes:
// ver w5500IsReady()).
bool w5500Begin();

// Llamar en cada vuelta de loop()
void w5500Update();

W5500State w5500State();
bool       w5500IsReady();          // enlace + IP
bool       w5500LinkUp();
IPAddress  w5500LocalIP();
String     w5500MacText();
void       w5500PrintStatus(Print &out);

// Consulta la hora a un servidor NTP (UDP 123) por Ethernet. 'server' puede ser nombre o IP.
// Es BLOQUEANTE (hasta W5500_NTP_DNS_TIMEOUT_MS + W5500_NTP_TIMEOUT_MS). Valida la respuesta
// y compensa la mitad del retardo de ida y vuelta. Devuelve la hora UTC en sec + usec.
#include <time.h>
bool w5500NtpGetTime(const char *server, time_t &sec, uint32_t &usec);

// Prueba de conectividad: resuelve el nombre (si es host) y abre una conexion TCP.
bool w5500TestTcp(const char *host, uint16_t port, uint32_t timeoutMs = 3000);
