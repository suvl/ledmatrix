// Host test for arduino/ledmatrix/otalogic.h (firmware update logic, slice 10 / ticket 33): versions, the manifest rules, the
// authorisation code and its throttle, the update flow, the check schedule, the probation checklist and the words for every failure.
// Run from the repo root (the ArduinoJson path is the usual Arduino library folder):
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" tools/test_otalogic.cpp -o <scratch>/test_otalogic && <scratch>/test_otalogic
// Also with the sanitizers (they catch buffer overruns, bad shifts and signed overflow):
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined -Iarduino/ledmatrix -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" tools/test_otalogic.cpp -o <scratch>/test_otalogic_asan && <scratch>/test_otalogic_asan
// With the path of a real build as the one argument it also checks that image (the version marker, the header).
// Every version, hash, host and code in here is a placeholder.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "otalogic.h"
#include "version.h"

using namespace otalogic;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(name) printf("%s\n", name)

static const uint32_t SEC = 1000;
static const uint32_t MIN = 60 * SEC;

// ---- versions -----------------------------------------------------------------------------------------------------------
static bool pv(const char *s, Version &v) { return parseVersion(s, v); }

static void testVersions() {
  SECTION("versions: parse");
  Version v;
  CHECK(pv("0.10.0", v) && v.major == 0 && v.minor == 10 && v.patch == 0);
  CHECK(pv("1.2.3", v) && v.major == 1 && v.minor == 2 && v.patch == 3);
  CHECK(pv("9999.9999.9999", v) && v.major == 9999 && v.minor == 9999 && v.patch == 9999);
  CHECK(pv("0.0.0", v) && v.major == 0 && v.minor == 0 && v.patch == 0);
  CHECK(pv("0001.0002.0003", v) && v.major == 1 && v.minor == 2 && v.patch == 3);  // leading zeros read as numbers (the service accepts them)
  CHECK(pv("01.02.03", v) && v.minor == 2);
  Version keep;
  keep.major = 7; keep.minor = 8; keep.patch = 9;
  for (const char *bad : {"", ".", "..", "1", "1.2", "1.2.", ".1.2", "1..2", "1.2.3.", "1.2.3.4", "1.2.3.4.5", "10000.0.0", "0.10000.0", "0.0.10000",
                          "00000.0.0", "-1.0.0", "+1.0.0", "1.0.0-rc1", "1.0.0+b", "v1.0.0", "1.0.0 ", " 1.0.0", "1. 0.0", "1,0,0", "1.0.0\n", "a.b.c",
                          "1.2.x", "0x1.0.0", "1e1.0.0", "1.0.0\r", "١.٠.٠", "1.2.3\x01"}) {
    Version t = keep;
    CHECK(!parseVersion(bad, t));
    CHECK(t.major == 7 && t.minor == 8 && t.patch == 9);  // untouched on failure
  }
  CHECK(!parseVersion(nullptr, v));
  CHECK(!parseVersion(nullptr, 5, v));
  CHECK(!parseVersion("1.2.3", 0, v));
  CHECK(!parseVersion("1.2.3", 15, v));  // longer than any version can be
  CHECK(!parseVersion("1.2.3", 6, v));   // length past the text: the NUL is not a digit
  CHECK(parseVersion("1.2.3", 5, v));
  CHECK(parseVersion("1.2.3junk", 5, v) && v.patch == 3);  // the length decides how much is read
  CHECK(!parseVersion("1.2.3junk", 9, v));
  CHECK(!parseVersion("12345678901234567890", v));        // an unbounded text is only looked at up to 15 bytes
  std::string longText(5000, '1');
  CHECK(!parseVersion(longText.c_str(), v));

  SECTION("versions: compare and format");
  Version a, b;
  pv("0.10.0", a); pv("0.10.1", b);
  CHECK(compareVersions(a, b) < 0 && compareVersions(b, a) > 0 && compareVersions(a, a) == 0);
  pv("0.9.9", a); pv("0.10.0", b);
  CHECK(compareVersions(a, b) < 0);  // numbers, not text: 9 < 10
  pv("1.0.0", a); pv("0.9999.9999", b);
  CHECK(compareVersions(a, b) > 0);
  pv("2.0.0", a); pv("1.9999.9999", b);
  CHECK(compareVersions(a, b) > 0);
  pv("1.2.3", a); pv("01.002.0003", b);
  CHECK(compareVersions(a, b) == 0);
  char buf[16];
  pv("0001.0010.0000", a);
  formatVersion(a, buf, sizeof buf);
  CHECK(strcmp(buf, "1.10.0") == 0);
  pv("9999.9999.9999", a);
  formatVersion(a, buf, sizeof buf);
  CHECK(strcmp(buf, "9999.9999.9999") == 0 && strlen(buf) == VERSION_TEXT_MAX);

  SECTION("versions: the firmware's own version is valid");
  Version own;
  CHECK(pv(FIRMWARE_VERSION, own));
  CHECK(strlen(FIRMWARE_VERSION) <= VERSION_TEXT_MAX);
}

static void testOffered() {
  SECTION("is it offered");
  Version run, pub, failed;
  pv("0.10.0", run);
  pv("0.10.1", pub);
  CHECK(isOffered(run, pub, nullptr));
  pv("0.10.0", pub);
  CHECK(!isOffered(run, pub, nullptr));  // the same version is not an update
  pv("0.9.9", pub);
  CHECK(!isOffered(run, pub, nullptr));  // an older one is never offered (no downgrade)
  pv("1.0.0", pub);
  CHECK(isOffered(run, pub, nullptr));
  pv("0.11.0", pub);
  CHECK(isOffered(run, pub, nullptr));
  // a version that failed here is not offered again until a newer one exists
  pv("0.10.1", failed); pv("0.10.1", pub);
  CHECK(!isOffered(run, pub, &failed));
  pv("0.10.2", pub);
  CHECK(isOffered(run, pub, &failed));
  pv("0.10.0", pub);
  CHECK(!isOffered(run, pub, &failed));
  // anything older than the failed one, if it is still newer than the running one, is not offered either
  pv("0.12.0", failed); pv("0.11.0", pub);
  CHECK(!isOffered(run, pub, &failed));
  pv("0.12.0", pub);
  CHECK(!isOffered(run, pub, &failed));
  pv("0.12.1", pub);
  CHECK(isOffered(run, pub, &failed));
  // a failed version below the running one changes nothing
  pv("0.9.0", failed); pv("0.10.1", pub);
  CHECK(isOffered(run, pub, &failed));
}

// ---- the manifest --------------------------------------------------------------------------------------------------------
static const char *SHA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static std::string manifest(const char *version = "0.10.1", const char *file = nullptr, const char *size = "1412537", const char *sha = SHA,
                            const char *notes = "\"first lines of the notes\"") {
  std::string f = file ? file : (std::string("ledmatrix-") + version + ".bin");
  std::string s = "{\"version\":\"" + std::string(version) + "\",\"file\":\"" + f + "\",\"size\":" + size + ",\"sha256\":\"" + sha + "\"";
  if (notes) s += std::string(",\"notes\":") + notes;
  s += "}";
  return s;
}
static ManifestError parse(const std::string &s, Manifest *out = nullptr) {
  Manifest m;
  ManifestError e = parseManifest(s.data(), s.size(), m);
  if (out) *out = m;
  return e;
}

static void testManifestValid() {
  SECTION("manifest: a valid one");
  Manifest m;
  CHECK(parse(manifest(), &m) == ManifestError::None);
  CHECK(strcmp(m.version, "0.10.1") == 0 && m.v.major == 0 && m.v.minor == 10 && m.v.patch == 1);
  CHECK(strcmp(m.file, "ledmatrix-0.10.1.bin") == 0);
  CHECK(m.size == 1412537);
  CHECK(strcmp(m.sha256, SHA) == 0);
  CHECK(strcmp(m.notes, "first lines of the notes") == 0);
  // the contract's own example, with white space around
  CHECK(parse(" {\"version\" : \"0.10.1\" , \"file\":\"ledmatrix-0.10.1.bin\", \"size\": 1412537,\n \"sha256\":\"" + std::string(SHA) + "\", \"notes\": \"x\" } \n", &m) == ManifestError::None);
  // key order does not matter
  std::string reordered = std::string("{\"notes\":\"n\",\"sha256\":\"") + SHA + "\",\"size\":200000,\"file\":\"ledmatrix-1.2.3.bin\",\"version\":\"1.2.3\"}";
  CHECK(parse(reordered, &m) == ManifestError::None && m.size == 200000 && strcmp(m.version, "1.2.3") == 0);
  // the version's own text is kept, and the file must use that text
  CHECK(parse(manifest("0.10.01"), &m) == ManifestError::None && strcmp(m.version, "0.10.01") == 0 && m.v.patch == 1 && strcmp(m.file, "ledmatrix-0.10.01.bin") == 0);
  CHECK(parse(manifest("0.10.01", "ledmatrix-0.10.1.bin")) == ManifestError::File);
  CHECK(parse(manifest("9999.9999.9999"), &m) == ManifestError::None && strcmp(m.file, "ledmatrix-9999.9999.9999.bin") == 0);
  // keys this firmware does not know are ignored (a later service may add one); a missing or null notes is empty
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":\"ledmatrix-1.0.0.bin\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\",\"released\":\"x\",\"extra\":[1,2]}", &m) == ManifestError::None);
  CHECK(strcmp(m.notes, "") == 0);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, nullptr), &m) == ManifestError::None && m.notes[0] == 0);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, "null"), &m) == ManifestError::None && m.notes[0] == 0);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, "\"\""), &m) == ManifestError::None && m.notes[0] == 0);
  // the download path is built from the checked file
  char path[64];
  parse(manifest(), &m);
  CHECK(downloadPath(m, path, sizeof path) && strcmp(path, "/v1/firmware/ledmatrix-0.10.1.bin") == 0);
  CHECK(!downloadPath(m, path, 10));
  CHECK(!downloadPath(m, path, 0));
}

