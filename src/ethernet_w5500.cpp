#include "ethernet_w5500.h"
#include "config.h"
#include <SPI.h>
#include <Ethernet.h>
#include <Dns.h>                      // DNSClient: resolucion de nombres con timeout propio
#if __has_include(<esp_random.h>)
  #include <esp_random.h>
#else
  #include <esp_system.h>
#endif

static uint8_t     mac[6];
static W5500State  state = W5500_NO_HARDWARE;
static bool        linkUp = false;
static bool        hasIp = false;
static bool        ipTried = false;           // ya se intento obtener IP en este episodio de enlace
static bool        dhcpFailReported = false;  // para avisar una sola vez por episodio
static uint32_t    lastIpTry = 0;
static uint32_t    lastPoll = 0;
static uint32_t    lastHwRetry = 0;
static W5500EventFn _evFn = nullptr;

void w5500SetEventHandler(W5500EventFn fn) { _evFn = fn; }

static void emit(const String &event, const String &detail, bool isError) {
  if (_evFn) _evFn(event, detail, isError);
}

// MAC propia: localmente administrada (bit 1 del primer byte) y unica por placa
static void buildMac() {
#ifdef W5500_MAC
  const uint8_t fixed[6] = W5500_MAC;
  memcpy(mac, fixed, 6);
#else
  uint64_t efuse = ESP.getEfuseMac();
  const uint8_t *b = (const uint8_t *)&efuse;
  mac[0] = 0x02;        // unicast + localmente administrada
  mac[1] = 0xA7;
  mac[2] = 0x67;
  mac[3] = b[3];
  mac[4] = b[4];
  mac[5] = b[5];
#endif
}

