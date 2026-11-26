// Host test for arduino/ledmatrix/webguard.h (portal hardening, slice 14 / ticket 46): the Host check on every request, the Origin /
// Referer check on every state-changing request, the canonical form of hosts and IPv6 addresses, the display's own names, the
// reason codes and the log helper.
// Run from the repo root:
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix tools/test_webguard.cpp -o <scratch>/test_webguard && <scratch>/test_webguard
// Also with the sanitizers (they catch buffer overruns, bad shifts and signed overflow):
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iarduino/ledmatrix tools/test_webguard.cpp -o <scratch>/test_webguard_asan && <scratch>/test_webguard_asan
// Every host, address and name in here is a placeholder (example.com, 192.0.2.x, 2001:db8::/32, aabbccddeeff).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "webguard.h"

using namespace webguard;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(name) printf("%s\n", name)

// The display of these tests: called ledmatrix, 192.0.2.10 on the home network, one link-local and one global IPv6 address.
static Names station() {
  Names n;
  n.addDevice("ledmatrix");
  n.add("192.0.2.10");
  n.add("fe80::1234:5678:9abc:def0");
  n.add("2001:db8::10");
  return n;
}
// In setup mode the display has no home address, only its own Wi-Fi network's.
static Names setupNames() {
  Names n;
  n.addDevice("ledmatrix");
  n.add("192.168.4.1");
  return n;
}

// A station-mode GET (reads) and POST (state-changing) with the given headers.
static Verdict get(const char *host) { return check(station(), true, false, Text(host), Text(), Text()); }
static Verdict post(const char *host, const char *origin = nullptr, const char *referer = nullptr) {
  return check(station(), true, true, Text(host), Text(origin), Text(referer));
}

static std::string canon(const char *s) {
  char out[CANON_MAX];
  return canonHost(Text(s), out, sizeof(out)) ? std::string(out) : std::string("<refused>");
}
static std::string canonUrl(const char *s, bool originOnly) {
  char out[CANON_MAX];
  return canonUrlHost(Text(s), originOnly, out, sizeof(out)) ? std::string(out) : std::string("<refused>");
}

