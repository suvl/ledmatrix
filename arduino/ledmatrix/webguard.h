// Portal hardening (slice 14, ticket 46): the decision logic that keeps other web pages away from the portal. Pure logic with no Arduino
// or ESP headers, so it is tested on the laptop (tools/test_webguard.cpp). The part that touches the web server is net.cpp (one
// middleware, one function portalOwnNames()).
//
// The portal has no password, by design (docs/security.md). What it does refuse is a request that a browser sends on behalf of ANOTHER
// web page, with two checks:
//
//   1. Host check, on every request (reads too): the Host header must be one of the display's own names or addresses. A web page on
//      evil.example whose name was re-pointed at the display's address (DNS rebinding) still sends "Host: evil.example", so it cannot
//      read /api/settings (the home location is in there). Refused with 421.
//   2. Origin/Referer check, on every state-changing request (anything but GET, HEAD, OPTIONS): when the browser names the page the
//      request came from (the Origin header, or the Referer when there is no Origin) that page must be the display itself. Refused
//      with 403. A request with neither header (curl, a script) is not a browser acting for another page and keeps working.
//
// "The display's own names": <name>.local (mDNS), <name> alone (the router's DNS often knows it), its IPv4 address, its IPv6
// addresses, and in setup mode the address of its own Wi-Fi network (192.168.4.1). Ports are ignored. Capital letters and one
// trailing dot are ignored. Everything else is refused, and anything odd about the text itself (a space, a control character, an @
// "user:password@" part, a backslash, a very long name) is refused too: the check fails closed.
//
// Nothing here logs or prints; the caller decides what to say. The reason codes are short fixed words, never a header value.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace webguard {

// ---- sizes ------------------------------------------------------------------------------------------------------------------
static const size_t HEADER_MAX = 300;   // a Host or Origin header longer than this is refused (a name is at most 253 characters, plus a port)
static const size_t CANON_MAX = 96;     // room for the canonical host of a name the display could own; a longer one cannot be ours
static const size_t OWN_NAME_MAX = 64;      // one stored own name, including the NUL
static const size_t NAMES_MAX = 8;      // name, name.local, IPv4, two IPv6, the AP address, and two spare

// A piece of header text with its length, so that a NUL inside the value is seen (a String's c_str() would stop at it).
struct Text {
  const char *p;
  size_t n;
  Text() : p(nullptr), n(0) {}
  Text(const char *s) : p(s), n(s ? strlen(s) : 0) {}
  Text(const char *s, size_t len) : p(s), n(s ? len : 0) {}
};