static void testManifestVersionAndFile() {
  SECTION("manifest: version and file");
  for (const char *bad : {"", "1", "1.2", "1.2.3.4", "10000.0.0", "1.0.0-rc1", "v1.0.0", "1.0.0 ", "a.b.c", "../../x"}) {
    std::string s = std::string("{\"version\":\"") + bad + "\",\"file\":\"ledmatrix-" + bad + ".bin\",\"size\":200000,\"sha256\":\"" + SHA + "\",\"notes\":\"\"}";
    CHECK(parse(s) == ManifestError::Version);
  }
  CHECK(parse("{\"file\":\"ledmatrix-1.0.0.bin\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::Version);  // missing
  CHECK(parse("{\"version\":1,\"file\":\"ledmatrix-1.bin\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::Version);  // a number
  CHECK(parse("{\"version\":null,\"file\":\"x\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::Version);
  CHECK(parse("{\"version\":[\"1.0.0\"],\"file\":\"x\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::Version);
  CHECK(parse("{\"version\":\"1.0.0\\u0000\",\"file\":\"x\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::Version);  // an embedded NUL
  for (const char *bad : {"ledmatrix-1.0.1.bin", "ledmatrix-1.0.0.BIN", "Ledmatrix-1.0.0.bin", "ledmatrix-1.0.0.bin ", " ledmatrix-1.0.0.bin", "ledmatrix-1.0.0",
                          "ledmatrix_1.0.0.bin", "ledmatrix-1.0.0.bin/../x", "/v1/firmware/ledmatrix-1.0.0.bin", "../ledmatrix-1.0.0.bin", "ledmatrix-1.0.0.bin\\\\x",
                          "https://example.com/ledmatrix-1.0.0.bin", "example.com/ledmatrix-1.0.0.bin", "ledmatrix-1.0.0.bin?x=1", "ledmatrix-1.0.0.bin#x", "", "ledmatrix-.bin"}) {
    CHECK(parse(manifest("1.0.0", bad)) == ManifestError::File);
  }
  CHECK(parse("{\"version\":\"1.0.0\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::File);  // missing
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":7,\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::File);
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":null,\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::File);
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":\"ledmatrix-1.0.0.bin\\u0000x\",\"size\":200000,\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::File);
}

static void testManifestSize() {
  SECTION("manifest: size");
  // above 100 KB (102400) and at most 2,097,152
  CHECK(parse(manifest("1.0.0", nullptr, "102400")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "102401")) == ManifestError::None);
  CHECK(parse(manifest("1.0.0", nullptr, "2097152")) == ManifestError::None);
  CHECK(parse(manifest("1.0.0", nullptr, "2097153")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "1")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "0")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "-1")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "-200000")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "4294967296")) == ManifestError::Size);   // a 33-bit number must not wrap into range
  CHECK(parse(manifest("1.0.0", nullptr, "4295067696")) == ManifestError::Size);   // 2^32 + 100400: wraps to a valid size if cast too early
  CHECK(parse(manifest("1.0.0", nullptr, "9223372036854775807")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "99999999999999999999999")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "1412537.0")) == ManifestError::Size);    // a whole number, not a fraction
  CHECK(parse(manifest("1.0.0", nullptr, "1412537.5")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "1.412537e6")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "\"1412537\"")) == ManifestError::Size);  // text
  CHECK(parse(manifest("1.0.0", nullptr, "null")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "true")) == ManifestError::Size);
  CHECK(parse(manifest("1.0.0", nullptr, "[1412537]")) == ManifestError::Size);
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":\"ledmatrix-1.0.0.bin\",\"sha256\":\"" + std::string(SHA) + "\"}") == ManifestError::Size);  // missing
  Manifest m;
  CHECK(parse(manifest("1.0.0", nullptr, "2097152"), &m) == ManifestError::None && m.size == 2097152);
}

static void testManifestSha() {
  SECTION("manifest: sha256");
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "")) == ManifestError::Sha256);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0123456789abcdef")) == ManifestError::Sha256);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde")) == ManifestError::Sha256);     // 63
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0")) == ManifestError::Sha256);  // 65
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef")) == ManifestError::Sha256);  // upper case
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0123456789abcdeg0123456789abcdef0123456789abcdef0123456789abcdef")) == ManifestError::Sha256);  // g
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde ")) == ManifestError::Sha256);  // a space
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "0x23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) == ManifestError::Sha256);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")) == ManifestError::None);  // the empty body's hash, a vector
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":\"ledmatrix-1.0.0.bin\",\"size\":200000}") == ManifestError::Sha256);  // missing
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":\"ledmatrix-1.0.0.bin\",\"size\":200000,\"sha256\":12345}") == ManifestError::Sha256);
  CHECK(parse("{\"version\":\"1.0.0\",\"file\":\"ledmatrix-1.0.0.bin\",\"size\":200000,\"sha256\":null}") == ManifestError::Sha256);
  // comparing the board's own digest
  char hex[65];
  uint8_t digest[32];
  for (int i = 0; i < 32; i++) digest[i] = (uint8_t)(i * 8 + 1);
  sha256ToHex(digest, hex);
  CHECK(strlen(hex) == 64 && isLowerHex64(hex, 64));
  CHECK(strncmp(hex, "0109111921", 10) == 0);
  CHECK(sha256Matches(hex, hex));
  char other[65];
  memcpy(other, hex, 65);
  other[63] = other[63] == '0' ? '1' : '0';
  CHECK(!sha256Matches(hex, other));
  memcpy(other, hex, 65);
  other[0] = other[0] == '0' ? '1' : '0';
  CHECK(!sha256Matches(hex, other));
  CHECK(!sha256Matches(hex, ""));
  CHECK(!sha256Matches(nullptr, hex) && !sha256Matches(hex, nullptr));
  CHECK(!sha256Matches(hex, "0123456789abcdef"));
}

static void testManifestNotes() {
  SECTION("manifest: notes");
  Manifest m;
  std::string n200(200, 'a'), n201(201, 'a');
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, ("\"" + n200 + "\"").c_str()), &m) == ManifestError::None && strlen(m.notes) == 200);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, ("\"" + n201 + "\"").c_str())) == ManifestError::Notes);
  // characters, not bytes: 200 two-byte characters are fine, 201 are not
  std::string e200, e201;
  for (int i = 0; i < 200; i++) e200 += "\xC3\xA9";
  e201 = e200 + "\xC3\xA9";
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, ("\"" + e200 + "\"").c_str()), &m) == ManifestError::None && strlen(m.notes) == 400);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, ("\"" + e201 + "\"").c_str())) == ManifestError::Notes);
  // 200 four-byte characters (800 bytes) fit the buffer exactly
  std::string f200;
  for (int i = 0; i < 200; i++) f200 += "\xF0\x9F\x98\x80";
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, ("\"" + f200 + "\"").c_str()), &m) == ManifestError::None && strlen(m.notes) == 800);
  // bytes that start no valid character cannot sneak a longer text into the buffer
  std::string junk(801, '\xF8');
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, ("\"" + junk + "\"").c_str())) == ManifestError::Notes);
  // escapes: a new line and a quote are one character each
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, "\"line one\\nline \\\"two\\\"\""), &m) == ManifestError::None && strcmp(m.notes, "line one\nline \"two\"") == 0);
  CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, "\"a\\u0000b\"")) == ManifestError::Notes);  // an embedded NUL
  // the wrong type
  for (const char *bad : {"5", "true", "[\"a\"]", "{\"a\":1}", "1.5"}) CHECK(parse(manifest("1.0.0", nullptr, "200000", SHA, bad)) == ManifestError::Notes);
  // the notes are copied whole, with a terminator
  CHECK(m.notes[strlen(m.notes)] == 0);
}

static void testManifestShape() {
  SECTION("manifest: the answer itself");
  CHECK(parse("") == ManifestError::NotJson);
  CHECK(parse("not json") == ManifestError::NotJson);
  CHECK(parse("{") == ManifestError::NotJson);
  CHECK(parse("{\"version\":") == ManifestError::NotJson);
  CHECK(parse("<html><body>502 Bad Gateway</body></html>") == ManifestError::NotJson);
  CHECK(parse("[]") == ManifestError::NotObject);
  CHECK(parse("\"text\"") == ManifestError::NotObject);
  CHECK(parse("42") == ManifestError::NotObject);
  CHECK(parse("null") == ManifestError::NotObject);
  CHECK(parse("true") == ManifestError::NotObject);
  CHECK(parse("{}") == ManifestError::Version);
  CHECK(parse("[{\"version\":\"1.0.0\"}]") == ManifestError::NotObject);
  Manifest m;
  CHECK(parseManifest(nullptr, 10, m) == ManifestError::NotJson);
  CHECK(parseManifest("{}", 0, m) == ManifestError::NotJson);
  // too long: the buffer the board reads into is 2048 bytes
  std::string big = manifest("1.0.0", nullptr, "200000", SHA, ("\"" + std::string(150, 'a') + "\"").c_str());
  CHECK(big.size() < MANIFEST_MAX_BYTES && parse(big) == ManifestError::None);
  std::string padded = big + std::string(MANIFEST_MAX_BYTES - big.size(), ' ');
  CHECK(padded.size() == MANIFEST_MAX_BYTES && parse(padded) == ManifestError::None);
  padded += " ";
  CHECK(parse(padded) == ManifestError::TooLong);
  CHECK(parse(std::string(100000, '[')) == ManifestError::TooLong);
  // deep nesting is refused by the parser, not crashed on
  CHECK(parse(std::string(1000, '[')) == ManifestError::NotJson);
  // only as much as the length says is read (the buffer after it is not part of the answer)
  std::string ok = manifest();
  std::string withTail = ok + "garbage after the end";
  CHECK(parseManifest(withTail.data(), ok.size(), m) == ManifestError::None);
  CHECK(parseManifest(withTail.data(), ok.size() - 1, m) == ManifestError::NotJson);  // cut one byte short
  // a failed parse leaves nothing half done for the caller to trust: `out` is only written on success
  Manifest before;
  strcpy(before.version, "marker");
  CHECK(parseManifest("{\"version\":\"1.0.0\"}", 19, before) == ManifestError::File && strcmp(before.version, "marker") == 0);
  // every reason has a short text that fits the problem list's detail
  for (ManifestError e : {ManifestError::TooLong, ManifestError::NotJson, ManifestError::NotObject, ManifestError::Version, ManifestError::File,
                          ManifestError::Size, ManifestError::Sha256, ManifestError::Notes}) {
    CHECK(strlen(manifestErrorText(e)) > 0 && strlen(manifestErrorText(e)) <= 47);
    std::string raw = std::string(PROBLEM_RAW_UPDATE_MANIFEST_INVALID) + ": " + manifestErrorText(e);
    CHECK(raw.size() < 79);  // the whole text fits the detail (80 bytes with the terminator)
  }
  CHECK(strlen(manifestErrorText(ManifestError::None)) == 0);
}

static void testImageHeader() {
  SECTION("image header");
  // the first 24 bytes of a real build of this firmware for this board (6 segments, ESP32-S3, SHA-256 appended)
  uint8_t h[24] = {0xE9, 6, 2, 0x3F, 0xD4, 0x5F, 0x37, 0x40, 0xEE, 0, 0, 0, 9, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 1};
  CHECK(imageHeaderOk(h, sizeof h));
  CHECK(imageHeaderOk(h, 24));
  CHECK(!imageHeaderOk(h, 23));  // too short to judge
  CHECK(!imageHeaderOk(h, 0));
  CHECK(!imageHeaderOk(nullptr, 24));
  uint8_t t[24];
  memcpy(t, h, 24); t[0] = 0xE8;
  CHECK(!imageHeaderOk(t, 24));  // magic
  memcpy(t, h, 24); t[0] = 0x00;
  CHECK(!imageHeaderOk(t, 24));
  memcpy(t, h, 24); t[1] = 0;
  CHECK(!imageHeaderOk(t, 24));  // no segments
  memcpy(t, h, 24); t[1] = 17;
  CHECK(!imageHeaderOk(t, 24));  // more than 16
  memcpy(t, h, 24); t[1] = 16;
  CHECK(imageHeaderOk(t, 24));
  memcpy(t, h, 24); t[4] = t[5] = t[6] = t[7] = 0;
  CHECK(!imageHeaderOk(t, 24));  // no entry point
  memcpy(t, h, 24); t[12] = 0;   // ESP32 (chip id 0)
  CHECK(!imageHeaderOk(t, 24));
  memcpy(t, h, 24); t[12] = 5;   // ESP32-C3
  CHECK(!imageHeaderOk(t, 24));
  memcpy(t, h, 24); t[12] = 2;   // ESP32-S2
  CHECK(!imageHeaderOk(t, 24));
  memcpy(t, h, 24); t[13] = 1;   // 0x0109
  CHECK(!imageHeaderOk(t, 24));
  // text, a zip, an ELF and a PNG are not programs for this chip
  const char *notImages[] = {"<html>", "PK\x03\x04", ("\x7f" "ELF"), "\x89PNG"};
  for (const char *s : notImages) {
    uint8_t buf[64] = {0};
    memcpy(buf, s, strlen(s));
    CHECK(!imageHeaderOk(buf, sizeof buf));
  }
  // a bootloader's header would pass this first look (the ESP-IDF validation and the app description catch it): documented, not hidden
  CHECK(IMAGE_MAGIC == 0xE9 && CHIP_ID_ESP32S3 == 9 && IMAGE_HEADER_LEN == 24);
}

