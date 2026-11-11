#pragma once
/*
  Configuracion central del proyecto (LilyGO T-A7670)
  Todo lo que se suele querer cambiar esta en este archivo.
*/

// ---------- Operador ----------
#define SIM_PIN         ""                      // PIN de la SIM, si tiene
#define NETWORK_APN     "datos.personal.com"    // APN (Personal Argentina)

// ---------- SMS ----------
#define SMS_DESTINATION "+5491154052645"
#define SMS_TEXT        "Hola Gustavo"
#define ALERT_DESTINATION SMS_DESTINATION       // a quien se le avisan los errores

// ---------- Control por SMS ----------
// Los numeros autorizados se guardan en memoria permanente (NVS) y se administran con el comando
// NUM (por consola o por SMS). Solo estos numeros pueden mandar comandos; el resto se ignora.
#define PHONE_MAX                8                   // cantidad maxima de numeros autorizados
#define PHONE_MATCH_DIGITS       10                  // se comparan los ultimos N digitos (tolera +549 11..., +54 11... y 11...)
#define PHONE_DEFAULT_ADMIN      SMS_DESTINATION     // administrador inicial (solo se carga la primera vez)
// PIN para comandos de administracion por SMS: se escribe al inicio del mensaje ("1234 LOGDEL").
// ¡CAMBIARLO! Viaja en texto claro por la red celular. "" = sin PIN (solo importa que el numero sea administrador).
#define SMSCMD_PIN               "1234"
#define SMSCMD_PIN_MAX_FAILS     5                   // PIN incorrectos seguidos...
#define SMSCMD_PIN_WINDOW_MS     600000UL            // ...dentro de esta ventana (10 min)...
#define SMSCMD_PIN_LOCK_MS       900000UL            // ...bloquean la administracion por SMS durante 15 min
#define SMSCMD_REPLY_MAX_CHARS   300                 // largo maximo de una respuesta (el resto se corta)
#define SMSCMD_MAX_REPLIES_HOUR  30                  // tope de respuestas por hora (frena bucles y gastos)
#define SMSCMD_QUEUE             4                   // mensajes pendientes de procesar

// ---------- Zona horaria ----------
// Argentina = UTC-3. TZ_POSIX usa el signo invertido: "<-03>3" = UTC-3
#define TZ_POSIX        "<-03>3"
#define TZ_OFFSET_SEC   (-3 * 3600)             // solo para mostrar hora local en el comando GPS

// ---------- Limites de arranque del modem ----------
#define MODEM_START_ROUNDS   3          // rondas de 10 intentos de AT (con pulso de PWRKEY entre rondas)
#define SIM_TIMEOUT_MS       30000UL
#define REG_TIMEOUT_MS       180000UL
#define RESTART_WAIT_SEC     15         // espera antes de reiniciar ante un error fatal

// ---------- Alertas por SMS ----------
#define ALERT_MIN_INTERVAL_MS  60000UL      // minimo entre dos SMS de alerta
#define ALERT_DEDUP_MS         600000UL     // no repetir el mismo error durante 10 min
#define ALERT_MAX_SMS_CHARS    150          // largo maximo de un SMS de alerta (1 SMS = 160)
#define ALERT_QUEUE_MAX        8            // errores pendientes en cola

// ---------- Sincronizacion de fecha y hora (GPS, NTP por Ethernet, red celular) ----------
// Orden en que se prueban los metodos (se pasa al siguiente si uno falla):
//   G = GPS   N = NTP (internet, por el W5500)   C = hora de la red celular (NITZ, AT+CCLK)
// Precision: NTP ~ decenas de ms; GPS y red celular ~ 1 s. Para priorizar la precision: "NGC".
#define TIMESYNC_ORDER          "GNC"
#define TIMESYNC_PERIOD_MS      43200000UL   // 12 h: con el cristal del ESP32 (~10-40 ppm) el desvio es < 2 s
#define TIMESYNC_RETRY_MIN_MS   60000UL      // si fallan los tres metodos: reintento a 1 min...
#define TIMESYNC_RETRY_MAX_MS   900000UL     // ...duplicando el intervalo hasta un maximo de 15 min
#define TIMESYNC_ALERT_AFTER_MS 1800000UL    // avisar (SMS) si pasan 30 min de retraso sin lograr sincronizar
#define TIMESYNC_NTP_SERVERS    { "ar.pool.ntp.org", "time.google.com" }   // se prueban en orden