// ---- canonical hosts ----------------------------------------------------------------------------------------------------------
static void testCanonHost() {
  SECTION("canonical host: names, ports, case, trailing dot");
  CHECK(canon("ledmatrix.local") == "ledmatrix.local");
  CHECK(canon("ledmatrix.local:80") == "ledmatrix.local");
  CHECK(canon("ledmatrix.local:8080") == "ledmatrix.local");
  CHECK(canon("LedMatrix.LOCAL") == "ledmatrix.local");
  CHECK(canon("ledmatrix.local.") == "ledmatrix.local");
  CHECK(canon("ledmatrix.local.:80") == "ledmatrix.local");
  CHECK(canon("  ledmatrix.local  ") == "ledmatrix.local");     // spaces and tabs around it are cut
  CHECK(canon("\tledmatrix.local\t") == "ledmatrix.local");
  CHECK(canon("ledmatrix") == "ledmatrix");
  CHECK(canon("my-display_2.local") == "my-display_2.local");
  CHECK(canon("192.0.2.10") == "192.0.2.10");
  CHECK(canon("192.0.2.10:80") == "192.0.2.10");
  CHECK(canon("192.0.2.10:65535") == "192.0.2.10");
  CHECK(canon("192.0.2.10:0") == "192.0.2.10");

  SECTION("canonical host: refused");
  for (const char *bad : {"", " ", "\t", ".", "..", ".local", "ledmatrix..local", "ledmatrix.local..", "a..", "..a", "ledmatrix.local:", "ledmatrix.local:abc",
                          "ledmatrix.local:65536", "ledmatrix.local:123456", "ledmatrix.local:80:80", "ledmatrix.local:-1", "ledmatrix.local: 80",
                          "ledmatrix .local", "ledmatrix\x01.local", "ledmatrix.local\r", "ledmatrix.local\n", "ledmatrix.local\x7f",
                          "evil.example@ledmatrix.local", "ledmatrix.local@evil.example", "user:pw@ledmatrix.local", "ledmatrix.local/", "ledmatrix.local/x",
                          "ledmatrix.local\\evil.example", "ledmatrix.local?x", "ledmatrix.local#x", "ledmatrix.local;x", "ledmatrix,local", "ledmatrix.local\"",
                          "ledmatrix.local'", "ledmatrix.l\xc3\xb6" "cal", "\xe2\x80\xae" "ledmatrix.local", "ledmatrix.local%00", "led%6datrix.local",
                          "[ledmatrix.local]", "[", "]", "[]", "[::1", "::1", "fe80::1", "192.0.2.10]", "[::1]x", "[::1]:", "[::1]:99999", "[::1][::1]"}) {
    if (canon(bad) != "<refused>") { printf("    unexpectedly accepted: \"%s\" -> %s\n", bad, canon(bad).c_str()); }
    CHECK(canon(bad) == "<refused>");
  }
  CHECK(canon(nullptr) == "<refused>");
  char one[1];
  CHECK(!canonHost(Text("ledmatrix.local"), one, sizeof(one)));    // an output buffer that is too small is refused, not overrun
  char small[8];
  CHECK(!canonHost(Text("ledmatrix.local"), small, sizeof(small)));
  CHECK(canonHost(Text("a.b"), small, sizeof(small)) && strcmp(small, "a.b") == 0);
  CHECK(canonHost(Text("abcdefg"), small, sizeof(small)) && strcmp(small, "abcdefg") == 0);   // 7 characters + NUL fit in 8 bytes
  CHECK(!canonHost(Text("abcdefgh"), small, sizeof(small)));                                   // 8 characters + NUL do not

  SECTION("canonical host: labels");
  std::string l63(63, 'a'), l64(64, 'a');
  CHECK(canon((l63 + ".local").c_str()) == l63 + ".local");
  CHECK(canon((l64 + ".local").c_str()) == "<refused>");

  SECTION("canonical host: very long");
  std::string longName(HEADER_MAX, 'a');           // exactly at the header limit but one label: refused by the label rule
  CHECK(canon(longName.c_str()) == "<refused>");
  std::string manyLabels;
  while (manyLabels.size() < HEADER_MAX + 50) manyLabels += "a.";
  CHECK(canon(manyLabels.c_str()) == "<refused>");
  std::string ninetyFive;                           // fits the canonical buffer (95 characters + NUL = 96)
  while (ninetyFive.size() < 95) ninetyFive += (ninetyFive.size() % 10 == 9 ? "." : "b");
  CHECK(ninetyFive.size() == 95 && canon(ninetyFive.c_str()) == ninetyFive);
  CHECK(canon((ninetyFive + "b").c_str()) == "<refused>");   // 96 characters need 97 bytes: too long for a name the display could own
  CHECK(canon(std::string(100000, 'a').c_str()) == "<refused>");

  SECTION("canonical host: NUL inside the text is seen");
  const char withNul[] = "ledmatrix.local\0.evil.example";
  CHECK(canonHost(Text(withNul, sizeof(withNul) - 1), nullptr, 0) == false);
  char buf[CANON_MAX];
  CHECK(!canonHost(Text(withNul, sizeof(withNul) - 1), buf, sizeof(buf)));
  CHECK(!canonHost(Text("ledmatrix.local\0", 16), buf, sizeof(buf)));
  CHECK(canonHost(Text("ledmatrix.local\0", 15), buf, sizeof(buf)) && strcmp(buf, "ledmatrix.local") == 0);  // the length is what counts

  SECTION("canonical IPv6");
  CHECK(canon("[fe80::1]") == "[fe80::1]");
  CHECK(canon("[FE80::1]") == "[fe80::1]");
  CHECK(canon("[fe80:0:0:0:0:0:0:1]") == "[fe80::1]");
  CHECK(canon("[fe80:0000:0000:0000:0000:0000:0000:0001]") == "[fe80::1]");
  CHECK(canon("[0FE80::1]") == "<refused>");                      // five digits in a group
  CHECK(canon("[fe80::1]:80") == "[fe80::1]");
  CHECK(canon("[FE80::1]:8080") == "[fe80::1]");
  CHECK(canon("[fe80::1%25en0]") == "[fe80::1]");                 // a zone is dropped
  CHECK(canon("[fe80::1%en0]:80") == "[fe80::1]");
  CHECK(canon("[::1]") == "[::1]");
  CHECK(canon("[0:0:0:0:0:0:0:1]") == "[::1]");
  CHECK(canon("[::]") == "[::]");
  CHECK(canon("[0:0:0:0:0:0:0:0]") == "[::]");
  CHECK(canon("[2001:db8:0:0:1:0:0:1]") == "[2001:db8::1:0:0:1]");  // the FIRST longest run is compressed
  CHECK(canon("[2001:db8::1:0:0:1]") == "[2001:db8::1:0:0:1]");
  CHECK(canon("[2001:0:0:1:0:0:0:1]") == "[2001:0:0:1::1]");        // the longer run wins
  CHECK(canon("[2001:db8:0:1:1:1:1:1]") == "[2001:db8:0:1:1:1:1:1]");  // a single zero group is not compressed
  CHECK(canon("[1:2:3:4:5:6:7:8]") == "[1:2:3:4:5:6:7:8]");
  CHECK(canon("[1:2:3:4:5:6:7::]") == "[1:2:3:4:5:6:7:0]");        // "::" for one group is accepted; a single zero group is written out
  CHECK(canon("[::2:3:4:5:6:7:8]") == "[0:2:3:4:5:6:7:8]");
  CHECK(canon("[1::]") == "[1::]");
  CHECK(canon("[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]") == "[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]");
  for (const char *bad : {"[1:2:3:4:5:6:7:8:9]", "[1:2:3:4:5:6:7]", "[1::2::3]", "[:::]", "[::1::]", "[1:2:3:4::5:6:7:8]", "[:1]", "[1:]", "[1:2:3:4:5:6:7:8::]",
                          "[g::1]", "[fe80::1.2.3.4]", "[::ffff:1.2.3.4]", "[ ::1]", "[::1 ]", "[fe80::1%]x", "[%25en0]", "[fe80::1]x", "[fe80::1]:80:80"}) {
    if (canon(bad) != "<refused>") { printf("    unexpectedly accepted: \"%s\" -> %s\n", bad, canon(bad).c_str()); }
    CHECK(canon(bad) == "<refused>");
  }
  char tiny[10];
  CHECK(!canonIPv6("fe80::1", 7, tiny, sizeof(tiny)));              // an output buffer too small for any address
  CHECK(!canonIPv6(nullptr, 0, buf, sizeof(buf)));
}