// ---- the authorisation code ----------------------------------------------------------------------------------------------------
struct Script : RandomSource {  // returns the given numbers in turn, then the last one for ever
  std::vector<uint32_t> v;
  size_t i = 0;
  int draws = 0;
  explicit Script(std::vector<uint32_t> values) : v(values) {}
  uint32_t next32() override {
    draws++;
    uint32_t r = v[i < v.size() ? i : v.size() - 1];
    i++;
    return r;
  }
};
struct XorShift : RandomSource {
  uint64_t s = 88172645463325252ull;
  uint32_t next32() override {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return (uint32_t)(s >> 16);
  }
};

static std::string codeOf(const CodeAuth &a) {
  char c[CODE_DIGITS + 1];
  if (!a.codeText(c)) return "";
  return c;
}

static void testCodesEqual() {
  SECTION("code: constant-time comparison");
  CHECK(codesEqual("123456", "123456"));
  CHECK(codesEqual("000000", "000000"));
  CHECK(!codesEqual("123456", "123457"));
  CHECK(!codesEqual("023456", "123456"));
  CHECK(!codesEqual("", "123456"));
  CHECK(!codesEqual("12345", "123456"));
  CHECK(!codesEqual("1234567", "123456"));
  CHECK(!codesEqual("12345 ", "123456"));
  CHECK(!codesEqual(" 123456", "123456"));
  CHECK(!codesEqual("123456 ", "123456"));
  CHECK(!codesEqual("123456\n", "123456"));
  CHECK(!codesEqual("12345a", "123456"));
  CHECK(!codesEqual("abcdef", "123456"));
  CHECK(!codesEqual("1234560", "123456"));
  CHECK(!codesEqual("123456123456", "123456"));
  CHECK(!codesEqual(nullptr, "123456"));
  CHECK(!codesEqual("123456", "123456") == false);
  std::string longText(100000, '1');
  CHECK(!codesEqual(longText.c_str(), "111111"));  // six ones are inside it, but it is not the code
  CHECK(!codesEqual("000000", "000001"));
  // every single wrong digit in every place is different
  for (int pos = 0; pos < 6; pos++)
    for (char d = '0'; d <= '9'; d++) {
      std::string t = "246810";
      t[pos] = d;
      CHECK(codesEqual(t.c_str(), "246810") == (t == "246810"));
    }
}

static void testCodeGeneration() {
  SECTION("code: six digits, leading zeros, no bias");
  CodeAuth a;
  Script r({7});
  CHECK(a.start(Source::Service, 1000, r) == StartResult::Started);
  CHECK(codeOf(a) == "000007");  // leading zeros are kept
  Script z({0});
  CodeAuth b;
  b.start(Source::Service, 0, z);
  CHECK(codeOf(b) == "000000");
  Script big({999999});
  CodeAuth c;
  c.start(Source::Service, 0, big);
  CHECK(codeOf(c) == "999999");
  Script mod({1234567});
  CodeAuth d;
  d.start(Source::Service, 0, mod);
  CHECK(codeOf(d) == "234567");
  // a draw from the top of the 32-bit range (it does not divide evenly) is thrown away and drawn again
  Script top({4294967295u, 4294000000u, 4293999999u});
  CodeAuth e;
  e.start(Source::Service, 0, top);
  CHECK(top.draws == 3 && codeOf(e) == "999999");  // 4293999999 mod 1000000
  Script edge({4294000000u, 17});
  CodeAuth f;
  f.start(Source::Service, 0, edge);
  CHECK(edge.draws == 2 && codeOf(f) == "000017");
  Script ok({4293999999u});
  CodeAuth g;
  g.start(Source::Service, 0, ok);
  CHECK(ok.draws == 1);
  // a source that only ever gives the top of the range still ends (and gives six digits)
  Script stuck({4294967295u});
  CodeAuth h;
  h.start(Source::Service, 0, stuck);
  CHECK(stuck.draws == 33 && codeOf(h).size() == 6);
  // the digits are spread over 0..999999: every place gets every digit, and the first digit is 0 about one time in ten
  XorShift x;
  int firstZero = 0, n = 20000, perPlace[6][10] = {};
  for (int i = 0; i < n; i++) {
    CodeAuth t;
    t.start(Source::Upload, 0, x);
    std::string s = codeOf(t);
    CHECK(s.size() == 6);
    for (int k = 0; k < 6; k++) {
      CHECK(s[k] >= '0' && s[k] <= '9');
      if (s[k] >= '0' && s[k] <= '9') perPlace[k][s[k] - '0']++;
    }
    if (s[0] == '0') firstZero++;
  }
  CHECK(firstZero > n / 10 - 300 && firstZero < n / 10 + 300);
  for (int k = 0; k < 6; k++)
    for (int dgt = 0; dgt < 10; dgt++) CHECK(perPlace[k][dgt] > n / 10 - 400 && perPlace[k][dgt] < n / 10 + 400);
  // two starts give two codes from two draws
  Script two({111111, 222222});
  CodeAuth p;
  p.start(Source::Service, 0, two);
  CHECK(codeOf(p) == "111111");
  p.start(Source::Service, 0, two);
  CHECK(codeOf(p) == "222222");  // a new start replaces the old code
  CHECK(!codesEqual("111111", "222222"));
  CHECK(p.check("111111", 100) == CheckResult::Wrong);
  CHECK(p.check("222222", 200) == CheckResult::Ok);
}

static void testCodeTriesAndExpiry() {
  SECTION("code: tries and expiry");
  Script r({123456});
  CodeAuth a;
  CHECK(!a.pending() && a.source() == Source::None && a.triesLeft() == 0 && a.secondsLeft(0) == 0);
  CHECK(a.check("123456", 0) == CheckResult::NoPending);
  CHECK(a.start(Source::Service, 5000, r) == StartResult::Started);
  CHECK(a.pending() && a.source() == Source::Service && a.triesLeft() == 3);
  CHECK(a.secondsLeft(5000) == 180);
  CHECK(a.secondsLeft(5000 + 1) == 180);
  CHECK(a.secondsLeft(5000 + 1000) == 179);
  CHECK(a.secondsLeft(5000 + 179999) == 1);
  CHECK(a.secondsLeft(5000 + 180000) == 0);
  // the right code at once
  CHECK(a.check("123456", 6000) == CheckResult::Ok);
  CHECK(!a.pending() && a.triesLeft() == 0 && codeOf(a) == "");
  CHECK(a.check("123456", 6001) == CheckResult::NoPending);  // a code is good once

  // two wrong, then right
  a.start(Source::Upload, 10000, r);
  CHECK(a.source() == Source::Upload);
  CHECK(a.check("000000", 11000) == CheckResult::Wrong && a.triesLeft() == 2);
  CHECK(a.check("", 12000) == CheckResult::Wrong && a.triesLeft() == 1);
  CHECK(a.check("123456", 13000) == CheckResult::Ok);
  CHECK(!a.pending());

  // three wrong: used up on the third, the right code is not accepted after
  a.start(Source::Service, 20000, r);
  CHECK(a.check("111111", 21000) == CheckResult::Wrong);
  CHECK(a.check("222222", 22000) == CheckResult::Wrong);
  CHECK(a.check("333333", 23000) == CheckResult::UsedUp);
  CHECK(!a.pending() && a.triesLeft() == 0 && codeOf(a) == "");
  CHECK(a.check("123456", 24000) == CheckResult::NoPending);
  CHECK(a.throttleSecondsLeft(23000) == 60);

  // wrong input of every shape costs a try, the right code last still counts if tries remain
  CodeAuth b;
  Script rb({654321});
  b.start(Source::Service, 0, rb);
  CHECK(b.check(nullptr, 1) == CheckResult::Wrong);
  CHECK(b.check("65432", 2) == CheckResult::Wrong);
  CHECK(b.check("654321", 3) == CheckResult::Ok);

  // expiry: valid for 3 minutes exactly
  CodeAuth e;
  e.start(Source::Service, 1000, r);
  CHECK(e.check("123456", 1000 + 179999) == CheckResult::Ok);
  CodeAuth e2;
  e2.start(Source::Service, 1000, r);
  CHECK(e2.check("123456", 1000 + 180000) == CheckResult::Expired);  // not even the right code
  CHECK(!e2.pending());
  CHECK(e2.check("123456", 1000 + 180001) == CheckResult::NoPending);
  CHECK(e2.throttleSecondsLeft(1000 + 180000) == 0);  // an expiry is not a guessing attempt: no throttle
  // tick says so once
  CodeAuth t;
  t.start(Source::Service, 0, r);
  CHECK(!t.tick(179999));
  CHECK(t.pending());
  CHECK(t.tick(180000));
  CHECK(!t.pending());
  CHECK(!t.tick(180001));
  CHECK(!t.tick(500000));
  // cancel
  CodeAuth c;
  CHECK(!c.cancel());
  c.start(Source::Service, 0, r);
  CHECK(c.cancel() && !c.pending() && codeOf(c) == "");
  CHECK(!c.cancel());
  CHECK(c.check("123456", 10) == CheckResult::NoPending);
  // the code is gone from the object after it is spent
  CodeAuth w;
  w.start(Source::Service, 0, r);
  w.check("123456", 10);
  char out[8] = "xxxxxxx";
  CHECK(!w.codeText(out) && out[0] == 0);
}

static void testCodeClockWrap() {
  SECTION("code: the millisecond clock wraps");
  Script r({424242});
  uint32_t start = 0xFFFFFFFFu - 60000;  // the code starts a minute before the clock wraps
  CodeAuth a;
  a.start(Source::Service, start, r);
  CHECK(a.secondsLeft(start) == 180);
  CHECK(a.secondsLeft(start + 120000) == 60);  // past the wrap
  CHECK(a.check("000000", start + 120000) == CheckResult::Wrong);
  CHECK(a.check("424242", start + 179999) == CheckResult::Ok);
  CodeAuth b;
  b.start(Source::Service, start, r);
  CHECK(b.check("424242", start + 180000) == CheckResult::Expired);
  // the throttle across the wrap
  CodeAuth c;
  uint32_t t0 = 0xFFFFFFFFu - 30000;
  c.start(Source::Service, t0, r);
  c.check("1", t0); c.check("1", t0);
  CHECK(c.check("1", t0) == CheckResult::UsedUp);
  CHECK(c.start(Source::Service, t0 + 59999, r) == StartResult::Throttled);  // 59.999 s later, past the wrap
  CHECK(c.start(Source::Service, t0 + 60000, r) == StartResult::Started);
}