// ---------- Log de eventos en la SD (circular, por segmentos) ----------
// El log se guarda en archivos (/log/e0000001.csv, e0000002.csv, ...). Cuando la tarjeta supera
// SDLOG_MAX_USED_PERCENT se borran los segmentos mas viejos (sin reescribir nada) y se sigue
// escribiendo en el mas nuevo. El borrado es de a un segmento entero: es lo que permite
// "quitar los primeros registros" en FAT sin copiar el resto de los datos.
//
// IMPORTANTE: en FAT, abrir un archivo obliga a recorrer toda la carpeta, asi que muchos archivos
// = acceso lento. Por eso el tamano del segmento se calcula SEGUN LA CAPACIDAD de la tarjeta para
// que haya como maximo ~SDLOG_TARGET_SEGMENTS archivos (2 GB -> ~36 MB, 8 GB -> ~144 MB,
// 32 GB -> 512 MB por archivo), y el segmento actual se mantiene abierto entre eventos.
#define SDLOG_DIR               "/log"
#define SDLOG_SEGMENT_BYTES     0                      // 0 = automatico (recomendado); un valor fijo lo fuerza
#define SDLOG_TARGET_SEGMENTS   50                     // cantidad de archivos objetivo al llegar al limite
#define SDLOG_SEGMENT_MIN_BYTES (256UL * 1024UL)       // limites del tamano automatico
#define SDLOG_SEGMENT_MAX_BYTES (512UL * 1024UL * 1024UL)
#define SDLOG_MAX_USED_PERCENT  90                     // % de la tarjeta a partir del cual se borra lo mas viejo
#define SDLOG_MIN_FREE_BYTES    (64UL * 1024UL)        // ademas, nunca dejar menos libre que esto
#define SDLOG_MOUNT             "/sd"                  // punto de montaje de la SD en el sistema de archivos del ESP32
#define SDLOG_REMOUNT_MS        30000UL                // reintento de montaje si la tarjeta falta (cada intento puede tardar ~1 s)
#define SDLOG_RAM_LINES         32                     // eventos que se guardan en memoria mientras la tarjeta no esta

// ---------- Expansores I2C MCP23017 ----------
#define IO_I2C_SDA            21            // verificar que 21 y 22 esten libres en tu placa
#define IO_I2C_SCL            22
#define IO_I2C_FREQ           400000UL
#define IO_RESET_PIN          (-1)          // GPIO conectado al RESET de los MCP (-1 = no usado, RESET a 3.3V)

#define IO_CHIPS              4
#define IO_I2C_ADDRS          { 0x20, 0x21, 0x22, 0x23 }   // A2 A1 A0 de cada chip

/*
  Mapa de pines: una fila por chip, 16 caracteres por fila.
  Posicion 0-7 = GPA0..GPA7, posicion 8-15 = GPB0..GPB7.
    'I' = entrada (1 = nivel alto)
    'L' = entrada activa en bajo, con pull-up interno (contacto a GND: 1 = cerrado)
    'O' = salida
    '-' = libre / a definir (queda como entrada con pull-up, sin uso)
  Las entradas y salidas se numeran (desde 1) en el orden: chip 0..3, pin 0..15.
*/
#define IO_MAP { \
  /* 0x20 */ "IIIIIIIIIIIIIIII", \
  /* 0x21 */ "IIIIOOOOOOOOOOOO", \
  /* 0x22 */ "OOOOOOOOOOOOOO--", \
  /* 0x23 */ "----------------"  \
}
#define IO_EXPECTED_INPUTS    20            // el modulo avisa si el mapa no coincide
#define IO_EXPECTED_OUTPUTS   26