static void testCanonUrl() {
  SECTION("canonical host of an Origin / Referer");
  CHECK(canonUrl("http://ledmatrix.local", true) == "ledmatrix.local");
  CHECK(canonUrl("http://ledmatrix.local:80", true) == "ledmatrix.local");
  CHECK(canonUrl("https://ledmatrix.local", true) == "ledmatrix.local");
  CHECK(canonUrl("HTTP://LEDMATRIX.LOCAL", true) == "ledmatrix.local");
  CHECK(canonUrl("http://192.0.2.10", true) == "192.0.2.10");
  CHECK(canonUrl("http://[fe80::1234:5678:9abc:def0]", true) == "[fe80::1234:5678:9abc:def0]");
  CHECK(canonUrl("http://[FE80:0:0:0:1234:5678:9ABC:DEF0]:80", true) == "[fe80::1234:5678:9abc:def0]");
  CHECK(canonUrl("  http://ledmatrix.local  ", true) == "ledmatrix.local");
  CHECK(canonUrl("http://ledmatrix.local/", true) == "<refused>");               // an Origin has no path
  CHECK(canonUrl("http://ledmatrix.local/", false) == "ledmatrix.local");        // a Referer has
  CHECK(canonUrl("http://ledmatrix.local/page?x=1#y", false) == "ledmatrix.local");
  CHECK(canonUrl("http://ledmatrix.local?x=1", false) == "ledmatrix.local");
  CHECK(canonUrl("http://ledmatrix.local#y", false) == "ledmatrix.local");
  CHECK(canonUrl("http://[fe80::1]:80/a/b", false) == "[fe80::1]");
  for (const char *bad : {"", "http://", "https://", "http:/ledmatrix.local", "http:ledmatrix.local", "//ledmatrix.local", "ledmatrix.local", "ftp://ledmatrix.local",
                          "file:///etc/passwd", "ws://ledmatrix.local", "chrome-extension://abcdef", "android-app://com.example.app", "about:blank", "data:text/html,x",
                          "javascript:alert(1)", "http://evil.example@ledmatrix.local", "http://ledmatrix.local@evil.example", "http://user:pw@ledmatrix.local",
                          "http://ledmatrix.local\\@evil.example", "http://evil.example\\@ledmatrix.local/", "http:///ledmatrix.local", "http://:80", "http://ledmatrix.local:80:80",
                          "http://ledmatrix .local", "http://ledmatrix.local\x01", "httpx://ledmatrix.local", "http//ledmatrix.local", "http://[::1", "http://::1/"}) {
    if (canonUrl(bad, false) != "<refused>") { printf("    unexpectedly accepted (referer rules): \"%s\" -> %s\n", bad, canonUrl(bad, false).c_str()); }
    CHECK(canonUrl(bad, false) == "<refused>");
    CHECK(canonUrl(bad, true) == "<refused>");
  }
  CHECK(canonUrl(nullptr, true) == "<refused>");
  // A huge header must be refused without scanning it all (and without overrunning anything).
  std::string huge = "http://" + std::string(200000, 'a') + ".example/";
  CHECK(canonUrl(huge.c_str(), false) == "<refused>");
  CHECK(canonUrl(("http://" + std::string(HEADER_MAX + 10, 'a')).c_str(), true) == "<refused>");
  // ... but a long Referer PATH after a good authority is fine: the display's own page can have a long address.
  std::string longPath = "http://ledmatrix.local/" + std::string(5000, 'p');
  CHECK(canonUrl(longPath.c_str(), false) == "ledmatrix.local");
}