static void testThrottle() {
  SECTION("code: the throttle after a code is used up");
  Script r({100200});
  CodeAuth a;
  uint32_t now = 1000;
  auto useUp = [&](uint32_t at) {  // start (must be accepted) and waste the three tries
    CHECK(a.start(Source::Service, at, r) == StartResult::Started);
    CHECK(a.check("0", at) == CheckResult::Wrong);
    CHECK(a.check("0", at) == CheckResult::Wrong);
    CHECK(a.check("0", at) == CheckResult::UsedUp);
  };
  useUp(now);
  // first: 60 seconds
  CHECK(a.throttleSecondsLeft(now) == 60);
  CHECK(a.start(Source::Service, now, r) == StartResult::Throttled);
  CHECK(a.start(Source::Upload, now + 59999, r) == StartResult::Throttled);
  CHECK(!a.pending());  // a refused start leaves nothing pending
  CHECK(a.throttleSecondsLeft(now + 30000) == 30);
  CHECK(a.throttleSecondsLeft(now + 60000) == 0);
  // the next one: 120, 240, 480, 960, 1920, then 3600 and it stays there
  const uint32_t want[] = {120, 240, 480, 960, 1920, 3600, 3600, 3600};
  for (uint32_t secs : want) {
    now += 60000 * 1000u > 0 ? 0 : 0;
    uint32_t at = now + a.throttleSecondsLeft(now) * 1000;  // the earliest moment a start is accepted
    useUp(at);
    CHECK(a.throttleSecondsLeft(at) == secs);
    CHECK(a.start(Source::Service, at + secs * 1000 - 1, r) == StartResult::Throttled);
    now = at;
  }
  // a start is accepted at the exact end; a wrong try after it starts the doubling over only when a code was right in between
  uint32_t at = now + 3600 * 1000u;
  CHECK(a.start(Source::Service, at, r) == StartResult::Started);
  CHECK(a.check("100200", at + 1000) == CheckResult::Ok);  // a correct code resets the throttle
  useUp(at + 2000);
  CHECK(a.throttleSecondsLeft(at + 2000) == 60);
  // an expired code does not count as used up, and does not reset the throttle level
  CodeAuth e;
  e.start(Source::Service, 0, r);
  CHECK(e.check("1", 1) == CheckResult::Wrong);
  CHECK(e.check("1", 2) == CheckResult::Wrong);
  CHECK(e.check("1", 3) == CheckResult::UsedUp);
  CHECK(e.start(Source::Service, 60003, r) == StartResult::Started);
  CHECK(e.check("1", 60004) == CheckResult::Wrong);
  CHECK(e.check("100200", 60005) == CheckResult::Ok);  // success: level 0 again
  CHECK(e.start(Source::Service, 60006, r) == StartResult::Started);
  e.check("1", 60007); e.check("1", 60008);
  CHECK(e.check("1", 60009) == CheckResult::UsedUp);
  CHECK(e.throttleSecondsLeft(60009) == 60);  // not 120: the success reset it
}

static void testThrottleAging() {
  SECTION("code: a throttle does not come back as the clock runs on");
  const uint32_t DAY = 24u * 3600 * 1000;
  CHECK(sinceMs(1000, 400) == 600);
  CHECK(sinceMs(5, 0xFFFFFFFFu) == 6);                 // across the wrap
  CHECK(sinceMs(1000, 1000) == 0);
  CHECK(sinceMs(1000, 1000 + 3000) == 0);              // a time a few seconds ahead counts as now, not as 49 days ago
  CHECK(sinceMs(0, 0xFFFFFFFFu - 100) == 101);
  CHECK(sinceMs(30 * DAY, 0) == 30 * DAY);             // exact far beyond the 24.9 days a signed difference can hold
  CHECK(sinceMs(49 * DAY, 0) == 49 * DAY);
  Script r({100200});
  // start before, across and long after the clock's wrap; the lock is over after its 60 seconds and stays over
  for (uint32_t t0 : {1000u, 123456789u, 0xFFFFFFFFu - 30000u, 0x7FFFFFF0u}) {
    CodeAuth a;
    a.start(Source::Service, t0, r);
    a.check("0", t0); a.check("0", t0);
    CHECK(a.check("0", t0) == CheckResult::UsedUp);
    CHECK(a.throttleSecondsLeft(t0) == 60);
    CHECK(a.start(Source::Service, t0 + 59999, r) == StartResult::Throttled);
    // aged 30 days with no call at all in between: not locked, and a start is accepted
    CHECK(a.throttleSecondsLeft(t0 + 30 * DAY) == 0);
    CHECK(a.start(Source::Service, t0 + 30 * DAY, r) == StartResult::Started);
    // the same with the board's half-second tick in between: it ends the lock as soon as it is over
    CodeAuth b;
    b.start(Source::Service, t0, r);
    b.check("0", t0); b.check("0", t0); b.check("0", t0);
    CHECK(b.throttleSecondsLeft(t0 + 59999) == 1);
    b.tick(t0 + 60000);
    CHECK(b.throttleSecondsLeft(t0 + 60000) == 0);
    // ... for 60 days of ticks every ten minutes it is never locked again (this is the one that failed after 24.9 days)
    for (uint32_t t = 60000; t <= 60 * DAY; t += 10 * 60 * 1000) {
      b.tick(t0 + t);
      if (b.throttleSecondsLeft(t0 + t) != 0) { CHECK(false); break; }
    }
    CHECK(b.start(Source::Upload, t0 + 60 * DAY, r) == StartResult::Started);
    // a longer lock (the second one: 120 s) ages the same way
    CodeAuth c;
    for (int round = 0; round < 2; round++) {
      uint32_t at = t0 + (uint32_t)round * 61000;
      CHECK(c.start(Source::Service, at, r) == StartResult::Started);
      c.check("0", at); c.check("0", at);
      CHECK(c.check("0", at) == CheckResult::UsedUp);
    }
    CHECK(c.throttleSecondsLeft(t0 + 61000) == 120);
    CHECK(c.throttleSecondsLeft(t0 + 61000 + 30 * DAY) == 0);
    CHECK(c.start(Source::Service, t0 + 61000 + 30 * DAY, r) == StartResult::Started);
    // a code that nobody ticks still expires after 30 days, and is not "fresh" again
    CodeAuth d;
    d.start(Source::Service, t0, r);
    CHECK(d.secondsLeft(t0 + 30 * DAY) == 0);
    CHECK(d.check("100200", t0 + 30 * DAY) == CheckResult::Expired);
    CodeAuth e;
    e.start(Source::Service, t0, r);
    CHECK(e.tick(t0 + 30 * DAY));
  }
  // the level of the throttle is kept while it is running down: the next lock after a long time is the doubled one
  CodeAuth f;
  f.start(Source::Service, 0, r);
  f.check("0", 0); f.check("0", 0); f.check("0", 0);
  f.tick(61000);
  CHECK(f.start(Source::Service, 62000, r) == StartResult::Started);
  f.check("0", 62000); f.check("0", 62000);
  CHECK(f.check("0", 62000) == CheckResult::UsedUp);
  CHECK(f.throttleSecondsLeft(62000) == 120);  // not 60 again: only a correct code resets the level
}

static void testStrikes() {
  SECTION("code: starting again is no way round the three tries");
  Script r({100200, 300400, 500600, 700800, 900100});
  CodeAuth a;
  a.start(Source::Service, 0, r);
  CHECK(a.check("0", 1) == CheckResult::Wrong && a.check("0", 2) == CheckResult::Wrong && a.triesLeft() == 1);
  // a new start replaces the code, but the two wrong tries still count: one try left, then the throttle
  CHECK(a.start(Source::Service, 3, r) == StartResult::Started);
  CHECK(codeOf(a) == "300400");
  CHECK(a.triesLeft() == 1);
  CHECK(a.check("0", 4) == CheckResult::UsedUp);
  CHECK(a.start(Source::Service, 5, r) == StartResult::Throttled);
  CHECK(a.throttleSecondsLeft(4) == 60);
  // the same through cancel
  CodeAuth b;
  b.start(Source::Upload, 0, r);
  b.check("0", 1);
  b.check("0", 2);
  CHECK(b.cancel());
  CHECK(b.start(Source::Upload, 3, r) == StartResult::Started && b.triesLeft() == 1);
  CHECK(b.check("0", 4) == CheckResult::UsedUp);
  // one wrong try carries over once: 2 left
  CodeAuth c;
  c.start(Source::Service, 0, r);
  c.check("0", 1);
  c.start(Source::Service, 2, r);
  CHECK(c.triesLeft() == 2);
  c.check("0", 3);
  c.start(Source::Service, 4, r);
  CHECK(c.triesLeft() == 1);
  // a correct code clears the strikes
  CHECK(c.check(codeOf(c).c_str(), 5) == CheckResult::Ok);
  c.start(Source::Service, 6, r);
  CHECK(c.triesLeft() == 3);
  // an expired code clears them too (the owner who walked away comes back to three tries)
  CodeAuth d;
  d.start(Source::Service, 0, r);
  d.check("0", 1);
  d.check("0", 2);
  CHECK(d.start(Source::Service, 180000, r) == StartResult::Started && d.triesLeft() == 3);  // the old code had run out; start notices
  CodeAuth e;
  e.start(Source::Service, 0, r);
  e.check("0", 1);
  e.check("0", 2);
  CHECK(e.tick(180000));
  e.start(Source::Service, 180001, r);
  CHECK(e.triesLeft() == 3);
}

