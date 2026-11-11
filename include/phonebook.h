#pragma once
/*
  Agenda de numeros autorizados a mandar comandos por SMS (y a recibir las respuestas).

  - Se guarda en memoria permanente (NVS, libreria Preferences): sobrevive a reinicios y a
    cambios de firmware que no borren la particion NVS.
  - Cada numero tiene un rol: USUARIO (comandos de consulta) o ADMINISTRADOR (ademas, comandos que
    cambian cosas; por SMS tambien requieren el PIN).
  - La primera vez se carga PHONE_DEFAULT_ADMIN como administrador, para poder empezar.
  - Un remitente con nombre (no numerico, por ejemplo "Personal") nunca queda autorizado.
  - Los numeros se comparan por sus ultimos PHONE_MATCH_DIGITS digitos, asi "+5491154052645",
    "+541154052645" y "1154052645" son el mismo numero.

  ATENCION: el remitente de un SMS se puede falsificar. Por eso los comandos que modifican algo
  exigen tambien el PIN (SMSCMD_PIN) y no solo que el numero este en la agenda.
*/
#include <Arduino.h>

enum PhoneRole { PHONE_NONE = 0, PHONE_USER = 1, PHONE_ADMIN = 2 };

// Abre la memoria permanente y carga la agenda (siembra el administrador inicial la primera vez)
bool phonebookBegin();

int        phoneCount();
int        phoneAdminCount();
bool       phoneGet(int index, String &number, PhoneRole &role);   // index 0..phoneCount()-1
PhoneRole  phoneRoleOf(const String &sender);                      // PHONE_NONE si no esta autorizado
const char *phoneRoleName(PhoneRole r);

// Agrega un numero o, si ya estaba, le cambia el rol. msg recibe el resultado o el motivo del rechazo.
// protectLastAdmin: no permite dejar la agenda sin ningun administrador (se usa para los pedidos por SMS).
bool phoneAdd(const String &number, PhoneRole role, String &msg, bool protectLastAdmin);

// Borra por numero o por posicion (1..N, como la muestra phoneList)
bool phoneRemove(const String &numberOrPosition, String &msg, bool protectLastAdmin);

void phoneList(Print &out);