// ---- the display's own names ---------------------------------------------------------------------------------------------------------
static void testNames() {
  SECTION("own names");
  Names n;
  CHECK(n.count == 0);
  CHECK(n.addDevice("ledmatrix"));
  CHECK(n.count == 2 && n.has("ledmatrix") && n.has("ledmatrix.local"));
  CHECK(n.add("192.0.2.10") && n.has("192.0.2.10"));
  CHECK(n.add("fe80::ABCD:1") && n.has("[fe80::abcd:1]"));                // brackets added, lower case
  CHECK(n.add("[2001:db8::10]") && n.has("[2001:db8::10]"));              // brackets already there
  CHECK(n.add("2001:0DB8:0:0:0:0:0:10") && n.count == 5);                 // the same address again: no second entry
  CHECK(!n.has("evil.example") && !n.has("localhost") && !n.has("127.0.0.1") && !n.has("192.168.4.1"));
  CHECK(!n.add(nullptr) && !n.add("") && !n.add("0.0.0.0") && !n.add("::") && !n.add("[::]") && !n.add("a b") && !n.add("evil@example.com"));
  CHECK(n.count == 5);
  CHECK(n.add("MyDisplay.LOCAL.") && n.has("mydisplay.local"));
  CHECK(!n.has("MyDisplay.LOCAL."));                                      // lookups take the canonical form
  Names full;
  for (size_t i = 0; i < NAMES_MAX; i++) CHECK(full.add((std::string("host") + (char)('a' + i)).c_str()));
  CHECK(full.count == NAMES_MAX && !full.add("one-too-many") && full.count == NAMES_MAX);
  CHECK(!Names().addDevice(nullptr) && !Names().addDevice(""));
  CHECK(!Names().addDevice(std::string(95, 'a').c_str()));                // name plus ".local" would not fit
  Names odd;
  CHECK(!odd.addDevice("bad name") && odd.count == 0);                          // an unusable name adds nothing
  Names ap = setupNames();
  CHECK(ap.has("192.168.4.1") && ap.has("ledmatrix.local"));
}

// ---- the Host check (every request) ---------------------------------------------------------------------------------------------------
static void testHost() {
  SECTION("host: the display's own names and addresses are accepted, on reads");
  for (const char *h : {"ledmatrix.local", "ledmatrix.local:80", "ledmatrix.local:8080", "LEDMATRIX.LOCAL", "LedMatrix.Local:80", "ledmatrix.local.", "ledmatrix.local.:80",
                        "ledmatrix", "ledmatrix:80", "LEDMATRIX", "192.0.2.10", "192.0.2.10:80", "[fe80::1234:5678:9abc:def0]", "[FE80::1234:5678:9ABC:DEF0]:80",
                        "[fe80:0:0:0:1234:5678:9abc:def0]", "[fe80::1234:5678:9abc:def0%25en0]", "[2001:db8::10]", "[2001:db8:0:0:0:0:0:10]:80", "  ledmatrix.local  "}) {
    if (!allowed(get(h))) printf("    refused: \"%s\" (%s)\n", h, reason(get(h)));
    CHECK(allowed(get(h)));
    CHECK(allowed(post(h)));  // a POST with no Origin and no Referer: the same answer
  }

  SECTION("host: anything else is refused with 421, on reads too");
  for (const char *h : {"evil.example", "evil.example:80", "EVIL.EXAMPLE", "example.com", "localhost", "localhost:80", "127.0.0.1", "127.0.0.1:80", "[::1]", "0.0.0.0",
                        "192.0.2.11", "192.0.2.1", "192.168.4.1", "10.0.0.1", "[fe80::1]", "[2001:db8::11]", "[::]",
                        "ledmatrix.local.evil.example", "ledmatrix.evil.example", "evil.example.ledmatrix.local", "xledmatrix.local", "ledmatrix.locale", "ledmatrix-local",
                        "ledmatrix.local.local", "ledmatrix.lan", "ledmatrix.home", "ledmatrix.fritz.box", "local", ".local",
                        "evil.example@ledmatrix.local", "ledmatrix.local@evil.example", "user:pw@ledmatrix.local", "ledmatrix.local:80@evil.example",
                        "ledmatrix.local\\.evil.example", "ledmatrix.local/", "ledmatrix.local, evil.example", "ledmatrix.local evil.example", "evil.example ledmatrix.local",
                        "ledmatrix.local:80:80", "ledmatrix.local:", "ledmatrix.local:99999", "192.0.2.10.evil.example", "192.0.2.010", "192.0.2.10.", "0xC0.0.2.10",
                        "3221226002", "[fe80::1234:5678:9abc:def0", "fe80::1234:5678:9abc:def0", "[::ffff:192.0.2.10]", "ledmatrix.local\x01", "ledmatrix.local%00"}) {
    Verdict v = get(h);
    // 192.0.2.10. has a trailing dot: that is the same address, so it is accepted; everything else in the list is not ours.
    if (strcmp(h, "192.0.2.10.") == 0) { CHECK(allowed(v)); continue; }
    if (v != Verdict::HostForeign) printf("    wrong verdict for \"%s\": %s\n", h, reason(v));
    CHECK(v == Verdict::HostForeign);
    CHECK(httpStatus(v) == 421);
    CHECK(post(h) == Verdict::HostForeign);   // a POST gets the host answer first
  }

  SECTION("host: missing, empty, very long");
  CHECK(get(nullptr) == Verdict::HostMissing);
  CHECK(get("") == Verdict::HostMissing);
  CHECK(get("   ") == Verdict::HostMissing);
  CHECK(httpStatus(Verdict::HostMissing) == 421);
  CHECK(post(nullptr) == Verdict::HostMissing);
  CHECK(get(std::string(HEADER_MAX + 1, 'a').c_str()) == Verdict::HostForeign);
  CHECK(get((std::string(HEADER_MAX, 'a') + ".local").c_str()) == Verdict::HostForeign);
  CHECK(get(std::string(1000000, 'a').c_str()) == Verdict::HostForeign);
  CHECK(get(("ledmatrix.local" + std::string(1000, ' ') + "x").c_str()) == Verdict::HostForeign);
  const char withNul[] = "ledmatrix.local\0.evil.example";
  CHECK(check(station(), true, false, Text(withNul, sizeof(withNul) - 1), Text(), Text()) == Verdict::HostForeign);   // a NUL hides nothing

  SECTION("host: the display's addresses are read fresh (a new DHCP address, a lost IPv6 address)");
  Names moved;
  moved.addDevice("ledmatrix");
  moved.add("192.0.2.77");
  moved.add("0.0.0.0");   // the unset address is not a name
  CHECK(allowed(check(moved, true, false, Text("192.0.2.77"), Text(), Text())));
  CHECK(check(moved, true, false, Text("192.0.2.10"), Text(), Text()) == Verdict::HostForeign);
  CHECK(check(moved, true, false, Text("0.0.0.0"), Text(), Text()) == Verdict::HostForeign);
  CHECK(check(moved, true, false, Text("[2001:db8::10]"), Text(), Text()) == Verdict::HostForeign);

  SECTION("host: a renamed display answers to its new name only");
  Names renamed;
  renamed.addDevice("kitchen-board");
  renamed.add("192.0.2.10");
  CHECK(allowed(check(renamed, true, false, Text("kitchen-board.local"), Text(), Text())));
  CHECK(allowed(check(renamed, true, false, Text("kitchen-board"), Text(), Text())));
  CHECK(check(renamed, true, false, Text("ledmatrix.local"), Text(), Text()) == Verdict::HostForeign);
  CHECK(allowed(check(renamed, true, false, Text("192.0.2.10"), Text(), Text())));
}