// ---- the flow ------------------------------------------------------------------------------------------------------------------
static void testFlowAllow() {
  SECTION("flow: which calls are legal in which state");
  struct Row { State base; bool checking; Refusal check, startSvc, startUp, confirm, upload, cancel; };
  const Refusal N = Refusal::None;
  // `offer` is true for the Start(service) column unless a row says otherwise
  Row rows[] = {
      //  base                 checking  Check                      Start(service) Start(upload)       Confirm                 Upload                  Cancel
      {State::Idle,        false, N,                          N,                N,                 Refusal::NoPending,     Refusal::NoPending,     N},
      {State::Idle,        true,  Refusal::AlreadyChecking,   N,                N,                 Refusal::NoPending,     Refusal::NoPending,     N},
      {State::WaitingCode, false, N,                          N,                N,                 N,                      Refusal::WrongSource,   N},  // source set below
      {State::Downloading, false, Refusal::Busy,              Refusal::Busy,    Refusal::Busy,     Refusal::Busy,          Refusal::Busy,          Refusal::Busy},
      {State::Installing,  false, Refusal::Busy,              Refusal::Busy,    Refusal::Busy,     Refusal::Busy,          Refusal::Busy,          Refusal::Busy},
      {State::Restarting,  false, Refusal::Busy,              Refusal::Busy,    Refusal::Busy,     Refusal::Busy,          Refusal::Busy,          Refusal::Busy},
      {State::Failed,      false, N,                          N,                N,                 Refusal::NoPending,     Refusal::NoPending,     N},
      {State::Probation,   false, N,                          N,                N,                 Refusal::NoPending,     Refusal::NoPending,     N},  // the FLOW's state alone changes nothing
  };
  for (const Row &r : rows) {
    Flow f;
    f.base = r.base;
    f.checking = r.checking;
    if (r.base == State::WaitingCode) f.source = Source::Service;
    CHECK(f.allow(Call::Check, true) == r.check);
    CHECK(f.allow(Call::Start, true, Source::Service) == r.startSvc);
    CHECK(f.allow(Call::Start, true, Source::Upload) == r.startUp);
    CHECK(f.allow(Call::Confirm, true, Source::Service) == r.confirm);
    CHECK(f.allow(Call::Upload, true, Source::Upload) == r.upload);
    CHECK(f.allow(Call::Cancel, true) == r.cancel);
  }
  // an image that is not yet confirmed (probationActive()) writes nothing: its other slot is the only image to go back to. This is the
  // image's state, not the flow's: after a failed rollback the flow is "failed" and the image is still unconfirmed.
  for (State st : {State::Idle, State::WaitingCode, State::Failed, State::Probation}) {
    Flow f;
    f.base = st;
    if (st == State::WaitingCode) f.source = Source::Service;
    CHECK(f.allow(Call::Start, true, Source::Service, true) == Refusal::Probation);
    CHECK(f.allow(Call::Start, true, Source::Upload, true) == Refusal::Probation);
    CHECK(f.allow(Call::Start, false, Source::Upload, true) == Refusal::Probation);
    CHECK(f.allow(Call::Confirm, true, Source::Service, true) == Refusal::Probation);
    CHECK(f.allow(Call::Upload, true, Source::Upload, true) == Refusal::Probation);
    CHECK(f.allow(Call::Check, true, Source::None, true) == Refusal::None);    // asking the service is harmless
    CHECK(f.allow(Call::Cancel, true, Source::None, true) == Refusal::None);
    CHECK(f.allow(Call::Start, true, Source::Service, false) != Refusal::Probation);  // the same flow without the image's state
  }
  {
    Flow f;
    f.startAuth(Source::Service);
    f.authOk();  // flashing: busy is the plainer reason
    CHECK(f.allow(Call::Start, true, Source::Service, true) == Refusal::Busy);
    CHECK(f.allow(Call::Confirm, true, Source::Service, true) == Refusal::Busy);
  }
  {
    Flow f;  // the scenario of the review: a rollback that could not be done leaves the flow failed, the image unconfirmed
    f.bootProbation();
    f.fail("The new firmware failed its start-up check, but there is no previous firmware to go back to, so it keeps running.");
    CHECK(f.shown() == State::Failed);
    CHECK(f.allow(Call::Start, true, Source::Upload, true) == Refusal::Probation);
    CHECK(f.allow(Call::Start, true, Source::Upload, false) == Refusal::None);
  }
  CHECK(strcmp(refusalText(Refusal::Probation), "The new firmware is still being checked; try again in a few minutes.") == 0);
  // the service's update needs an offer; an uploaded file does not
  Flow idle;
  CHECK(idle.allow(Call::Start, false, Source::Service) == Refusal::NoOffer);
  CHECK(idle.allow(Call::Start, false, Source::Upload) == Refusal::None);
  CHECK(idle.allow(Call::Start, true, Source::Service) == Refusal::None);
  // a pending code is for one source: the other call is refused (and it costs no try: the flow only says "wrong source")
  Flow w;
  w.startAuth(Source::Upload);
  CHECK(w.allow(Call::Confirm, true, Source::Service) == Refusal::WrongSource);
  CHECK(w.allow(Call::Upload, true, Source::Upload) == Refusal::None);
  Flow w2;
  w2.startAuth(Source::Service);
  CHECK(w2.allow(Call::Upload, true, Source::Upload) == Refusal::WrongSource);
  CHECK(w2.allow(Call::Confirm, true, Source::Service) == Refusal::None);
  // a check can run beside a pending code
  CHECK(w2.allow(Call::Check, true) == Refusal::None);
  w2.beginCheck();
  CHECK(w2.shown() == State::WaitingCode);  // the code is what the portal shows
  CHECK(w2.allow(Call::Check, true) == Refusal::AlreadyChecking);
  // every refusal has plain words that end in a full stop
  for (Refusal rf : {Refusal::Busy, Refusal::Probation, Refusal::NoOffer, Refusal::NoPending, Refusal::WrongSource, Refusal::AlreadyChecking}) {
    const char *t = refusalText(rf);
    CHECK(strlen(t) > 10 && t[strlen(t) - 1] == '.' && strlen(t) < 95);
  }
  CHECK(strlen(refusalText(Refusal::None)) == 0);
}

static void testFlowTransitions() {
  SECTION("flow: transitions");
  Flow f;
  CHECK(f.shown() == State::Idle && strcmp(stateName(f.shown()), "idle") == 0 && sourceName(f.source) == nullptr);
  // a check
  f.beginCheck();
  CHECK(f.shown() == State::Checking && strcmp(stateName(f.shown()), "checking") == 0 && f.base == State::Idle);
  f.endCheck();
  CHECK(f.shown() == State::Idle);
  // the service's update, start to finish
  f.startAuth(Source::Service);
  CHECK(f.shown() == State::WaitingCode && f.source == Source::Service && strcmp(sourceName(f.source), "service") == 0);
  f.authOk();
  CHECK(f.shown() == State::Downloading && f.done == 0 && f.total == 0 && f.flashing());
  f.progress(500, 1000);
  CHECK(f.done == 500 && f.total == 1000);
  f.downloaded();
  CHECK(f.shown() == State::Installing && f.flashing());
  f.installed();
  CHECK(f.shown() == State::Restarting && f.flashing());
  f.fail("too late");  // once the new image is set nothing can fail it
  CHECK(f.shown() == State::Restarting && f.error[0] == 0);
  f.progress(1, 2);    // and nothing moves
  CHECK(f.done == 500);

  // a failed download
  Flow g;
  g.startAuth(Source::Service);
  g.authOk();
  g.progress(10, 100);
  g.fail("The firmware download did not finish.");
  CHECK(g.shown() == State::Failed && strcmp(g.error, "The firmware download did not finish.") == 0 && !g.flashing());
  CHECK(strcmp(stateName(State::Failed), "failed") == 0);
  // failed → a new check clears it
  g.beginCheck();
  CHECK(g.base == State::Idle && g.error[0] == 0 && g.source == Source::None && g.shown() == State::Checking);
  g.endCheck();
  // failed → a new start clears it
  Flow g2;
  g2.startAuth(Source::Upload);
  g2.authOk();
  g2.fail("x");
  g2.startAuth(Source::Upload);
  CHECK(g2.shown() == State::WaitingCode && g2.error[0] == 0 && g2.done == 0 && g2.total == 0);

  // code gone: cancel → idle; used up / expired → failed with the reason
  Flow h;
  h.startAuth(Source::Service);
  h.authGone(nullptr);
  CHECK(h.shown() == State::Idle && h.source == Source::None);
  h.startAuth(Source::Service);
  h.authGone("The update was refused.");
  CHECK(h.shown() == State::Failed && strcmp(h.error, "The update was refused.") == 0);
  h.authGone(nullptr);  // not waiting any more: nothing changes
  CHECK(h.shown() == State::Failed);
  Flow h2;
  h2.authGone("nothing was pending");
  CHECK(h2.shown() == State::Idle && h2.error[0] == 0);
  // authOk only from waiting
  Flow i;
  i.authOk();
  CHECK(i.shown() == State::Idle);
  // downloaded / installed only in order
  Flow j;
  j.downloaded(); j.installed();
  CHECK(j.shown() == State::Idle);
  j.startAuth(Source::Service);
  j.downloaded();
  CHECK(j.shown() == State::WaitingCode);
  j.authOk();
  j.installed();
  CHECK(j.shown() == State::Downloading);
  // progress only while downloading
  Flow k;
  k.progress(5, 10);
  CHECK(k.done == 0);
  // probation
  Flow p;
  p.bootProbation();
  CHECK(p.shown() == State::Probation && strcmp(stateName(p.shown()), "probation") == 0);
  p.beginCheck();
  CHECK(p.shown() == State::Probation);  // a check shows as probation: the portal's main message
  p.endCheck();
  p.markedGood();
  CHECK(p.shown() == State::Idle);
  p.markedGood();
  CHECK(p.shown() == State::Idle);
  // the error text is cut safely
  Flow q;
  std::string longText(500, 'e');
  q.fail(longText.c_str());
  CHECK(strlen(q.error) == sizeof(q.error) - 1);
  q.fail(nullptr);
  CHECK(q.error[0] == 0 && q.shown() == State::Failed);
  // names
  CHECK(strcmp(stateName(State::WaitingCode), "waiting_code") == 0 && strcmp(stateName(State::Downloading), "downloading") == 0 &&
        strcmp(stateName(State::Installing), "installing") == 0 && strcmp(stateName(State::Restarting), "restarting") == 0);
  CHECK(strcmp(sourceName(Source::Upload), "upload") == 0 && sourceName(Source::None) == nullptr);
}

static void testProgress() {
  SECTION("progress");
  CHECK(progressPercent(0, 0) == 0);
  CHECK(progressPercent(5, 0) == 0);
  CHECK(progressPercent(0, 1000) == 0);
  CHECK(progressPercent(1, 1000) == 0);
  CHECK(progressPercent(10, 1000) == 1);
  CHECK(progressPercent(500, 1000) == 50);
  CHECK(progressPercent(999, 1000) == 99);
  CHECK(progressPercent(1000, 1000) == 100);
  CHECK(progressPercent(1001, 1000) == 100);
  CHECK(progressPercent(0xFFFFFFFFu, 0xFFFFFFFFu) == 100);
  CHECK(progressPercent(0xFFFFFFFEu, 0xFFFFFFFFu) == 99);  // no overflow in done * 100
  CHECK(progressPercent(2097151, 2097152) == 99);
  CHECK(progressPercent(1048576, 2097152) == 50);
  int last = 0;
  for (uint32_t d = 0; d <= 1412537; d += 997) {  // never goes backwards
    int p = progressPercent(d, 1412537);
    CHECK(p >= last && p <= 100);
    last = p;
  }
  // the words on the panel fit its line (23 characters) and say what is going on
  for (State s : {State::Downloading, State::Installing, State::Restarting})
    for (Source src : {Source::Service, Source::Upload})
      for (bool fm : {false, true}) CHECK(strlen(progressWord(s, src, fm)) > 0 && strlen(progressWord(s, src, fm)) <= 23);
  CHECK(strcmp(progressWord(State::Downloading, Source::Service, false), "Downloading") == 0);
  CHECK(strcmp(progressWord(State::Downloading, Source::Upload, false), "Receiving") == 0);
  CHECK(strcmp(progressWord(State::Downloading, Source::Service, true), "Checking") == 0);
  CHECK(strcmp(progressWord(State::Installing, Source::Service, false), "Installing") == 0);
  CHECK(strcmp(progressWord(State::Restarting, Source::Upload, false), "Restarting") == 0);
  CHECK(strlen(progressWord(State::Idle, Source::None, false)) == 0);
}