#define IO_POLL_MS            5             // cada cuanto se leen las entradas
#define IO_DEBOUNCE_SAMPLES   4             // lecturas iguales seguidas para aceptar un cambio (4 x 5 ms = 20 ms)
#define IO_VERIFY_MS          1000          // cada cuanto se verifica que cada chip no se haya reseteado
#define IO_RETRY_MS           2000          // reintento de conexion con un chip caido
#define IO_FAIL_LIMIT         3             // errores I2C seguidos para dar un chip por caido
#define IO_I2C_TIMEOUT_MS     20

// ---------- Ethernet W5500 (modulo USR-ES1 / Starware, por SPI) ----------
// Comparte el bus SPI con la SD (BOARD_SCK_PIN / MISO / MOSI), cada uno con su propio CS.
#define W5500_CS_PIN          32            // verificar que este libre en tu placa
#define W5500_RST_PIN         18            // -1 si no esta conectado
// El pin INT del modulo no hace falta (se hace polling)

#define W5500_USE_DHCP        1             // 1 = DHCP, 0 = IP fija (usar los valores de abajo)
#define W5500_STATIC_IP       192,168,1,50
#define W5500_GATEWAY         192,168,1,1
#define W5500_SUBNET          255,255,255,0
#define W5500_DNS             8,8,8,8
// #define W5500_MAC          { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 }   // opcional: si no se define, se arma con el MAC unico del ESP32

#define W5500_LINK_WAIT_MS    3000UL        // espera de la autonegociacion al iniciar
#define W5500_DHCP_TIMEOUT_MS 2500UL        // maximo de espera por DHCP (bloqueante)
#define W5500_DHCP_RESP_MS    2000UL
#define W5500_DHCP_RETRY_MS   60000UL       // reintento de DHCP si no se obtuvo IP (2,5 s de bloqueo cada 60 s)
#define W5500_POLL_MS         500UL         // cada cuanto se revisa enlace, chip y renovacion DHCP
#define W5500_HW_RETRY_MS     30000UL       // reintento si el chip no fue detectado
#define W5500_NTP_DNS_TIMEOUT_MS 2000UL     // consulta DNS del servidor NTP (bloqueante)
#define W5500_NTP_TIMEOUT_MS     1500UL     // espera de la respuesta NTP (bloqueante)

// ---------- TinyGSM / placa ----------
#define TINY_GSM_RX_BUFFER 1024
#define LILYGO_T_A7670

#if defined(LILYGO_T_A7670)
  #define MODEM_BAUDRATE                      (115200)
  #define MODEM_DTR_PIN                       (25)
  #define MODEM_TX_PIN                        (26)
  #define MODEM_RX_PIN                        (27)
  #define BOARD_PWRKEY_PIN                    (4)
  #define BOARD_ADC_PIN                       (35)
  #define BOARD_POWERON_PIN                   (12)
  #define MODEM_RING_PIN                      (33)
  #define MODEM_RESET_PIN                     (5)
  #define BOARD_MISO_PIN                      (2)
  #define BOARD_MOSI_PIN                      (15)
  #define BOARD_SCK_PIN                       (14)
  #define BOARD_SD_CS_PIN                     (13)
  #define BOARD_BAT_ADC_PIN                   (35)
  #define MODEM_RESET_LEVEL                   HIGH
  #define SerialAT                            Serial1
  #define MODEM_GPS_ENABLE_GPIO               (-1)
  #define MODEM_GPS_ENABLE_LEVEL              (-1)
  #ifndef TINY_GSM_MODEM_A7670
    #define TINY_GSM_MODEM_A7670
  #endif
#endif
