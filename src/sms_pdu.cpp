#include "sms_pdu.h"

// =====================================================================
//  UTF-8 <-> puntos de codigo Unicode
// =====================================================================
static void utf8Decode(const String &s, std::vector<uint32_t> &cps) {
  size_t i = 0, n = s.length();
  while (i < n) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80)              { cp = c;        extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { cps.push_back('?'); i++; continue; }

    if (i + extra >= n) { cps.push_back('?'); break; }      // secuencia cortada
    bool ok = true;
    for (int k = 1; k <= extra; k++) {
      uint8_t cc = (uint8_t)s[i + k];
      if ((cc & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok || cp > 0x10FFFF) { cps.push_back('?'); i++; continue; }
    cps.push_back(cp);
    i += extra + 1;
  }
}

static void utf8Append(String &out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

String smsUtf8Truncate(const String &s, size_t maxBytes) {
  if (s.length() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && (((uint8_t)s[cut]) & 0xC0) == 0x80) cut--;   // no cortar a mitad de caracter
  return s.substring(0, cut);
}

static String cpToAscii(uint32_t cp) {
  if (cp < 0x80) return String((char)cp);
  switch (cp) {
    case 0xE1: case 0xE0: case 0xE4: return "a";
    case 0xE9: case 0xE8: case 0xEB: return "e";
    case 0xED: case 0xEC: case 0xEF: return "i";
    case 0xF3: case 0xF2: case 0xF6: return "o";
    case 0xFA: case 0xF9: case 0xFC: return "u";
    case 0xC1: return "A";
    case 0xC9: return "E";
    case 0xCD: return "I";
    case 0xD3: return "O";
    case 0xDA: case 0xDC: return "U";
    case 0xF1: return "n";
    case 0xD1: return "N";
    case 0xBF: return "?";
    case 0xA1: return "!";
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "[U+%04X]", (unsigned)cp);
  return String(buf);
}

String smsUtf8ToAscii(const String &s) {
  std::vector<uint32_t> cps;
  utf8Decode(s, cps);
  String out;
  for (size_t i = 0; i < cps.size(); i++) out += cpToAscii(cps[i]);
  return out;
}

// =====================================================================
//  Alfabeto por defecto GSM 03.38
// =====================================================================
// Septeto (0..127) -> Unicode. El 0x1B es el escape a la tabla extendida (se maneja aparte).
static const uint16_t GSM_BASIC[128] = {
  // 0x00
  0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
  0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
  // 0x10
  0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
  0x03A3, 0x0398, 0x039E, 0x0000, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
  // 0x20
  0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
  0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
  // 0x30
  0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
  0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
  // 0x40
  0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
  0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
  // 0x50
  0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
  0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
  // 0x60
  0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
  0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
  // 0x70
  0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
  0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0
};

struct GsmExt { uint8_t septet; uint16_t cp; };
static const GsmExt GSM_EXT[] = {
  { 0x0A, 0x000C }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2F, '\\' },
  { 0x3C, '[' },    { 0x3D, '~' }, { 0x3E, ']' }, { 0x40, '|' }, { 0x65, 0x20AC }
};
static const size_t GSM_EXT_N = sizeof(GSM_EXT) / sizeof(GSM_EXT[0]);

// Unicode -> septetos GSM. Devuelve cuantos septetos usa (1 o 2) o 0 si no se puede representar.
static int cpToGsm(uint32_t cp, uint8_t out[2]) {
  for (int i = 0; i < 128; i++) {
    if (i == 0x1B) continue;
    if (GSM_BASIC[i] == cp) { out[0] = (uint8_t)i; return 1; }
  }
  for (size_t i = 0; i < GSM_EXT_N; i++) {
    if (GSM_EXT[i].cp == cp) { out[0] = 0x1B; out[1] = GSM_EXT[i].septet; return 2; }
  }
  return 0;
}

// Septetos -> Unicode (un septeto desconocido de la tabla extendida se muestra como '?')
static void gsmToCps(const std::vector<uint8_t> &sept, std::vector<uint32_t> &cps) {
  for (size_t i = 0; i < sept.size(); i++) {
    uint8_t s = sept[i] & 0x7F;
    if (s == 0x1B) {
      if (i + 1 >= sept.size()) break;
      uint8_t e = sept[++i] & 0x7F;
      uint32_t cp = '?';
      for (size_t k = 0; k < GSM_EXT_N; k++) if (GSM_EXT[k].septet == e) { cp = GSM_EXT[k].cp; break; }
      cps.push_back(cp);
    } else {
      cps.push_back(GSM_BASIC[s]);
    }
  }
}

// Empaqueta septetos [from,to) en octetos; 'pad' = bits de relleno al principio (alineacion tras un UDH)
static void packSeptets(const std::vector<uint8_t> &sept, size_t from, size_t to, int pad,
                        std::vector<uint8_t> &out) {
  size_t n = to - from;
  size_t totalBits = pad + 7 * n;
  out.assign((totalBits + 7) / 8, 0);
  for (size_t i = 0; i < n; i++) {
    size_t bit = pad + 7 * i;
    uint16_t v = (uint16_t)(sept[from + i] & 0x7F) << (bit % 8);
    out[bit / 8] |= v & 0xFF;
    if ((bit % 8) > 1) out[bit / 8 + 1] |= v >> 8;
  }
}

// Desempaqueta 'count' septetos de 'data' (sin relleno inicial)
static void unpackSeptets(const uint8_t *data, size_t dataLen, size_t count, std::vector<uint8_t> &out) {
  out.clear();
  for (size_t i = 0; i < count; i++) {
    size_t bit = 7 * i;
    size_t byteIdx = bit / 8;
    if (byteIdx >= dataLen) break;
    uint16_t v = data[byteIdx];
    if (byteIdx + 1 < dataLen) v |= (uint16_t)data[byteIdx + 1] << 8;
    out.push_back((v >> (bit % 8)) & 0x7F);
  }
}

// =====================================================================
//  Hexadecimal
// =====================================================================
static void hexAppend(String &s, uint8_t b) {
  static const char *H = "0123456789ABCDEF";
  s += H[b >> 4];
  s += H[b & 0x0F];
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool hexToBytes(const String &hex, std::vector<uint8_t> &out) {
  size_t n = hex.length();
  if (n == 0 || (n % 2) != 0) return false;
  out.clear();
  for (size_t i = 0; i < n; i += 2) {
    int h = hexVal(hex[i]), l = hexVal(hex[i + 1]);
    if (h < 0 || l < 0) return false;
    out.push_back((uint8_t)((h << 4) | l));
  }
  return true;
}

// =====================================================================
//  Envio (SMS-SUBMIT)
// =====================================================================
// Numero de destino -> digitos + indicador de formato internacional
static bool parseDest(const String &dest, String &digits, bool &intl) {
  digits = "";
  intl = false;
  for (size_t i = 0; i < dest.length(); i++) {
    char c = dest[i];
    if (c == '+' && digits.length() == 0 && !intl) intl = true;
    else if (c >= '0' && c <= '9') digits += c;
    else if (c == ' ' || c == '-' || c == '(' || c == ')') continue;
    else return false;
  }
  return digits.length() >= 3 && digits.length() <= 20;
}

static void appendAddress(String &pdu, const String &digits, bool intl) {
  hexAppend(pdu, (uint8_t)digits.length());          // cantidad de DIGITOS
  hexAppend(pdu, intl ? 0x91 : 0x81);                // tipo de numero
  for (size_t i = 0; i < digits.length(); i += 2) {  // BCD con nibbles invertidos
    uint8_t lo = digits[i] - '0';
    uint8_t hi = (i + 1 < digits.length()) ? (digits[i + 1] - '0') : 0x0F;
    hexAppend(pdu, (uint8_t)((hi << 4) | lo));
  }
}

bool smsBuildSubmit(const String &dest, const String &textUtf8,
                    std::vector<SmsPart> &parts, SmsEncoding &enc, uint8_t concatRef) {
  parts.clear();

  String digits;
  bool intl;
  if (!parseDest(dest, digits, intl)) return false;

  std::vector<uint32_t> cps;
  utf8Decode(textUtf8, cps);
  if (cps.empty()) return false;

  // ¿Entra todo en GSM de 7 bits?
  std::vector<uint8_t> sept;
  bool gsmOk = true;
  for (size_t i = 0; i < cps.size(); i++) {
    uint8_t g[2];
    int n = cpToGsm(cps[i], g);
    if (n == 0) { gsmOk = false; break; }
    sept.push_back(g[0]);
    if (n == 2) sept.push_back(g[1]);
  }
  enc = gsmOk ? SMS_ENC_GSM7 : SMS_ENC_UCS2;

  // Unidades UTF-16 (solo si hace falta)
  std::vector<uint16_t> units;
  if (!gsmOk) {
    for (size_t i = 0; i < cps.size(); i++) {
      uint32_t cp = cps[i];
      if (cp >= 0x10000) {
        cp -= 0x10000;
        units.push_back((uint16_t)(0xD800 + (cp >> 10)));
        units.push_back((uint16_t)(0xDC00 + (cp & 0x3FF)));
      } else {
        units.push_back((uint16_t)cp);
      }
    }
  }

  // Cortes: [inicio, fin) sobre septetos o unidades UTF-16
  size_t total = gsmOk ? sept.size() : units.size();
  size_t maxSingle = gsmOk ? 160 : 70;
  size_t maxMulti  = gsmOk ? 153 : 67;
  std::vector<std::pair<size_t, size_t>> ranges;
  if (total <= maxSingle) {
    ranges.push_back({0, total});
  } else {
    size_t start = 0;
    while (start < total) {
      size_t end = start + maxMulti;
      if (end > total) end = total;
      if (end < total) {
        if (gsmOk && sept[end - 1] == 0x1B) end--;                                  // no separar escape + caracter
        if (!gsmOk && units[end - 1] >= 0xD800 && units[end - 1] <= 0xDBFF) end--;  // no separar un par sustituto
      }
      ranges.push_back({start, end});
      start = end;
    }
  }
  if (ranges.size() > SMS_MAX_PARTS) return false;
  bool multi = ranges.size() > 1;

  for (size_t p = 0; p < ranges.size(); p++) {
    size_t from = ranges[p].first, to = ranges[p].second;

    // Encabezado de concatenacion (UDH de 6 bytes): 05 00 03 ref total secuencia
    std::vector<uint8_t> ud;
    if (multi) {
      ud.push_back(0x05); ud.push_back(0x00); ud.push_back(0x03);
      ud.push_back(concatRef);
      ud.push_back((uint8_t)ranges.size());
      ud.push_back((uint8_t)(p + 1));
    }

    uint8_t udl;
    if (gsmOk) {
      std::vector<uint8_t> packed;
      packSeptets(sept, from, to, multi ? 1 : 0, packed);   // 6 bytes de UDH = 48 bits -> 1 bit de relleno
      ud.insert(ud.end(), packed.begin(), packed.end());
      udl = (uint8_t)((multi ? 7 : 0) + (to - from));       // en septetos (el UDH ocupa 7)
    } else {
      for (size_t i = from; i < to; i++) {
        ud.push_back((uint8_t)(units[i] >> 8));
        ud.push_back((uint8_t)(units[i] & 0xFF));
      }
      udl = (uint8_t)ud.size();                             // en octetos
    }
    if (ud.size() > 140) return false;

    String pdu;
    hexAppend(pdu, 0x00);                                   // SMSC: el de la SIM
    hexAppend(pdu, multi ? 0x41 : 0x01);                    // SMS-SUBMIT (+ bit UDHI si hay encabezado)
    hexAppend(pdu, 0x00);                                   // referencia de mensaje
    appendAddress(pdu, digits, intl);
    hexAppend(pdu, 0x00);                                   // PID
    hexAppend(pdu, gsmOk ? 0x00 : 0x08);                    // DCS: 00 = GSM7, 08 = UCS2
    hexAppend(pdu, udl);
    for (size_t i = 0; i < ud.size(); i++) hexAppend(pdu, ud[i]);

    SmsPart part;
    part.pdu = pdu;
    part.tpduLen = (uint16_t)(pdu.length() / 2 - 1);        // sin el byte del SMSC
    parts.push_back(part);
  }
  return true;
}

// =====================================================================
//  Recepcion (SMS-DELIVER)
// =====================================================================
static int bcdPair(uint8_t b) { return (b & 0x0F) * 10 + (b >> 4); }   // semi-octetos invertidos

bool smsParseDeliver(const String &hexPdu, SmsDeliver &o) {
  o.sender = ""; o.timestamp = ""; o.text = "";
  o.isPart = false; o.concatRef = 0; o.partNo = 0; o.partTotal = 0;

  std::vector<uint8_t> b;
  if (!hexToBytes(hexPdu, b)) return false;
  size_t p = 0, n = b.size();

  if (p >= n) return false;
  size_t smscLen = b[p++];
  p += smscLen;
  if (p >= n) return false;

  uint8_t first = b[p++];
  if ((first & 0x03) != 0x00) return false;                  // no es SMS-DELIVER
  bool udhi = (first & 0x40) != 0;

  // Remitente
  if (p + 2 > n) return false;
  size_t oaDigits = b[p++];
  uint8_t oaType = b[p++];
  size_t oaBytes = (oaDigits + 1) / 2;
  if (p + oaBytes > n) return false;
  if ((oaType & 0x70) == 0x50) {                             // alfanumerico (GSM7 empaquetado)
    std::vector<uint8_t> sept;
    unpackSeptets(&b[p], oaBytes, oaDigits * 4 / 7, sept);
    std::vector<uint32_t> cps;
    gsmToCps(sept, cps);
    for (size_t i = 0; i < cps.size(); i++) utf8Append(o.sender, cps[i]);
  } else {
    if ((oaType & 0x70) == 0x10) o.sender += '+';
    for (size_t i = 0; i < oaDigits; i++) {
      uint8_t byte = b[p + i / 2];
      uint8_t nib = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
      o.sender += (nib <= 9) ? (char)('0' + nib) : (nib == 0x0A ? '*' : (nib == 0x0B ? '#' : '?'));
    }
  }
  p += oaBytes;

  // PID, DCS, fecha/hora, largo de datos
  if (p + 2 + 7 + 1 > n) return false;
  p++;                                                       // PID
  uint8_t dcs = b[p++];
  {
    char buf[32];
    uint8_t tz = b[p + 6];
    int quarters = (tz & 0x07) * 10 + (tz >> 4);
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d,%02d:%02d:%02d%c%02d",
             bcdPair(b[p]), bcdPair(b[p + 1]), bcdPair(b[p + 2]),
             bcdPair(b[p + 3]), bcdPair(b[p + 4]), bcdPair(b[p + 5]),
             ((tz & 0x08) && quarters) ? '-' : '+', quarters);
    o.timestamp = String(buf);
  }
  p += 7;
  size_t udl = b[p++];
  const uint8_t *ud = &b[p];
  size_t udLen = n - p;

  // Alfabeto segun DCS
  int alpha;                                                 // 0 = GSM7, 1 = 8 bits, 2 = UCS2
  if ((dcs & 0xC0) == 0x00)      alpha = (dcs >> 2) & 0x03;
  else if ((dcs & 0xF0) == 0xE0) alpha = 2;
  else if ((dcs & 0xF0) == 0xF0) alpha = (dcs & 0x04) ? 1 : 0;
  else                           alpha = 0;

  // Encabezado de datos de usuario (concatenacion)
  size_t hdrBytes = 0;
  if (udhi) {
    if (udLen < 1) return false;
    size_t udhl = ud[0];
    hdrBytes = udhl + 1;
    if (hdrBytes > udLen) return false;
    size_t i = 1;
    while (i + 1 < hdrBytes) {
      uint8_t iei = ud[i], iedl = ud[i + 1];
      if (i + 2 + iedl > hdrBytes) break;
      if (iei == 0x00 && iedl == 3) {
        o.isPart = true; o.concatRef = ud[i + 2]; o.partTotal = ud[i + 3]; o.partNo = ud[i + 4];
      } else if (iei == 0x08 && iedl == 4) {
        o.isPart = true; o.concatRef = ((uint16_t)ud[i + 2] << 8) | ud[i + 3];
        o.partTotal = ud[i + 4]; o.partNo = ud[i + 5];
      }
      i += 2 + iedl;
    }
  }

  std::vector<uint32_t> cps;
  if (alpha == 0) {                                          // GSM 7 bits
    size_t hdrBits = hdrBytes * 8;
    size_t pad = udhi ? (7 - hdrBits % 7) % 7 : 0;
    size_t skip = (hdrBits + pad) / 7;                       // septetos ocupados por el encabezado
    if (udl < skip) return false;
    std::vector<uint8_t> sept, text;
    unpackSeptets(ud, udLen, udl, sept);
    for (size_t i = skip; i < sept.size(); i++) text.push_back(sept[i]);
    gsmToCps(text, cps);
  } else if (alpha == 2) {                                   // UCS2 (UTF-16 big endian)
    size_t len = (udl > hdrBytes) ? udl - hdrBytes : 0;
    if (hdrBytes + len > udLen) len = udLen - hdrBytes;
    for (size_t i = 0; i + 1 < len; i += 2) {
      uint32_t u = ((uint32_t)ud[hdrBytes + i] << 8) | ud[hdrBytes + i + 1];
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
        uint32_t lo = ((uint32_t)ud[hdrBytes + i + 2] << 8) | ud[hdrBytes + i + 3];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      cps.push_back(u);
    }
  } else {                                                   // 8 bits: datos binarios
    size_t len = (udl > hdrBytes) ? udl - hdrBytes : 0;
    if (hdrBytes + len > udLen) len = udLen - hdrBytes;
    for (size_t i = 0; i < len; i++) {
      uint8_t c = ud[hdrBytes + i];
      cps.push_back((c >= 32 && c < 127) ? c : '.');
    }
  }

  for (size_t i = 0; i < cps.size(); i++) utf8Append(o.text, cps[i]);
  return true;
}