// ---- the schedule --------------------------------------------------------------------------------------------------------------
static void testSchedule() {
  SECTION("schedule: the first check, then daily");
  CheckSchedule s;
  // before the clock is set nothing is due
  s.tick(0, false, true);
  CHECK(!s.due(0, false, true) && !s.due(10 * MIN, false, true));
  CHECK(!s.armed);
  // the clock is set at 2 minutes: the first check is about 5 minutes later
  s.tick(2 * MIN, true, true);
  CHECK(s.armed && s.dueAt == 7 * MIN);
  CHECK(!s.due(2 * MIN, true, true) && !s.due(7 * MIN - 1, true, true));
  CHECK(s.due(7 * MIN, true, true));
  s.tick(7 * MIN, true, true);
  CHECK(s.dueAt == 7 * MIN);  // ticking does not move it
  // it ran, and worked: the next one in 24 hours
  s.done(7 * MIN + 3 * SEC, true);
  CHECK(!s.due(7 * MIN + 3 * SEC, true, true));
  uint32_t next = 7 * MIN + 3 * SEC + 24 * 60 * MIN;
  CHECK(!s.due(next - 1, true, true));
  CHECK(s.due(next, true, true));
  CHECK(s.due(next + 5 * MIN, true, true));
  // it ran and failed: an hour later
  s.done(next, false);
  CHECK(!s.due(next + 60 * MIN - 1, true, true) && s.due(next + 60 * MIN, true, true));
  // Check now is due at once, even with the daily one far away, and counts from its own time afterwards
  CheckSchedule n;
  n.tick(0, true, true);
  CHECK(!n.due(1000, true, true));
  n.checkNow();
  CHECK(n.due(1000, true, true));
  CHECK(n.due(1001, true, true));  // still due until it ran
  n.done(2000, true);
  CHECK(!n.due(2001, true, true) && !n.requested);
  CHECK(n.dueAt == 2000 + 24 * 60 * MIN);
  // Check now needs no clock and no address to be asked for (the data task says why it cannot run)
  CheckSchedule m;
  m.checkNow();
  CHECK(m.due(0, false, false));
  // without a service address the daily check waits, and its 5 minutes start when an address appears
  CheckSchedule a;
  a.tick(0, true, false);
  CHECK(a.armed);
  CHECK(!a.due(10 * MIN, true, false));
  a.tick(1000 * MIN, true, false);
  CHECK(!a.due(1000 * MIN, true, false));
  a.tick(1000 * MIN + 1, true, true);
  CHECK(a.dueAt == 1000 * MIN + 5 * MIN);   // from the last tick without an address
  CHECK(!a.due(1004 * MIN, true, true));
  // the address is saved at the first tick it appears: the next tick (500 ms later) must not be due yet
  CHECK(a.due(1005 * MIN + 1, true, true));
  // days without an address (a tick a minute), then one: the 5 minutes count from the last tick without it
  CheckSchedule d;
  uint32_t t = 0;
  for (; t < 3u * 86400000u; t += MIN) d.tick(t, true, false);
  CHECK(!d.due(t, true, false));
  d.tick(t, true, true);
  CHECK(!d.due(t, true, true));
  CHECK(!d.due(t + 4 * MIN - 1, true, true));
  CHECK(d.due(t + 4 * MIN, true, true));
  // the millisecond clock wraps
  CheckSchedule w;
  uint32_t t0 = 0xFFFFFFFFu - 2 * MIN;
  w.tick(t0, true, true);
  CHECK(w.dueAt == t0 + 5 * MIN);  // wrapped
  CHECK(!w.due(t0 + 5 * MIN - 1, true, true));
  CHECK(w.due(t0 + 5 * MIN, true, true));
  w.done(t0 + 5 * MIN, true);
  CHECK(w.due(t0 + 5 * MIN + 24 * 60 * MIN, true, true));
  CHECK(!w.due(t0 + 5 * MIN + 24 * 60 * MIN - 1, true, true));
  CHECK(CHECK_EVERY_MS == 24UL * 3600 * 1000 && FIRST_CHECK_MS == 5 * MIN && CHECK_RETRY_MS == 3600UL * 1000);
}

// ---- probation -----------------------------------------------------------------------------------------------------------------
static HealthInputs allGood() {
  HealthInputs in;
  in.wifiJoined = true;
  in.portalAnswering = true;
  in.framesDrawn = 100;
  in.framesOver1s = 0;
  in.serviceReached = true;
  in.serviceExpected = true;
  return in;
}

static void testHealth() {
  SECTION("health: everything within 3 minutes");
  Health h;
  h.begin(1000);
  CHECK(h.verdict() == HealthVerdict::Pending && h.why()[0] == 0);
  HealthInputs none;
  none.serviceExpected = true;
  CHECK(h.evaluate(1000, none) == HealthVerdict::Pending);
  CHECK(h.evaluate(1000 + 90 * SEC, none) == HealthVerdict::Pending);
  // all at once: good
  Health g;
  g.begin(1000);
  CHECK(g.evaluate(1000 + 30 * SEC, allGood()) == HealthVerdict::Pending);  // everything has happened, but a minute has not passed yet
  CHECK(g.evaluate(1000 + 59999, allGood()) == HealthVerdict::Pending);
  CHECK(g.evaluate(1000 + 60000, allGood()) == HealthVerdict::Good);       // exactly a minute after boot
  CHECK(g.verdict() == HealthVerdict::Good);
  // the verdict is final
  HealthInputs bad = none;
  CHECK(g.evaluate(1000 + 10 * MIN, bad) == HealthVerdict::Good);
  Health fin;
  fin.begin(0);
  HealthInputs stall = allGood();
  stall.framesOver1s = 1;
  CHECK(fin.evaluate(10, stall) == HealthVerdict::Failed);
  CHECK(fin.evaluate(20, allGood()) == HealthVerdict::Failed);  // too late to recover
  CHECK(strcmp(fin.why(), "The panel stalled for more than a second.") == 0);

  SECTION("health: the conditions one by one");
  // each condition missing at the end of the 3 minutes names itself
  struct Case { void (*unset)(HealthInputs &); const char *why; };
  Case cases[] = {
      {[](HealthInputs &i) { i.wifiJoined = false; }, "The board did not join Wi-Fi within 3 minutes of starting."},
      {[](HealthInputs &i) { i.portalAnswering = false; }, "The portal did not answer within 3 minutes of starting."},
      {[](HealthInputs &i) { i.framesDrawn = 99; }, "The panel did not draw 100 frames within 3 minutes of starting."},
      {[](HealthInputs &i) { i.serviceReached = false; }, "The service did not answer within 3 minutes of starting."},
  };
  for (const Case &c : cases) {
    Health x;
    x.begin(5000);
    HealthInputs in = allGood();
    c.unset(in);
    CHECK(x.evaluate(5000 + 60 * SEC, in) == HealthVerdict::Pending);
    CHECK(x.evaluate(5000 + 179 * SEC, in) == HealthVerdict::Pending);
    CHECK(x.evaluate(5000 + 180 * SEC, in) == HealthVerdict::Pending);  // exactly 3 minutes is still inside
    CHECK(x.evaluate(5000 + 180 * SEC + 1, in) == HealthVerdict::Failed);
    CHECK(strcmp(x.why(), c.why) == 0);
    CHECK(strlen(x.why()) < 95 && x.why()[strlen(x.why()) - 1] == '.');
  }
  // 100 frames, not 99; a fetch waived when no service address is saved
  Health f99, f100;
  f99.begin(0); f100.begin(0);
  HealthInputs in99 = allGood(); in99.framesDrawn = 99;
  HealthInputs in100 = allGood(); in100.framesDrawn = 100;
  CHECK(f99.evaluate(1000, in99) == HealthVerdict::Pending);
  CHECK(f100.evaluate(1000, in100) == HealthVerdict::Pending);  // all met, waiting for the minute
  CHECK(f100.evaluate(61 * SEC, in100) == HealthVerdict::Good);
  CHECK(f99.evaluate(2000, in100) == HealthVerdict::Pending);   // 100 frames later, still within the time
  CHECK(f99.evaluate(61 * SEC, in100) == HealthVerdict::Good);
  Health nosvc;
  nosvc.begin(0);
  HealthInputs ns = allGood();
  ns.serviceReached = false;
  CHECK(nosvc.evaluate(1000, ns) == HealthVerdict::Pending);
  ns.serviceExpected = false;
  CHECK(nosvc.evaluate(2000, ns) == HealthVerdict::Pending);
  CHECK(nosvc.evaluate(60 * SEC, ns) == HealthVerdict::Good);
  Health nofetch;
  nofetch.begin(0);
  HealthInputs nf = allGood();
  nf.serviceReached = false;  // the service is expected: it must have answered
  CHECK(nofetch.evaluate(1000, nf) == HealthVerdict::Pending);
  CHECK(nofetch.evaluate(181 * SEC, nf) == HealthVerdict::Failed);

  SECTION("health: conditions that happen one after another");
  Health seq;
  seq.begin(0);
  HealthInputs s;
  s.serviceExpected = true;
  s.wifiJoined = true;
  CHECK(seq.evaluate(10 * SEC, s) == HealthVerdict::Pending);
  s.portalAnswering = true;
  CHECK(seq.evaluate(20 * SEC, s) == HealthVerdict::Pending);
  s.framesDrawn = 100;
  CHECK(seq.evaluate(60 * SEC, s) == HealthVerdict::Pending);
  s.wifiJoined = false;  // the Wi-Fi dropped after it had joined: it has happened
  s.serviceReached = true;
  CHECK(seq.evaluate(120 * SEC, s) == HealthVerdict::Good);
  // the last condition arrives after the deadline: too late, and the first look after it fails
  Health late;
  late.begin(0);
  HealthInputs l = allGood();
  l.serviceReached = false;
  CHECK(late.evaluate(100 * SEC, l) == HealthVerdict::Pending);
  l.serviceReached = true;
  CHECK(late.evaluate(180 * SEC + 1, l) == HealthVerdict::Failed);  // the fetch is only seen after the 3 minutes: it does not count
  CHECK(strcmp(late.why(), "The service did not answer within 3 minutes of starting.") == 0);
  // the first unmet one is named when several are missing: Wi-Fi first
  Health many;
  many.begin(0);
  CHECK(many.evaluate(181 * SEC, none) == HealthVerdict::Failed);
  CHECK(strcmp(many.why(), "The board did not join Wi-Fi within 3 minutes of starting.") == 0);
  // a stall fails at once, even when everything else is good, and even when it is the first thing seen
  Health st;
  st.begin(0);
  HealthInputs sv = allGood();
  sv.framesOver1s = 3;
  CHECK(st.evaluate(0, sv) == HealthVerdict::Failed);
  // begin() starts again
  st.begin(5000);
  CHECK(st.verdict() == HealthVerdict::Pending && st.why()[0] == 0);
  CHECK(st.evaluate(5001, allGood()) == HealthVerdict::Pending);
  CHECK(st.evaluate(5000 + 60 * SEC, allGood()) == HealthVerdict::Good);

  SECTION("health: the clock wraps");
  Health w;
  uint32_t boot = 0xFFFFFFFFu - 60 * SEC;
  w.begin(boot);
  HealthInputs half = allGood();
  half.serviceReached = false;
  CHECK(w.evaluate(boot + 100 * SEC, half) == HealthVerdict::Pending);  // past the wrap
  half.serviceReached = true;
  CHECK(w.evaluate(boot + 170 * SEC, half) == HealthVerdict::Good);
  Health w2;
  w2.begin(boot);
  half.serviceReached = false;
  CHECK(w2.evaluate(boot + 181 * SEC, half) == HealthVerdict::Failed);
  CHECK(PROBATION_MS == 3 * MIN && PROBATION_MIN_FRAMES == 100);
}