// ---- the Origin / Referer check (state-changing requests) -----------------------------------------------------------------------------
static void testOrigin() {
  const char *H = "ledmatrix.local";
  SECTION("origin: reads are never checked for Origin or Referer");
  CHECK(allowed(check(station(), true, false, Text(H), Text("https://evil.example"), Text("https://evil.example/x"))));
  CHECK(allowed(check(station(), true, false, Text(H), Text("null"), Text())));

  SECTION("origin: no Origin and no Referer keeps working (curl, scripts)");
  CHECK(allowed(post(H)));
  CHECK(allowed(post(H, nullptr, nullptr)));
  CHECK(allowed(post(H, "", "")));
  CHECK(allowed(post(H, "   ", "  ")));

  SECTION("origin: the portal's own page (same origin) is accepted");
  for (const char *o : {"http://ledmatrix.local", "http://ledmatrix.local:80", "HTTP://LEDMATRIX.LOCAL", "http://LedMatrix.local", "http://ledmatrix.local.",
                        "http://ledmatrix", "http://192.0.2.10", "http://192.0.2.10:80", "http://[fe80::1234:5678:9abc:def0]", "http://[FE80:0:0:0:1234:5678:9ABC:DEF0]:80",
                        "http://[2001:db8::10]", "https://ledmatrix.local", "http://ledmatrix.local:8081", "  http://ledmatrix.local  "}) {
    if (!allowed(post(H, o))) printf("    refused origin: \"%s\" (%s)\n", o, reason(post(H, o)));
    CHECK(allowed(post(H, o)));
    CHECK(allowed(post("192.0.2.10", o)));            // the page may have been opened by name and the request go to the address, or the reverse
    CHECK(allowed(post("[2001:db8::10]:80", o)));
  }

  SECTION("origin: another site is refused with 403");
  for (const char *o : {"https://evil.example", "http://evil.example", "http://evil.example:80", "http://EVIL.EXAMPLE", "http://example.com", "http://localhost", "http://127.0.0.1",
                        "http://192.0.2.11", "http://192.168.4.1", "http://[fe80::1]", "http://[::1]", "http://ledmatrix.local.evil.example", "http://ledmatrix.evil.example",
                        "http://evil.example.ledmatrix.local", "http://xledmatrix.local", "http://ledmatrix.lan", "http://192.0.2.10.evil.example", "http://192.0.2.100"}) {
    Verdict v = post(H, o);
    if (v != Verdict::OriginForeign) printf("    wrong verdict for origin \"%s\": %s\n", o, reason(v));
    CHECK(v == Verdict::OriginForeign);
    CHECK(httpStatus(v) == 403);
  }

  SECTION("origin: userinfo, path, scheme and other tricks");
  for (const char *o : {"http://evil.example@ledmatrix.local", "http://ledmatrix.local@evil.example", "http://user:pw@ledmatrix.local", "http://ledmatrix.local:80@evil.example",
                        "http://ledmatrix.local\\@evil.example", "http://evil.example\\@ledmatrix.local", "http://ledmatrix.local/", "http://ledmatrix.local/x", "http://ledmatrix.local?x",
                        "http://ledmatrix.local#x", "ledmatrix.local", "//ledmatrix.local", "ftp://ledmatrix.local", "file://ledmatrix.local", "chrome-extension://ledmatrix.local",
                        "http://", "https://", "http:///", "http://:80", "http://ledmatrix.local:", "http://ledmatrix.local:99999", "http://ledmatrix.local:80:80",
                        "http://ledmatrix .local", "http://ledmatrix.local\x01", "http://ledmatrix.local, http://evil.example", "http://ledmatrix.local http://evil.example",
                        "http://evil.example http://ledmatrix.local", "https://ledmatrix.local.evil.example/", "http://[::1", "http://::1", "http://ledmatrix.local%00"}) {
    Verdict v = post(H, o);
    if (v != Verdict::OriginMalformed) printf("    wrong verdict for origin \"%s\": %s\n", o, reason(v));
    CHECK(v == Verdict::OriginMalformed);
    CHECK(httpStatus(v) == 403);
  }

  SECTION("origin: null");
  CHECK(post(H, "null") == Verdict::OriginNull);
  CHECK(post(H, "NULL") == Verdict::OriginNull);
  CHECK(post(H, " null ") == Verdict::OriginNull);
  CHECK(httpStatus(Verdict::OriginNull) == 403);
  CHECK(post(H, "null", "http://ledmatrix.local/") == Verdict::OriginNull);   // an own Referer does not rescue a null Origin
  CHECK(post(H, "nul") == Verdict::OriginMalformed);
  CHECK(post(H, "nullx") == Verdict::OriginMalformed);

  SECTION("origin: very long");
  CHECK(post(H, ("http://" + std::string(HEADER_MAX + 1, 'a')).c_str()) == Verdict::OriginMalformed);
  CHECK(post(H, (std::string("http://ledmatrix.local:80") + std::string(100000, '0')).c_str()) == Verdict::OriginMalformed);
  CHECK(post(H, std::string(1000000, 'x').c_str()) == Verdict::OriginMalformed);
  const char withNul[] = "http://ledmatrix.local\0.evil.example";
  CHECK(check(station(), true, true, Text(H), Text(withNul, sizeof(withNul) - 1), Text()) == Verdict::OriginMalformed);

  SECTION("referer: used only when there is no Origin");
  for (const char *r : {"http://ledmatrix.local/", "http://ledmatrix.local", "http://ledmatrix.local/index.html", "http://ledmatrix.local:80/?a=b#c", "HTTP://LEDMATRIX.LOCAL/x",
                        "http://192.0.2.10/", "http://[fe80::1234:5678:9abc:def0]/", "http://[2001:db8::10]:80/page", "https://ledmatrix.local/x", "  http://ledmatrix.local/  "}) {
    if (!allowed(post(H, nullptr, r))) printf("    refused referer: \"%s\" (%s)\n", r, reason(post(H, nullptr, r)));
    CHECK(allowed(post(H, nullptr, r)));
    CHECK(allowed(post(H, "", r)));                  // an empty Origin counts as none
  }
  for (const char *r : {"https://evil.example/", "http://evil.example", "http://evil.example/ledmatrix.local", "http://evil.example/?u=http://ledmatrix.local/",
                        "http://evil.example#http://ledmatrix.local/", "http://ledmatrix.local.evil.example/", "http://192.0.2.11/", "http://localhost/",
                        "http://ledmatrix.lan/"}) {
    Verdict v = post(H, nullptr, r);
    if (v != Verdict::RefererForeign) printf("    wrong verdict for referer \"%s\": %s\n", r, reason(v));
    CHECK(v == Verdict::RefererForeign);
    CHECK(httpStatus(v) == 403);
  }
  for (const char *r : {"http://evil.example@ledmatrix.local/", "http://ledmatrix.local@evil.example/", "http://user:pw@ledmatrix.local/", "http://ledmatrix.local\\@evil.example/",
                        "ledmatrix.local/", "/page", "about:blank", "android-app://com.example.app", "ftp://ledmatrix.local/", "http://", "garbage", "http://ledmatrix.local:99999/"}) {
    Verdict v = post(H, nullptr, r);
    if (v != Verdict::RefererMalformed) printf("    wrong verdict for referer \"%s\": %s\n", r, reason(v));
    CHECK(v == Verdict::RefererMalformed);
    CHECK(httpStatus(v) == 403);
  }
  CHECK(allowed(post(H, nullptr, ("http://ledmatrix.local/" + std::string(100000, 'p')).c_str())));          // a long path after a good host
  CHECK(post(H, nullptr, ("http://evil.example/" + std::string(100000, 'p')).c_str()) == Verdict::RefererForeign);
  CHECK(post(H, nullptr, ("http://" + std::string(100000, 'a') + ".example/").c_str()) == Verdict::RefererMalformed);

  SECTION("origin wins over referer");
  CHECK(post(H, "https://evil.example", "http://ledmatrix.local/") == Verdict::OriginForeign);   // an own Referer does not rescue a foreign Origin
  CHECK(allowed(post(H, "http://ledmatrix.local", "https://evil.example/")));                    // the Origin is the one that counts
  CHECK(allowed(post(H, "http://ledmatrix.local", "garbage")));
  CHECK(post(H, "garbage", "http://ledmatrix.local/") == Verdict::OriginMalformed);

  SECTION("origin: the same host as the Host header is the same origin (setup mode: any Host a phone sends)");
  Names ap = setupNames();
  auto setupPost = [&](const char *host, const char *origin, const char *referer = nullptr) {
    return check(ap, false, true, Text(host), Text(origin), Text(referer));
  };
  CHECK(allowed(setupPost("192.168.4.1", "http://192.168.4.1")));                // the setup form, at the setup address
  CHECK(allowed(setupPost("192.168.4.1:80", "http://192.168.4.1:80")));
  CHECK(allowed(setupPost("192.168.4.1", nullptr)));                              // no Origin: keeps working
  CHECK(allowed(setupPost("captive.apple.com", "http://captive.apple.com")));     // the form served to a probe's own Host, posted back to it
  CHECK(allowed(setupPost("neverssl.com:80", "http://NEVERSSL.com")));
  CHECK(allowed(setupPost("example.net", nullptr, "http://example.net/")));
  CHECK(allowed(setupPost("ledmatrix.local", "http://192.168.4.1")));             // the page at the setup address, a request to a name
  CHECK(setupPost("192.168.4.1", "https://evil.example") == Verdict::OriginForeign);   // another page posting to the setup address
  CHECK(setupPost("captive.apple.com", "https://evil.example") == Verdict::OriginForeign);
  CHECK(setupPost("192.168.4.1", "null") == Verdict::OriginNull);
  CHECK(setupPost("192.168.4.1", nullptr, "https://evil.example/") == Verdict::RefererForeign);
  CHECK(setupPost(nullptr, "http://192.168.4.1") == Verdict::Allow);              // no Host header at all: the own names alone decide
  CHECK(setupPost(nullptr, "http://evil.example") == Verdict::OriginForeign);
  CHECK(setupPost("evil.example@192.168.4.1", "http://evil.example") == Verdict::OriginForeign);   // an unusable Host is no match
  CHECK(allowed(check(ap, false, false, Text("captive.apple.com"), Text(), Text())));              // reads: no Host check in setup mode ...
  CHECK(allowed(check(ap, false, false, Text(), Text(), Text())));
  CHECK(allowed(check(ap, false, false, Text("evil.example"), Text("https://evil.example"), Text())));   // ... and no Origin check on reads
  // In station mode the same-host rule is no loophole: the Host must be the display's own first.
  CHECK(check(station(), true, true, Text("evil.example"), Text("http://evil.example"), Text()) == Verdict::HostForeign);
  CHECK(check(station(), true, true, Text("evil.example"), Text("http://ledmatrix.local"), Text()) == Verdict::HostForeign);
}