// ---- small character helpers ------------------------------------------------------------------------------------------------
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline char lowerCase(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline int hexValue(char c) { return isDigit(c) ? c - '0' : (lowerCase(c) - 'a' + 10); }

// Cut spaces and tabs off both ends.
inline Text trim(Text t) {
  while (t.n > 0 && (t.p[0] == ' ' || t.p[0] == '\t')) { t.p++; t.n--; }
  while (t.n > 0 && (t.p[t.n - 1] == ' ' || t.p[t.n - 1] == '\t')) t.n--;
  return t;
}

// What may appear in a host (with an optional port, or an IPv6 literal in brackets). Anything else - a control character, a space,
// "@", "/", a backslash, "?", "#", a quote, a non-ASCII byte - makes the whole text unusable.
inline bool hostCharOk(char c) { return isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == ':' || c == '[' || c == ']' || c == '%'; }

// ---- IPv6 -------------------------------------------------------------------------------------------------------------------
// The text between the brackets, to the canonical form "[...]": lower case, no leading zeros, the first longest run of zero groups
// (at least two) written "::". The display's own address and the browser's spelling of it both go through this, so they compare equal
// whichever way each was written. An embedded IPv4 tail ("::ffff:1.2.3.4") is refused.
inline bool canonIPv6(const char *s, size_t n, char *out, size_t cap) {
  if (!s || n == 0 || cap < 2 + 39 + 1) return false;
  uint16_t g[8];
  int count = 0, gap = -1;
  size_t i = 0;
  if (s[0] == ':') {
    if (n < 2 || s[1] != ':') return false;
    gap = 0;
    i = 2;
  }
  while (i < n) {
    uint32_t v = 0;
    int digits = 0;
    while (i < n && isHex(s[i])) {
      if (++digits > 4) return false;
      v = v * 16 + (uint32_t)hexValue(s[i]);
      i++;
    }
    if (digits == 0 || count >= 8) return false;  // an empty group (":::", "1:", ":1") or too many
    g[count++] = (uint16_t)v;
    if (i == n) break;
    if (s[i] != ':') return false;  // includes '.', an embedded IPv4 tail
    i++;
    if (i < n && s[i] == ':') {
      if (gap >= 0) return false;  // "::" twice
      gap = count;
      i++;
      if (i == n) break;
    } else if (i == n) {
      return false;  // a single ':' at the end
    }
  }
  uint16_t all[8];
  if (gap < 0) {
    if (count != 8) return false;
    for (int k = 0; k < 8; k++) all[k] = g[k];
  } else {
    if (count > 7) return false;  // "::" stands for at least one group
    int fill = 8 - count, k = 0;
    for (int a = 0; a < gap; a++) all[k++] = g[a];
    for (int a = 0; a < fill; a++) all[k++] = 0;
    for (int a = gap; a < count; a++) all[k++] = g[a];
  }
  int bestStart = -1, bestLen = 0;
  for (int a = 0; a < 8;) {
    if (all[a] != 0) { a++; continue; }
    int b = a;
    while (b < 8 && all[b] == 0) b++;
    if (b - a > bestLen && b - a >= 2) { bestStart = a; bestLen = b - a; }
    a = b;
  }
  size_t o = 0;
  out[o++] = '[';
  bool sep = false;
  for (int a = 0; a < 8;) {
    if (a == bestStart) {
      out[o++] = ':';
      out[o++] = ':';
      a += bestLen;
      sep = false;
      continue;
    }
    if (sep) out[o++] = ':';
    char digitsOut[4];
    int nd = 0;
    uint16_t v = all[a];
    do { digitsOut[nd++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    while (nd) out[o++] = digitsOut[--nd];
    sep = true;
    a++;
  }
  out[o++] = ']';
  out[o] = 0;
  return true;
}

// ---- host (with an optional port) to its canonical text ---------------------------------------------------------------------------
// "Ledmatrix.LOCAL.:80" -> "ledmatrix.local";  "[FE80:0:0:0:0:0:0:1%25en0]:80" -> "[fe80::1]";  "192.0.2.10:80" -> "192.0.2.10".
// The port is checked (1 to 5 digits, at most 65535) and dropped. One trailing dot is dropped. A zone ("%...") is dropped. False when the
// text is not a plain host: empty, too long, a forbidden character, a bare IPv6 address without brackets (two colons), an empty label
// ("a..b", ".a"), a label over 63 characters, a bad port, or a canonical form longer than `cap` - 1.
inline bool canonHost(Text t, char *out, size_t cap) {
  t = trim(t);
  if (!t.p || t.n == 0 || t.n > HEADER_MAX || cap < 2) return false;
  for (size_t i = 0; i < t.n; i++)
    if (!hostCharOk(t.p[i])) return false;

  const char *portText = nullptr;
  size_t portLen = 0;
  if (t.p[0] == '[') {
    size_t close = 1;
    while (close < t.n && t.p[close] != ']') close++;
    if (close >= t.n) return false;
    size_t inner = close - 1;
    for (size_t i = 1; i < close; i++)
      if (t.p[i] == '%') { inner = i - 1; break; }  // the zone is not part of the address
    if (inner == 0) return false;
    if (close + 1 < t.n) {  // after the bracket: only ":port"
      if (t.p[close + 1] != ':') return false;
      portText = t.p + close + 2;
      portLen = t.n - close - 2;
    }
    if (!canonIPv6(t.p + 1, inner, out, cap)) return false;
  } else {
    size_t colons = 0, colonAt = 0;
    for (size_t i = 0; i < t.n; i++) {
      char c = t.p[i];
      if (c == '[' || c == ']' || c == '%') return false;
      if (c == ':') { colons++; colonAt = i; }
    }
    if (colons > 1) return false;
    size_t hostLen = t.n;
    if (colons == 1) {
      hostLen = colonAt;
      portText = t.p + colonAt + 1;
      portLen = t.n - colonAt - 1;
    }
    if (hostLen > 0 && t.p[hostLen - 1] == '.') hostLen--;  // "ledmatrix.local." is the same name
    if (hostLen == 0 || hostLen >= cap) return false;
    size_t label = 0;
    for (size_t i = 0; i < hostLen; i++) {
      char c = t.p[i];
      if (c == '.') {
        if (label == 0) return false;
        label = 0;
      } else if (++label > 63) {
        return false;
      }
      out[i] = lowerCase(c);
    }
    if (label == 0) return false;
    out[hostLen] = 0;
  }
  if (portText) {  // "host:" (no digits) is refused too
    if (portLen == 0 || portLen > 5) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < portLen; i++) {
      if (!isDigit(portText[i])) return false;
      v = v * 10 + (uint32_t)(portText[i] - '0');
    }
    if (v > 65535) return false;
  }
  return true;
}

// "http://host:port/anything" or "https://..." to the canonical host. Nothing but http and https. The authority (what lies between "://" and
// the next "/", "?" or "#") goes through canonHost, which refuses an "@" and a backslash (so "evil.example@ledmatrix.local",
// "ledmatrix.local@evil.example" and "evil.example\@ledmatrix.local" are all unusable: a parser that reads a backslash as a slash and
// one that does not would disagree about the last, so it is refused outright). With `originOnly` the text must end right after the authority, as a browser's
// Origin header does.
inline bool canonUrlHost(Text t, bool originOnly, char *out, size_t cap) {
  t = trim(t);
  if (!t.p) return false;
  size_t start;
  auto prefix = [&](const char *lit) {
    size_t L = strlen(lit);
    if (t.n < L) return false;
    for (size_t i = 0; i < L; i++)
      if (lowerCase(t.p[i]) != lit[i]) return false;
    return true;
  };
  if (prefix("http://")) start = 7;
  else if (prefix("https://")) start = 8;
  else return false;
  size_t end = start;
  while (end < t.n && t.p[end] != '/' && t.p[end] != '?' && t.p[end] != '#') {
    if (end - start > HEADER_MAX) return false;  // no end of the authority in sight: do not scan a huge header any further
    end++;
  }
  if (originOnly && end != t.n) return false;
  return canonHost(Text(t.p + start, end - start), out, cap);
}

// ---- the display's own names --------------------------------------------------------------------------------------------------------
// Built by the caller for each request (cheap) from whatever the display is called and addressed as right now.
struct Names {
  char item[NAMES_MAX][OWN_NAME_MAX];
  size_t count = 0;

  // A host name or an address, as the display writes it: "ledmatrix.local", "192.0.2.10", "fe80::1" (brackets are added when missing).
  // Ignored (false) when empty, unusable, too long, the table is full, or it is the "unset" address 0.0.0.0 / ::.
  bool add(const char *text) {
    if (!text || !text[0] || count >= NAMES_MAX) return false;
    char wrapped[CANON_MAX + 2];
    size_t n = strlen(text);
    Text t(text, n);
    if (memchr(text, ':', n) && text[0] != '[') {  // an IPv6 address written without brackets
      if (n + 2 >= sizeof(wrapped)) return false;
      wrapped[0] = '[';
      memcpy(wrapped + 1, text, n);
      wrapped[n + 1] = ']';
      t = Text(wrapped, n + 2);
    }
    char canon[CANON_MAX];
    if (!canonHost(t, canon, sizeof(canon))) return false;
    if (strcmp(canon, "0.0.0.0") == 0 || strcmp(canon, "[::]") == 0) return false;
    if (strlen(canon) >= OWN_NAME_MAX) return false;
    if (has(canon)) return true;
    strcpy(item[count++], canon);
    return true;
  }

  // The display's name, as `<name>` and `<name>.local`.
  bool addDevice(const char *name) {
    if (!name || !name[0] || strlen(name) + 6 >= CANON_MAX) return false;
    char dotLocal[CANON_MAX];
    strcpy(dotLocal, name);
    strcat(dotLocal, ".local");
    bool a = add(name);
    bool b = add(dotLocal);
    return a && b;
  }

  bool has(const char *canon) const {
    for (size_t i = 0; i < count; i++)
      if (strcmp(item[i], canon) == 0) return true;
    return false;
  }
};

// ---- the decision -------------------------------------------------------------------------------------------------------------------
enum class Verdict : uint8_t {
  Allow = 0,
  HostMissing,        // no Host header, or an empty one
  HostForeign,        // the Host is not one of the display's own names (or is not a plain host at all)
  OriginNull,         // "Origin: null" (a sandboxed frame, a local file, a privacy setting): it names no page, so it cannot be the display
  OriginMalformed,    // an Origin that is not http(s)://host[:port] alone
  OriginForeign,      // another site's Origin
  RefererMalformed,   // no Origin, and a Referer that is not an http(s) address with a plain host
  RefererForeign,     // no Origin, and another site's Referer
};

inline bool allowed(Verdict v) { return v == Verdict::Allow; }

// The HTTP answer for a refusal: 421 (Misdirected Request) for a Host the display does not answer to, 403 for a request from another page.
inline int httpStatus(Verdict v) {
  switch (v) {
    case Verdict::Allow: return 200;
    case Verdict::HostMissing:
    case Verdict::HostForeign: return 421;
    default: return 403;
  }
}

// A short fixed word for the log. Never a header value.
inline const char *reason(Verdict v) {
  switch (v) {
    case Verdict::Allow: return "allowed";
    case Verdict::HostMissing: return "host-missing";
    case Verdict::HostForeign: return "host";
    case Verdict::OriginNull: return "origin-null";
    case Verdict::OriginMalformed: return "origin-malformed";
    case Verdict::OriginForeign: return "origin";
    case Verdict::RefererMalformed: return "referer-malformed";
    case Verdict::RefererForeign: return "referer";
  }
  return "?";
}

// Decide one request.
//   own           the display's own names right now
//   checkHost     true in station mode. False in setup mode: there the display's own Wi-Fi network answers every name (its DNS
//                 sends all of them to the display, so a phone's captive-portal probe arrives with any Host), and the Host cannot
//                 tell a stranger from a probe. The Origin check still applies (a page is "the display itself" when its host is
//                 one of the own names or is the very name the request was sent to).
//   stateChanging true for every method but GET, HEAD and OPTIONS
//   host, origin, referer   the three header values; empty (or null) when the header was not sent
inline Verdict check(const Names &own, bool checkHost, bool stateChanging, Text host, Text origin, Text referer) {
  char hostCanon[CANON_MAX];
  bool hostOk = canonHost(host, hostCanon, sizeof(hostCanon));
  if (checkHost) {
    if (trim(host).n == 0) return Verdict::HostMissing;
    if (!hostOk || !own.has(hostCanon)) return Verdict::HostForeign;
  }
  if (!stateChanging) return Verdict::Allow;

  char page[CANON_MAX];
  Text o = trim(origin);
  if (o.n > 0) {
    if (o.n == 4 && lowerCase(o.p[0]) == 'n' && lowerCase(o.p[1]) == 'u' && lowerCase(o.p[2]) == 'l' && lowerCase(o.p[3]) == 'l') return Verdict::OriginNull;
    if (!canonUrlHost(o, true, page, sizeof(page))) return Verdict::OriginMalformed;
    if (own.has(page) || (hostOk && strcmp(page, hostCanon) == 0)) return Verdict::Allow;
    return Verdict::OriginForeign;
  }
  Text r = trim(referer);
  if (r.n > 0) {  // only when there is no Origin
    if (!canonUrlHost(r, false, page, sizeof(page))) return Verdict::RefererMalformed;
    if (own.has(page) || (hostOk && strcmp(page, hostCanon) == 0)) return Verdict::Allow;
    return Verdict::RefererForeign;
  }
  return Verdict::Allow;  // neither header: curl, a script, a browser that sends neither: not another page acting for someone
}

// ---- for the log --------------------------------------------------------------------------------------------------------------------
// Copy at most cap - 1 characters of `s` into `out` for a log line: printable ASCII only, anything else (a control character, a byte
// above 127, a NUL) becomes '?', so a crafted request path cannot put escape sequences or a fake second line in the log.
inline size_t printable(const char *s, size_t n, char *out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; s && i < n && o + 1 < cap; i++) out[o++] = (s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?';
  out[o] = 0;
  return o;
}

}  // namespace webguard