static void testHealthSoak() {
  SECTION("health: at least a minute before a new image is kept");
  CHECK(PROBATION_MIN_SOAK_MS == 60 * SEC);
  // all conditions met in the first seconds: still pending until the minute, then good
  Health a;
  a.begin(2000);
  CHECK(a.evaluate(2000 + 1 * SEC, allGood()) == HealthVerdict::Pending);
  CHECK(a.evaluate(2000 + 59 * SEC, allGood()) == HealthVerdict::Pending);
  CHECK(a.evaluate(2000 + 60 * SEC, allGood()) == HealthVerdict::Good);
  // a stall during the wait still fails the image: that is what the minute is for
  Health b;
  b.begin(0);
  CHECK(b.evaluate(5 * SEC, allGood()) == HealthVerdict::Pending);
  HealthInputs stall = allGood();
  stall.framesOver1s = 1;
  CHECK(b.evaluate(30 * SEC, stall) == HealthVerdict::Failed);
  CHECK(strcmp(b.why(), "The panel stalled for more than a second.") == 0);
  // a condition met during the wait is kept: the Wi-Fi may drop afterwards
  Health c;
  c.begin(0);
  CHECK(c.evaluate(5 * SEC, allGood()) == HealthVerdict::Pending);
  HealthInputs dropped = allGood();
  dropped.wifiJoined = false;
  dropped.portalAnswering = false;
  CHECK(c.evaluate(61 * SEC, dropped) == HealthVerdict::Good);
  // the wait is part of the 3 minutes, not added to them: a condition that comes at 2:50 is in time and the verdict is immediate
  Health d;
  d.begin(0);
  HealthInputs late = allGood();
  late.serviceReached = false;
  CHECK(d.evaluate(100 * SEC, late) == HealthVerdict::Pending);
  late.serviceReached = true;
  CHECK(d.evaluate(170 * SEC, late) == HealthVerdict::Good);
  // across the clock's wrap
  Health w;
  uint32_t boot = 0xFFFFFFFFu - 20 * SEC;
  w.begin(boot);
  CHECK(w.evaluate(boot + 10 * SEC, allGood()) == HealthVerdict::Pending);
  CHECK(w.evaluate(boot + 59 * SEC, allGood()) == HealthVerdict::Pending);   // past the wrap, 59 s after boot
  CHECK(w.evaluate(boot + 60 * SEC, allGood()) == HealthVerdict::Good);
}

static void testServiceReached() {
  SECTION("health: the service was reached (any answer), and when that is waived");
  // any complete HTTP answer counts: a board whose key is refused (401) must still be able to take a fix
  for (int ok : {100, 200, 204, 301, 304, 400, 401, 403, 404, 429, 500, 502, 503, 599}) CHECK(isHttpAnswer(ok));
  for (int bad : {0, -1, 99, 600, 999, 12345, -200}) CHECK(!isHttpAnswer(bad));
  // what the data task would fetch
  CHECK(anythingToFetch(true, false, false, true, false));    // flights with a location
  CHECK(anythingToFetch(false, true, false, true, false));    // weather with a location
  CHECK(anythingToFetch(false, false, true, false, true));    // headlines with a topic (no location needed)
  CHECK(!anythingToFetch(true, true, false, false, false));   // flights and weather but no location
  CHECK(!anythingToFetch(false, false, true, true, false));   // headlines but no topic
  CHECK(!anythingToFetch(false, false, false, true, true));   // every screen off
  CHECK(!anythingToFetch(false, false, false, false, false));
  CHECK(anythingToFetch(true, true, true, false, true));
  // the condition applies only with an address, an identity that can sign, and something to fetch
  for (int m = 0; m < 8; m++) {
    bool saved = m & 1, idOk = m & 2, fetch = m & 4;
    CHECK(serviceCheckApplies(saved, idOk, fetch) == (saved && idOk && fetch));
  }
  CHECK(serviceCheckApplies(true, true, true));
  CHECK(!serviceCheckApplies(false, true, true));   // no service address saved
  CHECK(!serviceCheckApplies(true, false, true));   // no identity record: nothing could be sent
  CHECK(!serviceCheckApplies(true, true, false));   // nothing would be fetched
  // through the checklist: a 401 is an answer (reached), no answer at all is not, and the waiver ends the wait
  Health refused;
  refused.begin(0);
  HealthInputs in = allGood();
  in.serviceReached = isHttpAnswer(401);
  CHECK(refused.evaluate(10 * SEC, in) == HealthVerdict::Pending);
  CHECK(refused.evaluate(61 * SEC, in) == HealthVerdict::Good);
  Health silent;
  silent.begin(0);
  in = allGood();
  in.serviceReached = isHttpAnswer(0);
  CHECK(silent.evaluate(10 * SEC, in) == HealthVerdict::Pending);
  CHECK(silent.evaluate(181 * SEC, in) == HealthVerdict::Failed);
  CHECK(strcmp(silent.why(), "The service did not answer within 3 minutes of starting.") == 0);
  Health waived;
  waived.begin(0);
  in = allGood();
  in.serviceReached = false;
  in.serviceExpected = serviceCheckApplies(true, false, true);  // identity not ok
  CHECK(waived.evaluate(61 * SEC, in) == HealthVerdict::Good);
  Health noFetch;
  noFetch.begin(0);
  in.serviceExpected = serviceCheckApplies(true, true, anythingToFetch(false, false, false, true, true));
  CHECK(noFetch.evaluate(61 * SEC, in) == HealthVerdict::Good);
}

// ---- the rollback mechanism's boot-time decision ----------------------------------------------------------------------------
static void testProbationStep() {
  SECTION("probation: the boot counter");
  BootRecord rec;
  // an image that is not on probation (a USB flash, a marked-good image): nothing counts, the record is reset to zero
  rec.magic = 0xDEADBEEF; rec.boots = 77;  // garbage, as after a power-on reset
  ProbationStep a = probationStep(false, rec);
  CHECK(!a.onProbation && a.boots == 0 && !a.rollback);
  CHECK(rec.magic == BOOT_MAGIC && rec.boots == 0);
  rec.magic = BOOT_MAGIC; rec.boots = 3;  // a stale count from an old probation
  a = probationStep(false, rec);
  CHECK(!a.onProbation && !a.rollback && rec.boots == 0);

  // the first boot of an updated image: the old image left a zero count; boot 1, 2, 3 are fine, the 4th goes back
  rec.magic = BOOT_MAGIC; rec.boots = 0;
  for (uint32_t n = 1; n <= PROBATION_MAX_BOOTS; n++) {
    ProbationStep p = probationStep(true, rec);
    CHECK(p.onProbation && p.boots == n && !p.rollback && rec.boots == n && rec.magic == BOOT_MAGIC);
  }
  ProbationStep p4 = probationStep(true, rec);
  CHECK(p4.onProbation && p4.boots == 4 && p4.rollback);
  CHECK(PROBATION_MAX_BOOTS == 3);
  ProbationStep p5 = probationStep(true, rec);  // still failing to go back (no previous image): keeps asking
  CHECK(p5.rollback && p5.boots == 5);

  // after a power-on reset the memory is garbage: the count starts at 1, whatever the garbage says
  for (uint32_t garbage : {0u, 1u, 3u, 4u, 0xFFFFFFFFu, 0x12345678u}) {
    BootRecord g;
    g.magic = garbage; g.boots = 2;
    ProbationStep s = probationStep(true, g);
    CHECK(s.onProbation && s.boots == 1 && !s.rollback && g.magic == BOOT_MAGIC && g.boots == 1);
  }
  // a valid magic with a huge count does not wrap
  BootRecord big;
  big.magic = BOOT_MAGIC; big.boots = 0xFFFFFFFFu;
  ProbationStep bs = probationStep(true, big);
  CHECK(bs.onProbation && bs.boots == 1);  // not a count this firmware wrote: starts again
  big.magic = BOOT_MAGIC; big.boots = 0xFFFFFFEFu;
  bs = probationStep(true, big);
  CHECK(bs.boots == 0xFFFFFFF0u && bs.rollback);
  bs = probationStep(true, big);
  CHECK(bs.boots == 1);  // at the top it starts over instead of wrapping to 0

  // marked good: the count starts again, and the next probation is a fresh one
  BootRecord good;
  good.magic = BOOT_MAGIC; good.boots = 2;
  probationOver(good);
  CHECK(good.magic == BOOT_MAGIC && good.boots == 0);
  CHECK(probationStep(true, good).boots == 1);
  // a crash loop: the image restarts, counts, and on the 4th boot says so; a rollback gives a not-on-probation boot that resets
  BootRecord loop;
  loop.magic = 0; loop.boots = 0;
  int rolledBackAt = 0;
  for (int boot = 1; boot <= 6 && !rolledBackAt; boot++) {
    ProbationStep s = probationStep(true, loop);
    if (s.rollback) rolledBackAt = boot;
    else if (boot == 1) { loop.magic = BOOT_MAGIC; }  // (the first boot after a power-on reset: the count is 1)
  }
  CHECK(rolledBackAt == 4);
  CHECK(!probationStep(false, loop).onProbation && loop.boots == 0);  // the previous image booted: reset

  SECTION("probation: which image states are on probation");
  CHECK(isProbation(ImageState::New));
  CHECK(isProbation(ImageState::PendingVerify));
  CHECK(!isProbation(ImageState::Valid));
  CHECK(!isProbation(ImageState::Undefined));  // an image flashed over USB
  CHECK(!isProbation(ImageState::Invalid));
  CHECK(!isProbation(ImageState::Aborted));
  CHECK(strlen(WHY_RESTART_LOOP) < 95 && WHY_RESTART_LOOP[strlen(WHY_RESTART_LOOP) - 1] == '.');
  CHECK(strcmp(WHY_RESTART_LOOP, "The new version kept restarting.") == 0);

  SECTION("probation: the record of a failed version");
  Version run, failed;
  pv("0.10.0", run); pv("0.10.1", failed);
  CHECK(!failureRecordObsolete(run, failed));   // rolled back to 0.10.0: the record about 0.10.1 stays
  pv("0.10.1", run);
  CHECK(failureRecordObsolete(run, failed));    // the same version runs: history
  pv("0.10.2", run);
  CHECK(failureRecordObsolete(run, failed));    // a newer one runs: history
  pv("0.9.9", run);
  CHECK(!failureRecordObsolete(run, failed));
  // and the offer rule that goes with it, in the order a real board lives it: failed 0.10.1, service still publishes 0.10.1
  pv("0.10.0", run);
  Version pub;
  pv("0.10.1", pub);
  CHECK(!isOffered(run, pub, &failed));
  pv("0.10.2", pub);
  CHECK(isOffered(run, pub, &failed));
  // the service goes back to an older release than the one that failed: still not offered
  pv("0.10.0", pub);
  CHECK(!isOffered(run, pub, &failed));
}