static void testVerdicts() {
  SECTION("verdicts: status and reason codes");
  const Verdict all[] = {Verdict::Allow, Verdict::HostMissing, Verdict::HostForeign, Verdict::OriginNull, Verdict::OriginMalformed, Verdict::OriginForeign,
                         Verdict::RefererMalformed, Verdict::RefererForeign};
  CHECK(allowed(Verdict::Allow) && httpStatus(Verdict::Allow) == 200);
  for (Verdict v : all) {
    if (v == Verdict::Allow) continue;
    CHECK(!allowed(v));
    CHECK(httpStatus(v) == 403 || httpStatus(v) == 421);
    CHECK(strlen(reason(v)) > 0 && strlen(reason(v)) < 24);
    for (const char *p = reason(v); *p; p++) CHECK((*p >= 'a' && *p <= 'z') || *p == '-');   // a fixed lower-case word, never header text
  }
  CHECK(httpStatus(Verdict::HostMissing) == 421 && httpStatus(Verdict::HostForeign) == 421);
  CHECK(httpStatus(Verdict::OriginNull) == 403 && httpStatus(Verdict::OriginMalformed) == 403 && httpStatus(Verdict::OriginForeign) == 403);
  CHECK(httpStatus(Verdict::RefererMalformed) == 403 && httpStatus(Verdict::RefererForeign) == 403);
  CHECK(strcmp(reason(Verdict::HostForeign), "host") == 0 && strcmp(reason(Verdict::OriginForeign), "origin") == 0 && strcmp(reason(Verdict::RefererForeign), "referer") == 0);
  // No two refusals share a code.
  for (Verdict a : all)
    for (Verdict b : all)
      if (a != b) CHECK(strcmp(reason(a), reason(b)) != 0);
  // The reason codes carry no header value: a refusal for a distinctive Host and Origin never contains them.
  Verdict v1 = post("distinctive-host.example", "http://distinctive-origin.example");
  CHECK(v1 == Verdict::HostForeign && strstr(reason(v1), "distinctive") == nullptr);
  Verdict v2 = post("ledmatrix.local", "http://distinctive-origin.example");
  CHECK(v2 == Verdict::OriginForeign && strstr(reason(v2), "distinctive") == nullptr);
}

