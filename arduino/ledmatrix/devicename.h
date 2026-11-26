// The display name (ticket 45): what the display calls itself on the network. It is the mDNS host name (the portal is at
// <name>.local), the DHCP host name and the name of the setup Wi-Fi. Pure C++ with no Arduino includes, like otalogic.h, so the
// laptop tests build it (tools/test_devicename.cpp).
//
// The rules are the DNS label rules (RFC 1123): lower-case letters, digits and hyphens, 1 to 24 characters, no hyphen at either end.
// 24 is well under the 63 a DNS label may have and the 32 a Wi-Fi network name may have, so one name fits all three uses and
// the 64-pixel panel can still show most of it. The name is stored and used in lower case: mDNS names are not case-sensitive,
// but the setup Wi-Fi name is, and one spelling avoids two names for one display.
#pragma once
#include <stddef.h>
#include <string.h>

namespace devicename {

constexpr size_t MAX_LEN = 24;  // characters, without the end mark: a buffer holds MAX_LEN + 1
constexpr const char *DEFAULT_NAME = "ledmatrix";  // what a display is called until the owner chooses (and what an old display keeps)

enum class Error : unsigned char {
  None,
  NotText,  // the settings JSON held something other than text (the caller finds that out, normalise() never returns it)
  Empty,
  TooLong,
  BadChar,  // anything but a letter, a digit or a hyphen: a space inside, an underscore, a dot, an accent, a control character
  Hyphen    // starts or ends with a hyphen
};

// The plain sentence for an error, in the style of the other settings errors. It goes into a JSON string without escaping, so it
// never holds a quote, a backslash or a control character (the host test checks that).
inline const char *message(Error e) {
  switch (e) {
    case Error::None: return "";
    case Error::NotText: return "The display name must be text, such as ledmatrix.";
    case Error::Empty: return "The display name cannot be empty.";
    case Error::TooLong: return "The display name can be at most 24 characters.";
    case Error::BadChar: return "The display name can only have letters, digits and hyphens, with no spaces.";
    case Error::Hyphen: return "The display name cannot start or end with a hyphen.";
  }
  return "";
}

// Checks a name as the owner typed it (len bytes at `in`, which may hold anything, even a NUL) and writes the normalised name
// into out, a buffer of at least MAX_LEN + 1 bytes: spaces and tabs at either end are dropped and upper-case letters become lower
// case. Returns Error::None, or the first problem found, and then out holds an empty string.
inline Error normalise(const char *in, size_t len, char *out) {
  out[0] = 0;
  size_t a = 0, b = len;
  while (a < b && (in[a] == ' ' || in[a] == '\t')) a++;
  while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t')) b--;
  size_t n = b - a;
  if (n == 0) return Error::Empty;
  if (n > MAX_LEN) return Error::TooLong;
  for (size_t i = 0; i < n; i++) {
    char c = in[a + i];
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!letter && !(c >= '0' && c <= '9') && c != '-') return Error::BadChar;
  }
  if (in[a] == '-' || in[b - 1] == '-') return Error::Hyphen;
  for (size_t i = 0; i < n; i++) {
    char c = in[a + i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  out[n] = 0;
  return Error::None;
}

// A name read back from flash: it must already be in the normalised form (what this module wrote), or it is ignored and the
// default stays. A damaged value never reaches the mDNS name or the access point.
inline bool storedOk(const char *s) {
  char tmp[MAX_LEN + 1];
  size_t n = strlen(s);
  return normalise(s, n, tmp) == Error::None && strcmp(tmp, s) == 0;
}

}  // namespace devicename