String w5500MacText() {
  char buf[20];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void w5500PrepareBus() {
  pinMode(W5500_CS_PIN, OUTPUT);
  digitalWrite(W5500_CS_PIN, HIGH);
  pinMode(BOARD_SD_CS_PIN, OUTPUT);
  digitalWrite(BOARD_SD_CS_PIN, HIGH);
}

// Reset por hardware + inicializacion del chip (sin pedir IP todavia).
// Devuelve true si se detecto un W5500.
static bool initChip() {
  if (W5500_RST_PIN >= 0) {
    pinMode(W5500_RST_PIN, OUTPUT);
    digitalWrite(W5500_RST_PIN, LOW);
    delay(2);
    digitalWrite(W5500_RST_PIN, HIGH);
    delay(200);                               // el chip necesita tiempo para arrancar
  }
  SPI.begin(BOARD_SCK_PIN, BOARD_MISO_PIN, BOARD_MOSI_PIN);   // si la SD ya lo inicio, no hace nada
  Ethernet.init(W5500_CS_PIN);

  // Inicializa el chip con IP 0.0.0.0 (sin DHCP) para poder detectar el hardware
  // sin esperar el timeout de DHCP cuando no hay cable.
  Ethernet.begin(mac, IPAddress(0, 0, 0, 0));
  return Ethernet.hardwareStatus() == EthernetW5500;
}

// Configura la IP (DHCP o fija). Devuelve true si quedo con IP.
static bool acquireIp() {
#if W5500_USE_DHCP
  if (Ethernet.begin(mac, W5500_DHCP_TIMEOUT_MS, W5500_DHCP_RESP_MS) == 0) {
    if (!dhcpFailReported) {
      dhcpFailReported = true;
      emit("ETH_DHCP_FALLO", "sin respuesta del servidor DHCP", true);
    }
    return false;
  }
#else
  Ethernet.begin(mac, IPAddress(W5500_STATIC_IP), IPAddress(W5500_DNS),
                 IPAddress(W5500_GATEWAY), IPAddress(W5500_SUBNET));
#endif
  dhcpFailReported = false;
  emit("ETH_IP", "IP " + Ethernet.localIP().toString(), false);
  return true;
}

bool w5500Begin() {
  buildMac();
  w5500PrepareBus();
  lastHwRetry = millis();

  if (!initChip()) {
    state = W5500_NO_HARDWARE;
    emit("ETH_HW_FALLO", "W5500 no detectado (revisar SPI, CS y alimentacion 3.3V)", true);
    return false;
  }

  // Esperar la autonegociacion del enlace
  uint32_t t0 = millis();
  while (Ethernet.linkStatus() != LinkON && millis() - t0 < W5500_LINK_WAIT_MS) delay(100);

  linkUp = (Ethernet.linkStatus() == LinkON);
  if (!linkUp) {
    state = W5500_LINK_DOWN;
    // Solo informativo: un arranque sin cable no debe generar un SMS en cada reinicio.
    // La alerta (error) se reserva para cuando el enlace se cae con el sistema en marcha.
    emit("ETH_SIN_ENLACE_INICIO", "sin cable o sin enlace al iniciar", false);
    Serial.println("W5500 detectado, sin enlace (cable desconectado?)");
    return true;
  }

  hasIp = acquireIp();
  ipTried = true;
  lastIpTry = millis();
  linkUp = (Ethernet.linkStatus() == LinkON);       // la re-inicializacion puede haber renegociado
  state = hasIp ? W5500_READY : W5500_NO_IP;

  Serial.printf("W5500 detectado, enlace %s, %s\n", linkUp ? "OK" : "caido",
                hasIp ? ("IP " + Ethernet.localIP().toString()).c_str() : "sin IP");
  return true;
}

void w5500Update() {
  uint32_t now = millis();
  if (now - lastPoll < W5500_POLL_MS) return;
  lastPoll = now;

  // Chip no detectado: reintentar de vez en cuando
  if (state == W5500_NO_HARDWARE) {
    if (now - lastHwRetry >= W5500_HW_RETRY_MS) {
      lastHwRetry = now;
      if (initChip()) {
        emit("ETH_HW_RECUPERADO", "W5500 detectado", false);
        linkUp = false; hasIp = false; ipTried = false;
        state = W5500_LINK_DOWN;       // el enlace se evalua en la proxima vuelta
      }
    }
    return;
  }

  // 1) Integridad: si el chip se reseteo, su registro de MAC vuelve a cero
  uint8_t chipMac[6];
  Ethernet.MACAddress(chipMac);
  if (memcmp(chipMac, mac, 6) != 0) {
    emit("ETH_CHIP_RESET", "el W5500 se reseteo (caida de 3.3V?), se reinicia", true);
    linkUp = false; hasIp = false; ipTried = false;
    state = initChip() ? W5500_LINK_DOWN : W5500_NO_HARDWARE;
    return;
  }

  // 2) Enlace (cable)
  bool up = (Ethernet.linkStatus() == LinkON);
  if (up != linkUp) {
    linkUp = up;
    hasIp = false;
    ipTried = false;                   // al volver el enlace se pide IP enseguida
    dhcpFailReported = false;
    if (up) emit("ETH_LINK_UP", "cable conectado", false);
    else    emit("ETH_LINK_DOWN", "cable desconectado", true);
  }

  // 3) IP
  if (linkUp && !hasIp && (!ipTried || now - lastIpTry >= W5500_DHCP_RETRY_MS)) {
    ipTried = true;
    lastIpTry = now;
    hasIp = acquireIp();               // puede bloquear hasta W5500_DHCP_TIMEOUT_MS
  }

  // 4) Renovacion de la concesion DHCP
#if W5500_USE_DHCP
  if (hasIp) {
    int m = Ethernet.maintain();
    if (m == 1 || m == 3) {            // 1 = renovacion fallida, 3 = reasignacion fallida
      hasIp = false;
      ipTried = false;
      emit("ETH_DHCP_FALLO", "no se pudo renovar la IP", true);
    }
  }
#endif

  state = !linkUp ? W5500_LINK_DOWN : (hasIp ? W5500_READY : W5500_NO_IP);
}

W5500State w5500State()   { return state; }
bool w5500IsReady()       { return state == W5500_READY; }
bool w5500LinkUp()        { return linkUp; }
IPAddress w5500LocalIP()  { return Ethernet.localIP(); }

void w5500PrintStatus(Print &out) {
  const char *txt = "?";
  switch (state) {
    case W5500_NO_HARDWARE: txt = "chip NO detectado"; break;
    case W5500_LINK_DOWN:   txt = "sin enlace (cable)"; break;
    case W5500_NO_IP:       txt = "enlace OK, sin IP"; break;
    case W5500_READY:       txt = "LISTO"; break;
  }
  out.printf("W5500: %s\n", txt);
  out.printf("MAC     : %s\n", w5500MacText().c_str());
  if (state == W5500_READY || state == W5500_NO_IP) {
    out.print("IP      : "); out.println(Ethernet.localIP());
    out.print("Mascara : "); out.println(Ethernet.subnetMask());
    out.print("Gateway : "); out.println(Ethernet.gatewayIP());
    out.print("DNS     : "); out.println(Ethernet.dnsServerIP());
  }
  out.printf("Modo    : %s\n", W5500_USE_DHCP ? "DHCP" : "IP fija");
}

// Herramienta de diagnostico manual (comando ETH TEST). Es BLOQUEANTE: mientras dura,
// el loop() no atiende el modem ni las entradas. timeoutMs limita solo la conexion TCP;
// la resolucion de nombres (DNS) tiene su propio timeout dentro de la libreria Ethernet.
// Si el host es una IP no hay consulta DNS y la espera maxima es timeoutMs.
bool w5500TestTcp(const char *host, uint16_t port, uint32_t timeoutMs) {
  if (!w5500IsReady()) return false;
  EthernetClient client;
  client.setConnectionTimeout((uint16_t)timeoutMs);
  bool ok = client.connect(host, port) == 1;
  client.stop();
  return ok;
}

// ---------- Cliente NTP (SNTP, RFC 4330) ----------
bool w5500NtpGetTime(const char *server, time_t &sec, uint32_t &usec) {
  if (!w5500IsReady()) return false;

  // Resolver el nombre con un timeout corto (si ya es una IP, no hay consulta DNS)
  IPAddress ip;
  if (!ip.fromString(server)) {
    DNSClient dns;
    dns.begin(Ethernet.dnsServerIP());
    if (dns.getHostByName(server, ip, (uint16_t)W5500_NTP_DNS_TIMEOUT_MS) != 1) return false;
  }

  EthernetUDP udp;
  if (udp.begin((uint16_t)(49152 + (esp_random() % 16384))) != 1) return false;   // puerto local aleatorio

  uint8_t req[48] = {0};
  req[0] = 0x23;                                   // LI=0, VN=4, Modo=3 (cliente)
  uint32_t r1 = esp_random(), r2 = esp_random();   // marca de envio aleatoria: la respuesta debe repetirla
  memcpy(&req[40], &r1, 4);
  memcpy(&req[44], &r2, 4);

  uint32_t t0 = millis();
  if (!udp.beginPacket(ip, 123)) { udp.stop(); return false; }
  udp.write(req, sizeof(req));
  if (!udp.endPacket()) { udp.stop(); return false; }

  uint8_t rsp[48];
  bool got = false;
  while (millis() - t0 < W5500_NTP_TIMEOUT_MS) {
    int n = udp.parsePacket();
    if (n >= 48) {
      udp.read(rsp, 48);
      if (memcmp(&rsp[24], &req[40], 8) == 0) { got = true; break; }   // descarta paquetes ajenos
    }
    delay(2);
  }
  uint32_t rtt = millis() - t0;
  udp.stop();
  if (!got) return false;

  // Validaciones: reloj del servidor sincronizado, modo servidor, estrato valido (0 = "kiss of death")
  uint8_t li = rsp[0] >> 6, mode = rsp[0] & 0x07, stratum = rsp[1];
  if (li == 3 || (mode != 4 && mode != 5) || stratum == 0 || stratum > 15) return false;

  uint32_t ntpSec = ((uint32_t)rsp[40] << 24) | ((uint32_t)rsp[41] << 16) | ((uint32_t)rsp[42] << 8) | rsp[43];
  uint32_t frac   = ((uint32_t)rsp[44] << 24) | ((uint32_t)rsp[45] << 16) | ((uint32_t)rsp[46] << 8) | rsp[47];
  if (ntpSec == 0) return false;

  // NTP cuenta desde 1900; Unix desde 1970 (diferencia 2208988800 s). Tras 2036 el contador de 32 bits da la vuelta.
  int64_t unixSec = (ntpSec >= 2208988800UL) ? (int64_t)ntpSec - 2208988800LL
                                             : (int64_t)ntpSec + 4294967296LL - 2208988800LL;
  uint64_t us = ((uint64_t)frac * 1000000ULL) >> 32;       // fraccion de segundo -> microsegundos
  us += (uint64_t)(rtt / 2) * 1000ULL;                     // compensar la mitad del viaje de ida y vuelta
  unixSec += (int64_t)(us / 1000000ULL);
  usec = (uint32_t)(us % 1000000ULL);
  sec = (time_t)unixSec;
  return true;
}