static void testPrintable() {
  SECTION("log helper");
  char out[16];
  CHECK(printable("/api/settings", 13, out, sizeof(out)) == 13 && strcmp(out, "/api/settings") == 0);
  CHECK(printable("/a\x1b[31mb\r\nc", 11, out, sizeof(out)) == 11 && strcmp(out, "/a?[31mb??c") == 0);   // no escape, no line break survives
  CHECK(printable("/\xc3\xa9", 3, out, sizeof(out)) == 3 && strcmp(out, "/??") == 0);
  CHECK(printable("a\0b", 3, out, sizeof(out)) == 3 && out[1] == '?' && out[3] == 0);
  CHECK(printable("0123456789abcdefXYZ", 19, out, sizeof(out)) == 15 && strcmp(out, "0123456789abcde") == 0);   // cut to the buffer
  CHECK(printable("x", 1, out, 1) == 0 && out[0] == 0);
  CHECK(printable(nullptr, 5, out, sizeof(out)) == 0 && out[0] == 0);
  CHECK(printable("x", 1, nullptr, 0) == 0);
}

// ---- noise ----------------------------------------------------------------------------------------------------------------------------------
// Random and half-valid headers must never crash (the sanitizers watch), and whatever is let through must be a request that names the display.
static uint32_t rngState = 12345;
static uint32_t rnd() { rngState = rngState * 1664525u + 1013904223u; return rngState >> 8; }