static void testVersionTag() {
  SECTION("version tag: what an image says it is");
  // the marker the firmware embeds: prefix, version, end byte
  std::string tag = std::string(VERSION_TAG_PREFIX) + FIRMWARE_VERSION + "\x03";
  CHECK(tag.size() == VERSION_TAG_PREFIX_LEN + strlen(FIRMWARE_VERSION) + 1);
  CHECK(VERSION_TAG_PREFIX_LEN == 22 && VERSION_TAG_PREFIX[0] == '\x01' && VERSION_TAG_PREFIX[VERSION_TAG_PREFIX_LEN - 1] == '\x02');
  auto scan = [](const std::string &bytes, size_t piece) {
    VersionTagScanner sc;
    for (size_t i = 0; i < bytes.size(); i += piece) sc.feed((const uint8_t *)bytes.data() + i, std::min(piece, bytes.size() - i));
    return sc;
  };
  // an image: some bytes, the marker, more bytes, whatever the size of the pieces it is read in
  std::string image(5000, 'x');
  for (size_t i = 0; i < image.size(); i += 7) image[i] = (char)(i * 31);
  image.replace(3001, 0, tag);
  for (size_t piece : {1u, 2u, 3u, 5u, 21u, 22u, 23u, 64u, 500u, 4096u, 100000u}) {
    VersionTagScanner sc = scan(image, piece);
    CHECK(sc.found() && strcmp(sc.text(), FIRMWARE_VERSION) == 0);
  }
  // at the very start and the very end of the image
  CHECK(scan(tag, 1).found() && scan(tag, 1000).found());
  CHECK(scan("zzz" + tag, 4).found());
  CHECK(!scan(tag.substr(0, tag.size() - 1), 4).found());  // cut before the end byte
  // the prefix alone (the constant in the code that looks for it) is skipped; a later real marker is found
  std::string both = std::string(VERSION_TAG_PREFIX) + std::string("\0\0\0 code", 9) + std::string("....") + std::string(VERSION_TAG_PREFIX) + "0.12.3\x03";
  VersionTagScanner b = scan(both, 3);
  CHECK(b.found() && strcmp(b.text(), "0.12.3") == 0 && b.version().major == 0 && b.version().minor == 12 && b.version().patch == 3);
  // not a version: no digits, letters, a missing dot, too long, too many parts, an empty one
  for (const char *bad : {"abc", "", "1.2", "1.2.3.4", "1..3", "10000.0.0", "1.2.x", "12345678901234567890", "1.2.3 "}) {
    std::string t = std::string(VERSION_TAG_PREFIX) + bad + "\x03";
    CHECK(!scan(t, 5).found());
  }
  // a broken marker followed by a good one: the good one is found, also when the broken one ends in the first byte of the prefix
  std::string broken = std::string(VERSION_TAG_PREFIX) + "1.2" + std::string("\x01") + (VERSION_TAG_PREFIX + 1) + "0.9.9\x03";
  VersionTagScanner c = scan(broken, 2);
  CHECK(c.found() && strcmp(c.text(), "0.9.9") == 0);
  // the first valid marker wins
  std::string two = tag + std::string(VERSION_TAG_PREFIX) + "7.7.7\x03";
  VersionTagScanner d = scan(two, 9);
  CHECK(d.found() && strcmp(d.text(), FIRMWARE_VERSION) == 0);
  // nothing in it
  CHECK(!scan(std::string(100000, 'a'), 4096).found());
  CHECK(!scan("", 1).found());
  VersionTagScanner none;
  CHECK(!none.found() && strcmp(none.text(), "") == 0);
  none.feed(nullptr, 0);
  CHECK(!none.found());
  // comparing with the manifest: by number, so a leading zero is the same version
  Version v1, v2;
  pv("0.10.1", v1); pv("0.10.01", v2);
  CHECK(compareVersions(v1, v2) == 0);
}

static void testChunks() {
  SECTION("chunked coding: the size line");
  size_t n = 99;
  CHECK(parseChunkSize("0", 1, n) && n == 0);                   // the last chunk
  CHECK(parseChunkSize("0\r", 2, n) && n == 0);
  CHECK(parseChunkSize("1", 1, n) && n == 1);
  CHECK(parseChunkSize("a", 1, n) && n == 10);
  CHECK(parseChunkSize("A\r", 2, n) && n == 10);
  CHECK(parseChunkSize("1000\r", 5, n) && n == 4096);
  CHECK(parseChunkSize("ffffffff", 8, n) && n == 0xFFFFFFFFu);
  CHECK(parseChunkSize("00000010", 8, n) && n == 16);
  CHECK(parseChunkSize("1f4;name=value\r", 15, n) && n == 500);      // a chunk extension
  CHECK(parseChunkSize("0;x\r", 4, n) && n == 0);
  CHECK(parseChunkSize("10 \r", 4, n) && n == 16);
  CHECK(parseChunkSize("10\n", 3, n) && n == 16);
  // garbage is an error, never "0": a body that is not chunked at all, a blank line, a sign, a prefix, too many digits
  size_t keep = 4242;
  for (const char *bad : {"", "\r", " ", "g", "zz\r", "<html>", "HTTP/1.1 200 OK\r", "-1", "+5", "0x10", "1 2", "10000000000", "123456789", "\xC3\xA9",
                          "1,5", "5;", "12\x01"}) {
    size_t out = keep;
    size_t len = strlen(bad);
    bool ok = parseChunkSize(bad, len, out);
    if (strcmp(bad, "5;") == 0) { CHECK(ok && out == 5); continue; }   // a bare ";" opens an empty extension: allowed
    CHECK(!ok);
    CHECK(out == keep);  // untouched on failure
  }
  CHECK(!parseChunkSize(nullptr, 3, n));
  CHECK(!parseChunkSize("5", 0, n));      // the length decides: nothing to read
  CHECK(parseChunkSize("5xyz", 1, n) && n == 5);  // only `len` bytes are looked at
  // the line after a chunk's data
  CHECK(isChunkEnd("", 0));
  CHECK(isChunkEnd("\r", 1));
  CHECK(!isChunkEnd("x", 1) && !isChunkEnd("\r\r", 2) && !isChunkEnd("0\r", 2) && !isChunkEnd(" ", 1));
  // a whole body, decoded the way live.cpp does it: size line, data, end line, ... a zero size ends it
  std::string wire = "5\r\nhello\r\n6;e=1\r\n world\r\n0\r\n\r\n";
  size_t pos = 0, total = 0;
  std::string body;
  bool done = false, error = false;
  while (!done && !error) {
    size_t eol = wire.find('\n', pos);
    if (eol == std::string::npos) { error = true; break; }
    std::string line = wire.substr(pos, eol - pos);
    pos = eol + 1;
    size_t sz;
    if (!parseChunkSize(line.data(), line.size(), sz)) { error = true; break; }
    if (sz == 0) { done = true; break; }
    body += wire.substr(pos, sz);
    total += sz;
    pos += sz;
    size_t e2 = wire.find('\n', pos);
    std::string endLine = wire.substr(pos, e2 - pos);
    if (!isChunkEnd(endLine.data(), endLine.size())) { error = true; break; }
    pos = e2 + 1;
  }
  CHECK(done && !error && body == "hello world" && total == 11);
  // the same body with the second size line damaged is an error, not the end
  std::string damaged = "5\r\nhello\r\nQ#\r\n world\r\n0\r\n\r\n";
  pos = 0; done = error = false; body.clear();
  while (!done && !error) {
    size_t eol = damaged.find('\n', pos);
    std::string line = damaged.substr(pos, eol - pos);
    pos = eol + 1;
    size_t sz;
    if (!parseChunkSize(line.data(), line.size(), sz)) { error = true; break; }
    if (sz == 0) { done = true; break; }
    body += damaged.substr(pos, sz);
    pos += sz;
    size_t e2 = damaged.find('\n', pos);
    pos = e2 + 1;
  }
  CHECK(error && !done && body == "hello");
}

static void testAnswers() {
  SECTION("the portal's answers: ages and sentences");
  CHECK(ageSeconds(100, 40) == 60);
  CHECK(ageSeconds(100, 100) == 0);
  CHECK(ageSeconds(100, 101) == 0);                           // a reading taken a moment later in another task: not 4 billion
  CHECK(ageSeconds(0, 5000000000ll) == 0);
  CHECK(ageSeconds(60LL * 86400, 0) == 60u * 86400u);         // 60 days, no wrap at 24.9 or 49.7
  CHECK(ageSeconds(0x200000000ll, 0) == 0xFFFFFFFFu);         // more than 32 bits: capped
  CHECK(ageSeconds(4294967ll, 0) == 4294967u);
  char buf[96];
  throttleSentence(60, buf, sizeof buf);
  CHECK(strcmp(buf, "Too many wrong codes. Wait 60 seconds, then press Update.") == 0);
  throttleSentence(1, buf, sizeof buf);
  CHECK(strcmp(buf, "Too many wrong codes. Wait 1 second, then press Update.") == 0);
  throttleSentence(3600, buf, sizeof buf);
  CHECK(strlen(buf) < 95 && buf[strlen(buf) - 1] == '.');
  throttleSentence(4294967295u, buf, sizeof buf);
  CHECK(strlen(buf) < 95);
  for (const char *t : {SENTENCE_CODE_WRONG, SENTENCE_CODE_EXPIRED}) CHECK(strlen(t) < 95 && t[strlen(t) - 1] == '.' && t[0] >= 'A' && t[0] <= 'Z');
  CHECK(strcmp(SENTENCE_CODE_EXPIRED, "The code expired. Press Update to get a new one.") == 0);
  // every refusal status has a sentence
  for (int http : {400, 403, 409, 413, 422, 429, 500, 503}) {
    const char *t = refusalFallback(http);
    CHECK(strlen(t) > 10 && strlen(t) < 95 && t[strlen(t) - 1] == '.');
  }
}

// ---- the words ---------------------------------------------------------------------------------------------------------------
static void testFailWords() {
  SECTION("fail: every way to fail has its own plain sentence");
  std::vector<std::string> sentences;
  for (int i = (int)Fail::NoFirmware; i <= (int)Fail::CannotConfirm; i++) {
    Fail f = (Fail)i;
    const char *raw = failRaw(f);
    CHECK(raw && strlen(raw) > 5);
    char why[160], detail[80];
    problemWords(raw, why, sizeof why, detail, sizeof detail);
    CHECK(strlen(why) > 20 && strlen(why) < 159 && why[strlen(why) - 1] == '.');
    CHECK(strcmp(why, raw) != 0);  // worded, not passed through
    CHECK(why[0] >= 'A' && why[0] <= 'Z');
    // a reason after the raw text changes the detail, never the sentence
    std::string withReason = std::string(raw) + ": some reason";
    char why2[160], detail2[80];
    problemWords(withReason.c_str(), why2, sizeof why2, detail2, sizeof detail2);
    CHECK(strcmp(why, why2) == 0);
    // a sentence fed back in is itself
    char why3[160], detail3[80];
    problemWords(why, why3, sizeof why3, detail3, sizeof detail3);
    CHECK(strcmp(why, why3) == 0);
    for (const std::string &other : sentences) CHECK(other != why);
    sentences.push_back(why);
  }
  CHECK(sentences.size() == 17);
  CHECK(strlen(failRaw(Fail::None)) == 0);
}

// Optional: the path of a real build (build/ledmatrix.ino.bin). The marker the firmware embeds must be in it, say this version, and the
// image header must pass the first look the updater takes.
static void testRealImage(const char *path) {
  SECTION("a real build");
  std::ifstream f(path, std::ios::binary);
  std::string img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(img.size() > IMAGE_MIN_BYTES && img.size() <= IMAGE_MAX_BYTES);
  CHECK(imageHeaderOk((const uint8_t *)img.data(), img.size()));
  VersionTagScanner sc;
  for (size_t i = 0; i < img.size(); i += 4096) sc.feed((const uint8_t *)img.data() + i, std::min<size_t>(4096, img.size() - i));
  CHECK(sc.found());
  CHECK(strcmp(sc.text(), FIRMWARE_VERSION) == 0);
  printf("    image %zu bytes, says version %s\n", img.size(), sc.text());
}

int main(int argc, char **argv) {
  testVersions();
  testOffered();
  testManifestValid();
  testManifestVersionAndFile();
  testManifestSize();
  testManifestSha();
  testManifestNotes();
  testManifestShape();
  testImageHeader();
  testCodesEqual();
  testCodeGeneration();
  testCodeTriesAndExpiry();
  testCodeClockWrap();
  testThrottle();
  testThrottleAging();
  testStrikes();
  testFlowAllow();
  testFlowTransitions();
  testProgress();
  testSchedule();
  testHealth();
  testHealthSoak();
  testServiceReached();
  testProbationStep();
  testVersionTag();
  testChunks();
  testAnswers();
  testFailWords();
  if (argc > 1) testRealImage(argv[1]);
  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
