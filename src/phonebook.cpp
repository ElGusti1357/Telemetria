#include "phonebook.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;
static bool        opened = false;
static String      nums[PHONE_MAX];          // numeros normalizados ("+5491154052645" o "1154052645")
static PhoneRole   roles[PHONE_MAX];
static int         count_ = 0;

// ---------- Utilidades ----------
// Deja solo '+' inicial y digitos. Devuelve false si no parece un numero de telefono.
static bool normalize(const String &in, String &out) {
  out = "";
  int digits = 0;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '+' && out.length() == 0) out += c;
    else if (c >= '0' && c <= '9') { out += c; digits++; }
    else if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.') continue;
    else return false;                       // letras u otros simbolos: remitente con nombre, no autorizable
  }
  return digits >= 6 && digits <= 15;
}

static String digitsOf(const String &n) {
  String d;
  for (size_t i = 0; i < n.length(); i++) if (n[i] >= '0' && n[i] <= '9') d += n[i];
  return d;
}

static bool sameNumber(const String &a, const String &b) {
  String da = digitsOf(a), db = digitsOf(b);
  if (da == db) return true;
  const size_t n = PHONE_MATCH_DIGITS;       // tolera distintos prefijos de pais / el 9 / el 0 de area
  if (da.length() >= n && db.length() >= n) {
    return da.substring(da.length() - n) == db.substring(db.length() - n);
  }
  return false;
}

static int indexOfNumber(const String &normalized) {
  for (int i = 0; i < count_; i++) if (sameNumber(nums[i], normalized)) return i;
  return -1;
}

static void save() {
  if (!opened) return;
  for (int i = 0; i < PHONE_MAX; i++) {
    String kn = String("n") + String(i);
    String kr = String("r") + String(i);
    if (i < count_) {
      prefs.putString(kn.c_str(), nums[i]);
      prefs.putUChar(kr.c_str(), (uint8_t)roles[i]);
    } else {
      prefs.remove(kn.c_str());
      prefs.remove(kr.c_str());
    }
  }
  prefs.putUChar("count", (uint8_t)count_);
}

// ---------- API ----------
bool phonebookBegin() {
  opened = prefs.begin("phonebook", false);
  count_ = 0;
  if (!opened) return false;

  if (!prefs.getBool("init", false)) {       // primer arranque: se siembra el administrador inicial
    String n;
    if (normalize(String(PHONE_DEFAULT_ADMIN), n)) {
      nums[0] = n;
      roles[0] = PHONE_ADMIN;
      count_ = 1;
    }
    save();
    prefs.putBool("init", true);
    return true;
  }

  int c = prefs.getUChar("count", 0);
  if (c > PHONE_MAX) c = PHONE_MAX;
  for (int i = 0; i < c; i++) {
    String kn = String("n") + String(i);
    String kr = String("r") + String(i);
    String n = prefs.getString(kn.c_str(), "");
    uint8_t r = prefs.getUChar(kr.c_str(), 0);
    if (n.length() == 0 || (r != PHONE_USER && r != PHONE_ADMIN)) continue;   // registro danado: se omite
    nums[count_] = n;
    roles[count_] = (PhoneRole)r;
    count_++;
  }
  return true;
}

int phoneCount() { return count_; }

int phoneAdminCount() {
  int n = 0;
  for (int i = 0; i < count_; i++) if (roles[i] == PHONE_ADMIN) n++;
  return n;
}

bool phoneGet(int index, String &number, PhoneRole &role) {
  if (index < 0 || index >= count_) return false;
  number = nums[index];
  role = roles[index];
  return true;
}

PhoneRole phoneRoleOf(const String &sender) {
  String n;
  if (!normalize(sender, n)) return PHONE_NONE;
  int i = indexOfNumber(n);
  return i >= 0 ? roles[i] : PHONE_NONE;
}

const char *phoneRoleName(PhoneRole r) {
  switch (r) {
    case PHONE_ADMIN: return "ADMIN";
    case PHONE_USER:  return "USUARIO";
    default:          return "NINGUNO";
  }
}

bool phoneAdd(const String &number, PhoneRole role, String &msg, bool protectLastAdmin) {
  String n;
  if (!normalize(number, n)) { msg = "Numero invalido"; return false; }
  if (role != PHONE_USER && role != PHONE_ADMIN) { msg = "Rol invalido"; return false; }

  int i = indexOfNumber(n);
  if (i >= 0) {                              // ya estaba: solo se cambia el rol
    if (roles[i] == PHONE_ADMIN && role != PHONE_ADMIN && protectLastAdmin && phoneAdminCount() <= 1) {
      msg = "No se puede quitar el ultimo administrador";
      return false;
    }
    roles[i] = role;
    save();
    msg = String("Actualizado: ") + nums[i] + " " + phoneRoleName(role);
    return true;
  }
  if (count_ >= PHONE_MAX) { msg = String("Agenda llena (max ") + String(PHONE_MAX) + ")"; return false; }
  nums[count_] = n;
  roles[count_] = role;
  count_++;
  save();
  msg = String("Agregado: ") + n + " " + phoneRoleName(role);
  return true;
}

bool phoneRemove(const String &arg, String &msg, bool protectLastAdmin) {
  String a = arg;
  a.trim();
  int idx = -1;

  String n;
  bool allDigits = a.length() > 0 && a.length() <= 2;
  for (size_t i = 0; i < a.length() && allDigits; i++) if (a[i] < '0' || a[i] > '9') allDigits = false;
  if (allDigits) {                           // posicion 1..N
    int p = atoi(a.c_str());
    if (p >= 1 && p <= count_) idx = p - 1;
  } else if (normalize(a, n)) {
    idx = indexOfNumber(n);
  }
  if (idx < 0) { msg = "No esta en la agenda"; return false; }

  if (roles[idx] == PHONE_ADMIN && protectLastAdmin && phoneAdminCount() <= 1) {
    msg = "No se puede borrar el ultimo administrador";
    return false;
  }
  String removed = nums[idx];
  for (int i = idx; i < count_ - 1; i++) { nums[i] = nums[i + 1]; roles[i] = roles[i + 1]; }
  count_--;
  save();
  msg = String("Borrado: ") + removed;
  return true;
}

void phoneList(Print &out) {
  if (count_ == 0) { out.print("Agenda vacia\n"); return; }
  for (int i = 0; i < count_; i++) {
    out.printf("%d: %s %s\n", i + 1, nums[i].c_str(), phoneRoleName(roles[i]));
  }
}