static std::string noise(const char *const *seeds, size_t nseeds) {
  std::string s = (rnd() % 4 == 0) ? std::string() : std::string(seeds[rnd() % nseeds]);
  int edits = (int)(rnd() % 4);
  static const char junk[] = "@:/\\.[]%#? \t\r\n-_0123456789abcdefghijklmnopqrstuvwxyzABCDEFXYZ\x01\x7f\x80\xff";
  while (edits--) {
    size_t pos = s.empty() ? 0 : rnd() % (s.size() + 1);
    switch (rnd() % 3) {
      case 0: s.insert(pos, 1, junk[rnd() % (sizeof(junk) - 1)]); break;
      case 1: if (!s.empty()) s.erase(rnd() % s.size(), 1); break;
      case 2: if (!s.empty()) s[rnd() % s.size()] = junk[rnd() % (sizeof(junk) - 1)]; break;
    }
  }
  return s;
}

static void testNoise() {
  SECTION("noise: no crash, and nothing foreign gets through");
  static const char *const hosts[] = {"ledmatrix.local", "ledmatrix.local:80", "192.0.2.10", "[fe80::1234:5678:9abc:def0]:80", "[2001:db8::10]", "evil.example", "ledmatrix"};
  static const char *const urls[] = {"http://ledmatrix.local", "http://192.0.2.10:80/", "http://[2001:db8::10]", "https://evil.example/x", "null", "http://ledmatrix.local/page?x=y"};
  Names own = station();
  long allowedCount = 0, refusedCount = 0;
  for (int i = 0; i < 300000; i++) {
    std::string h = noise(hosts, 7), o = noise(urls, 6), r = noise(urls, 6);
    bool changing = rnd() % 2;
    bool checkHost = rnd() % 4 != 0;
    Verdict v = check(own, checkHost, changing, Text(h.data(), h.size()), Text(o.data(), o.size()), Text(r.data(), r.size()));
    if (!allowed(v)) { refusedCount++; continue; }
    allowedCount++;
    char hc[CANON_MAX], pc[CANON_MAX];
    bool hostOk = canonHost(Text(h.data(), h.size()), hc, sizeof(hc));
    if (checkHost && !(hostOk && own.has(hc))) { fails++; printf("    FAIL: allowed with a foreign Host\n"); break; }
    if (changing) {
      Text ot = trim(Text(o.data(), o.size())), rt = trim(Text(r.data(), r.size()));
      if (ot.n > 0) {
        bool ok = canonUrlHost(ot, true, pc, sizeof(pc)) && (own.has(pc) || (hostOk && strcmp(pc, hc) == 0));
        if (!ok) { fails++; printf("    FAIL: allowed with a foreign Origin\n"); break; }
      } else if (rt.n > 0) {
        bool ok = canonUrlHost(rt, false, pc, sizeof(pc)) && (own.has(pc) || (hostOk && strcmp(pc, hc) == 0));
        if (!ok) { fails++; printf("    FAIL: allowed with a foreign Referer\n"); break; }
      }
    }
  }
  checks++;
  printf("    %ld allowed, %ld refused\n", allowedCount, refusedCount);
  CHECK(allowedCount > 1000 && refusedCount > 1000);   // the noise is mixed enough to exercise both sides
}

int main() {
  testCanonHost();
  testCanonUrl();
  testNames();
  testHost();
  testOrigin();
  testVerdicts();
  testPrintable();
  testNoise();
  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
