// Host test for arduino/ledmatrix/updatesig.h and updatesig_mbedtls.h (the signed firmware manifest, ticket 50). It reads the shared known-answer
// vectors at run time, so the Go service, the Python tools and this code all prove the same bytes (shared/manifest-signing.md,
// shared/manifest-signing-vectors.json). Run from the repo root; the one argument is the vectors file (default: that path).
//
// Two modes, one source file:
//   without -DWITH_MBEDTLS   the cryptography is a stand-in that says "valid" only for the signatures of the vectors that are valid (a table of
//                            key, digest, signature triples), with a SHA-256 of this file's own. It proves everything AROUND the crypto: the
//                            manifest rules, the shapes, the canonical bytes (through the digests: they are the signed ones), the key ids, the order
//                            of the checks, the three-valued results and the policy. It runs anywhere.
//   with -DWITH_MBEDTLS      the same vectors through updatesig_mbedtls.h: the very code the board runs, against a host mbedTLS (3.6.x; Homebrew has
//                            mbedtls@3). Every valid vector must verify and every invalid one must not, and the real public keys pinned in
//                            update_keys.h must be points on the curve.
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" tools/test_updatesig.cpp -o <scratch>/test_updatesig \
//       && <scratch>/test_updatesig shared/manifest-signing-vectors.json
//   M=$(brew --prefix mbedtls@3); c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -DWITH_MBEDTLS -Iarduino/ledmatrix -I"$M/include" \
//       -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" tools/test_updatesig.cpp "$M/lib/libmbedcrypto.a" -o <scratch>/test_updatesig_mbed \
//       && <scratch>/test_updatesig_mbed shared/manifest-signing-vectors.json
// Add -DUPDATE_REQUIRE_SIGNATURE=0 to build the development variant (the image marker and the policy constant then say "off").
// Every key in the vectors is a throwaway test key; every host, version and hash is a placeholder.
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "updatesig.h"
#ifdef WITH_MBEDTLS
#include "updatesig_mbedtls.h"
#endif

using namespace updatesig;
using otalogic::Fail;
using otalogic::Manifest;
using otalogic::ManifestError;
using otalogic::SigFields;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECKV(c, what) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s  [%s]\n", __LINE__, #c, std::string(what).c_str()); } } while (0)
#define SECTION(name) printf("%s\n", name)

// ---- SHA-256, this file's own (FIPS 180-4): the stand-in's hash, and a second opinion on the vectors' digests -----------------------------------------
struct Sha256 {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t total = 0;
  size_t used = 0;
  Sha256() {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, iv, sizeof h);
  }
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t *p, size_t n) {
    total += n;
    while (n) {
      size_t take = 64 - used < n ? 64 - used : n;
      memcpy(buf + used, p, take);
      used += take; p += take; n -= take;
      if (used == 64) { block(buf); used = 0; }
    }
  }
  void final(uint8_t out[32]) {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
  }
};
static void sha256(const uint8_t *p, size_t n, uint8_t out[32]) { Sha256 s; s.update(p, n); s.final(out); }

static std::string hex(const uint8_t *p, size_t n) {
  static const char d[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
static std::string hexOf(const std::string &s) { return hex((const uint8_t *)s.data(), s.size()); }
[[maybe_unused]] static std::vector<uint8_t> unhex(const std::string &s) {
  std::vector<uint8_t> v;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)(nib(s[i]) << 4 | nib(s[i + 1])));
  return v;
}

// ---- the stand-in cryptography ---------------------------------------------------------------------------------------------------------------
// SHA-256 is real (this file's own). The signature check says Valid only for a triple it was told about (the valid vectors), Invalid for anything
// else, unless a test forces another answer or scripts the next answers.
struct StandIn : Crypto {
  std::set<std::string> valid;  // hex(pub) ":" hex(digest) ":" hex(sig)
  bool shaFails = false;        // every SHA-256 fails
  bool shaFailsForBig = false;  // only a SHA-256 of more than 65 bytes fails (the hash of the canonical bytes, not of a key)
  bool forceVerify = false;
  Verify forced = Verify::Invalid;
  std::vector<Verify> script;   // when not empty: the next answers, in order (then the table again)
  void learn(const std::string &pub, const std::string &digest, const std::string &sig) { valid.insert(pub + ":" + digest + ":" + sig); }
  bool sha256(const uint8_t *msg, size_t len, uint8_t out[32]) override {
    if (shaFails || (shaFailsForBig && len > PUBKEY_LEN)) return false;
    ::sha256(msg, len, out);
    return true;
  }
  Verify p256Verify(const uint8_t pub[PUBKEY_LEN], const uint8_t hash[32], const uint8_t sig[otalogic::SIG_RAW_LEN]) override {
    if (!script.empty()) { Verify v = script.front(); script.erase(script.begin()); return v; }
    if (forceVerify) return forced;
    return valid.count(hex(pub, PUBKEY_LEN) + ":" + hex(hash, 32) + ":" + hex(sig, 64)) ? Verify::Valid : Verify::Invalid;
  }
};

// Every SHA-256 of a 65-byte key gives the same digest: two different pinned keys whose ids collide, so both match and both are tried.
struct CollidingIds : StandIn {
  bool sha256(const uint8_t *msg, size_t len, uint8_t out[32]) override {
    if (len == PUBKEY_LEN) { memset(out, 0xA5, 32); return true; }
    return StandIn::sha256(msg, len, out);
  }
};

// Counts what a check costs, whichever crypto is behind it.
struct Counting : Crypto {
  Crypto &inner;
  int shaCalls = 0, verifyCalls = 0;
  std::vector<std::string> pubs;  // the keys a verification was tried with, in order
  explicit Counting(Crypto &c) : inner(c) {}
  bool sha256(const uint8_t *msg, size_t len, uint8_t out[32]) override { shaCalls++; return inner.sha256(msg, len, out); }
  Verify p256Verify(const uint8_t pub[PUBKEY_LEN], const uint8_t hash[32], const uint8_t sig[otalogic::SIG_RAW_LEN]) override {
    verifyCalls++;
    pubs.push_back(hex(pub, PUBKEY_LEN));
    return inner.p256Verify(pub, hash, sig);
  }
  void reset() { shaCalls = verifyCalls = 0; pubs.clear(); }
};

// ---- the vectors -----------------------------------------------------------------------------------------------------------------------------
static JsonDocument vec;
struct KeyInfo { std::string name, id, pubHex; bool pinned; };
static std::vector<KeyInfo> keyInfos;

static std::string manifestText(JsonObjectConst v) {
  std::string t;
  serializeJson(v["manifest"], t);
  return t;
}
static std::string pubOfId(const std::string &id) {
  for (const KeyInfo &k : keyInfos)
    if (k.id == id) return k.pubHex;
  return "";
}
static Keys keysFor(const std::vector<std::string> &hexes) {  // the strings must outlive the Keys
  Keys k;
  for (const std::string &h : hexes) k.hex[k.count++] = h.c_str();
  return k;
}
static std::vector<std::string> pinnedHexes() {
  std::vector<std::string> v;
  for (const KeyInfo &k : keyInfos)
    if (k.pinned) v.push_back(k.pubHex);
  return v;
}
static Sig expectedSig(const std::string &e) {
  if (e == "ok") return Sig::Ok;
  if (e == "unsigned") return Sig::Unsigned;
  if (e == "malformed") return Sig::Malformed;
  if (e == "unknown_key") return Sig::UnknownKey;
  if (e == "bad_signature") return Sig::BadSignature;
  return Sig::CheckFailed;  // "manifest_invalid" never gets this far
}
static const char *sigName(Sig s) {
  switch (s) {
    case Sig::Ok: return "Ok";
    case Sig::NotChecked: return "NotChecked";
    case Sig::Unsigned: return "Unsigned";
    case Sig::Malformed: return "Malformed";
    case Sig::UnknownKey: return "UnknownKey";
    case Sig::BadSignature: return "BadSignature";
    case Sig::CheckFailed: return "CheckFailed";
  }
  return "?";
}
static bool hasControlByte(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7F) return true;
  return false;
}

// ---- hex, keys, ids, ranges --------------------------------------------------------------------------------------------------------------------
static void testHex() {
  SECTION("hex: strict decoding (the contract's rules)");
  uint8_t out[8];
  CHECK(otalogic::hexDecodeLower("00ff10ab", 8, out, sizeof out) && out[0] == 0x00 && out[1] == 0xFF && out[2] == 0x10 && out[3] == 0xAB);
  CHECK(otalogic::hexDecodeLower("", 0, out, sizeof out));
  CHECK(!otalogic::hexDecodeLower("00FF", 4, out, sizeof out));        // upper case
  CHECK(!otalogic::hexDecodeLower("00fF", 4, out, sizeof out));        // mixed
  CHECK(!otalogic::hexDecodeLower("0", 1, out, sizeof out));           // odd length
  CHECK(!otalogic::hexDecodeLower("00f", 3, out, sizeof out));
  CHECK(!otalogic::hexDecodeLower("0x00", 4, out, sizeof out));        // a prefix is not hex
  CHECK(!otalogic::hexDecodeLower("00 f", 4, out, sizeof out));        // a space
  CHECK(!otalogic::hexDecodeLower("-0ff", 4, out, sizeof out));        // a sign
  CHECK(!otalogic::hexDecodeLower("00g0", 4, out, sizeof out));        // a letter past f
  CHECK(!otalogic::hexDecodeLower("00`0", 4, out, sizeof out));        // the character before a
  CHECK(!otalogic::hexDecodeLower("00:0", 4, out, sizeof out));        // the character after 9
  CHECK(!otalogic::hexDecodeLower("00/0", 4, out, sizeof out));        // the character before 0
  CHECK(!otalogic::hexDecodeLower(std::string("00\0f", 4).c_str(), 4, out, sizeof out));  // a NUL inside
  CHECK(!otalogic::hexDecodeLower("00ff00ff00", 10, out, 4));          // does not fit the output
  CHECK(otalogic::hexDecodeLower("00ff00ff", 8, out, 4));              // exactly fits
  CHECK(!otalogic::hexDecodeLower(nullptr, 0, out, sizeof out));
  CHECK(!otalogic::hexDecodeLower("00", 2, nullptr, 1));
  CHECK(!otalogic::hexDecodeLower("\xEF\xBC\x90\xEF\xBC\x91", 6, out, sizeof out));  // full-width digits are not digits
  CHECK(otalogic::isLowerHex("0123456789abcdef", 16) && !otalogic::isLowerHex("0123456789abcdeF", 16) && !otalogic::isLowerHex("g", 1));
  CHECK(otalogic::isLowerHex("", 0) && !otalogic::isLowerHex(nullptr, 0));
  uint8_t all[8];
  CHECK(otalogic::hexDecodeLower("0123456789abcdef", 16, all, sizeof all) && all[0] == 0x01 && all[1] == 0x23 && all[2] == 0x45 && all[3] == 0x67 &&
        all[4] == 0x89 && all[5] == 0xAB && all[6] == 0xCD && all[7] == 0xEF);
}

static void testKeysAndIds() {
  SECTION("keys: the ids of the vectors' keys are the first 4 bytes of SHA-256 of the 65-byte point");
  StandIn c;
  CHECK(keyInfos.size() == 3);
  for (const KeyInfo &k : keyInfos) {
    uint8_t pub[PUBKEY_LEN];
    CHECKV(decodePublicKey(k.pubHex.c_str(), pub) && pub[0] == 0x04, k.name);
    char id[9];
    CHECKV(keyIdOf(c, pub, id), k.name);
    CHECKV(k.id == id, k.name);
    uint8_t d[32];
    sha256(pub, PUBKEY_LEN, d);
    CHECKV(k.id == hex(d, 4), k.name);  // ...and written out by hand
    CHECK(strlen(id) == 8 && otalogic::isLowerHex(id, 8));
  }
  SECTION("keys: the public key text rules");
  uint8_t pub[PUBKEY_LEN];
  std::string good = keyInfos[0].pubHex;
  CHECK(decodePublicKey(good.c_str(), pub));
  CHECK(!decodePublicKey(nullptr, pub));
  CHECK(!decodePublicKey("", pub));
  CHECK(!decodePublicKey(good.substr(0, 128).c_str(), pub));                       // short
  CHECK(!decodePublicKey((good + "00").c_str(), pub));                             // long
  std::string upper = good;
  for (char &ch : upper) ch = (char)toupper((unsigned char)ch);
  CHECK(!decodePublicKey(upper.c_str(), pub));
  std::string compressed = "02" + good.substr(2, 64);                              // a compressed point is not accepted
  CHECK(!decodePublicKey(compressed.c_str(), pub));
  std::string wrongTag = "05" + good.substr(2);                                    // not the uncompressed marker
  CHECK(!decodePublicKey(wrongTag.c_str(), pub));
  std::string notHex = good;
  notHex[40] = 'z';
  CHECK(!decodePublicKey(notHex.c_str(), pub));
  CHECK(keyTextOk(good.c_str()) && !keyTextOk(upper.c_str()) && !keyTextOk(compressed.c_str()) && !keyTextOk(wrongTag.c_str()) && !keyTextOk(""));
  CHECK(!keyTextOk(good.substr(0, 129).c_str()) && !keyTextOk((good + "0").c_str()) && !keyTextOk(notHex.c_str()));
  CHECK(sameText("abc", "abc") && !sameText("abc", "abd") && !sameText("abc", "ab") && !sameText("ab", "abc") && sameText("", ""));

  SECTION("keys: the ids of the pinned keys");
  std::vector<std::string> two = {keyInfos[0].pubHex, keyInfos[1].pubHex};
  PinnedIds ids;
  CHECK(derivePinnedIds(keysFor(two), c, ids) && ids.count == 2 && keyInfos[0].id == ids.id[0] && keyInfos[1].id == ids.id[1]);
  std::vector<std::string> one = {keyInfos[1].pubHex};
  CHECK(derivePinnedIds(keysFor(one), c, ids) && ids.count == 1 && keyInfos[1].id == ids.id[0]);
  std::vector<std::string> broken = {keyInfos[0].pubHex, "04zz"};
  PinnedIds keep;
  keep.count = 7;
  CHECK(!derivePinnedIds(keysFor(broken), c, keep) && keep.count == 7);            // untouched on failure
  c.shaFails = true;
  CHECK(!derivePinnedIds(keysFor(two), c, keep) && keep.count == 7);
  c.shaFails = false;
  Keys tooMany;
  tooMany.count = 3;
  CHECK(!derivePinnedIds(tooMany, c, keep));
}

static void testRange() {
  SECTION("r and s: 1 <= x < n");
  uint8_t x[32];
  memset(x, 0, 32);
  CHECK(!scalarInRange(x));                                           // 0
  x[31] = 1;
  CHECK(scalarInRange(x));                                            // 1
  memcpy(x, P256_ORDER, 32);
  CHECK(!scalarInRange(x));                                           // n
  x[31] = 0x50;
  CHECK(scalarInRange(x));                                            // n - 1
  x[31] = 0x52;
  CHECK(!scalarInRange(x));                                           // n + 1
  memset(x, 0xFF, 32);
  CHECK(!scalarInRange(x));                                           // 2^256 - 1
  memcpy(x, P256_ORDER, 32);
  x[0] = 0xFE;
  CHECK(scalarInRange(x));                                            // smaller in the first byte, whatever follows
  memcpy(x, P256_ORDER, 32);
  x[16] = 0xBD;                                                       // n with one middle byte raised: above n
  CHECK(!scalarInRange(x));
  memcpy(x, P256_ORDER, 32);
  x[16] = 0xBB;
  x[31] = 0xFF;                                                       // one middle byte lowered, the rest high: below n
  CHECK(scalarInRange(x));
  memset(x, 0, 32);
  x[0] = 0x80;
  CHECK(scalarInRange(x));                                            // 2^255
  CHECK(P256_ORDER[0] == 0xFF && P256_ORDER[4] == 0x00 && P256_ORDER[8] == 0xFF && P256_ORDER[16] == 0xBC && P256_ORDER[31] == 0x51);
}

// ---- the manifest's signature fields and its notes rule ------------------------------------------------------------------------------------------
static std::string baseJson(const std::string &extra, const std::string &notes = "\"Test release: signed manifest vectors.\"") {
  return "{\"version\":\"0.11.0\",\"file\":\"ledmatrix-0.11.0.bin\",\"size\":1412537,\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\","
         "\"notes\":" + notes + extra + "}";
}
static ManifestError parse(const std::string &text, Manifest &m) { return otalogic::parseManifest(text.c_str(), text.size(), m); }

static void testManifestFields() {
  SECTION("manifest: the signature fields (shapes), in every wrong way");
  const std::string sig = "d29adc49a905a9f46197a1d1b988d04bf9829a65293f9565f9c19a103ddf7c0e39e6f8d57baccf1993a129a2acf231603e97bf90970d88c44baa638bb342c6c4";
  const std::string kid = "f864ea3b";
  auto both = [&](const std::string &k, const std::string &s) { return ",\"keyid\":" + k + ",\"sig\":" + s; };
  auto q = [](const std::string &s) { return "\"" + s + "\""; };
  struct Case { std::string extra; SigFields want; const char *what; };
  std::vector<Case> cases = {
      {"", SigFields::Absent, "neither field"},
      {both("null", "null"), SigFields::Absent, "both null: null is absent"},
      {both(q(kid), q(sig)), SigFields::Present, "both valid"},
      {both(q(kid), q(sig)) + ",\"extra\":[1,2]", SigFields::Present, "an unknown key is ignored"},
      {",\"sig\":" + q(sig), SigFields::Malformed, "sig without keyid"},
      {",\"keyid\":" + q(kid), SigFields::Malformed, "keyid without sig"},
      {both(q(kid), "null"), SigFields::Malformed, "keyid and a null sig"},
      {both("null", q(sig)), SigFields::Malformed, "a null keyid and a sig"},
      {both(q(kid.substr(0, 7)), q(sig)), SigFields::Malformed, "keyid of 7"},
      {both(q(kid + "0"), q(sig)), SigFields::Malformed, "keyid of 9"},
      {both(q("F864EA3B"), q(sig)), SigFields::Malformed, "keyid upper case"},
      {both(q("f864ea3g"), q(sig)), SigFields::Malformed, "keyid not hex"},
      {both(q(""), q(sig)), SigFields::Malformed, "keyid empty"},
      {both("12345678", q(sig)), SigFields::Malformed, "keyid a number"},
      {both("true", q(sig)), SigFields::Malformed, "keyid a boolean"},
      {both("[\"f864ea3b\"]", q(sig)), SigFields::Malformed, "keyid a list"},
      {both("{}", q(sig)), SigFields::Malformed, "keyid an object"},
      {both("\"f864ea3\\u0000\"", q(sig)), SigFields::Malformed, "keyid with a NUL inside (8 bytes, 7 characters)"},
      {both(q(kid), q(sig.substr(0, 126))), SigFields::Malformed, "sig of 126"},
      {both(q(kid), q(sig + "00")), SigFields::Malformed, "sig of 130"},
      {both(q(kid), q(sig.substr(0, 127))), SigFields::Malformed, "sig of 127 (odd)"},
      {both(q(kid), q(std::string(128, 'z'))), SigFields::Malformed, "sig not hex"},
      {both(q(kid), q(std::string(128, 'D'))), SigFields::Malformed, "sig upper case"},
      {both(q(kid), q("")), SigFields::Malformed, "sig empty"},
      {both(q(kid), "12345"), SigFields::Malformed, "sig a number"},
      {both(q(kid), "[1]"), SigFields::Malformed, "sig a list"},
      {both(q(kid), "false"), SigFields::Malformed, "sig false"},
      {both(q(kid), q(sig.substr(0, 127) + "\\u0000")), SigFields::Malformed, "sig with a NUL at its end"},
  };
  for (const Case &c : cases) {
    Manifest m;
    ManifestError e = parse(baseJson(c.extra), m);
    CHECKV(e == ManifestError::None, c.what);  // a bad signature field never makes the manifest itself invalid
    CHECKV(m.sigFields == c.want, c.what);
    if (c.want == SigFields::Present) {
      CHECKV(strcmp(m.keyid, kid.c_str()) == 0 && hex(m.sig, 64) == sig, c.what);
    }
  }
  // the fields do not change the values the other rules read
  Manifest m;
  CHECK(parse(baseJson(both(q(kid), q(sig))), m) == ManifestError::None && strcmp(m.version, "0.11.0") == 0 && m.size == 1412537 && strcmp(m.notes, "Test release: signed manifest vectors.") == 0);
  // upper case in the value is the point: the hex decoder never sees it
  CHECK(parse(baseJson(both(q(kid), q(sig))), m) == ManifestError::None && m.sig[0] == 0xd2 && m.sig[63] == 0xc4);
  // the notes of a signed manifest are held, and all of the signature's other rules still apply first
  CHECK(parse("{\"version\":\"0.11.0\",\"file\":\"ledmatrix-0.11.1.bin\",\"size\":1412537,\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\"" + both(q(kid), q(sig)) + "}", m) == ManifestError::File);

  SECTION("manifest: notes hold no control character");
  for (const char *notes : {"\"line one\\nline two\"", "\"tab\\there\"", "\"cr\\rhere\"", "\"esc\\u001bhere\"", "\"bell\\u0007\"", "\"del\\u007f\"", "\"unit\\u001f\"", "\"\\u0001\"", "\"\\n\""})
    CHECKV(parse(baseJson(both(q(kid), q(sig)), notes), m) == ManifestError::NotesControl, notes);
  for (const char *notes : {"\"plain text.\"", "\"\"", "null", "\"space and ~ and \\u0020\"", "\"Atualiza\\u00e7\\u00e3o: ecr\\u00e3 (\\u2713)\"", "\"\\ud83d\\ude80\"", "\"quote \\\" and slash \\\\ and \\/\""})
    CHECKV(parse(baseJson("", notes), m) == ManifestError::None, notes);
  CHECK(parse(baseJson("", "123"), m) == ManifestError::Notes);                               // existing rules unchanged
  CHECK(strcmp(otalogic::manifestErrorText(ManifestError::NotesControl), "notes hold a control character") == 0);
  CHECK(strlen(otalogic::manifestErrorText(ManifestError::NotesControl)) <= 46);
  // 200 characters of 4 bytes each are fine; 201 are not; both without control characters
  std::string rocket;
  for (int i = 0; i < 200; i++) rocket += "\xF0\x9F\x9A\x80";
  CHECK(parse(baseJson(both(q(kid), q(sig)), "\"" + rocket + "\""), m) == ManifestError::None && strlen(m.notes) == 800);
  CHECK(parse(baseJson("", "\"" + rocket + "\xF0\x9F\x9A\x80\""), m) == ManifestError::Notes);
  CHECK(parse(baseJson("", "\"\\ud83d\\ude80\""), m) == ManifestError::None && strcmp(m.notes, "\xF0\x9F\x9A\x80") == 0);  // an escaped pair is the same character
}

// ---- the canonical bytes -------------------------------------------------------------------------------------------------------------------------
static void testCanonical() {
  SECTION("canonical bytes: layout, sizes, guards");
  Manifest m;
  char buf[CANONICAL_MAX];
  size_t len = 0;
  CHECK(parse(baseJson(""), m) == ManifestError::None && canonicalBytes(m, buf, sizeof buf, len));
  std::string got(buf, len);
  CHECK(got == "ledmatrix-update-v1\nversion=0.11.0\nfile=ledmatrix-0.11.0.bin\nsize=1412537\nsha256=2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\nnotes=Test release: signed manifest vectors.");
  CHECK(got.back() != '\n');  // no line feed after the last line
  int lines = 0;
  for (char ch : got) lines += ch == '\n';
  CHECK(lines == 5);          // six lines, five line feeds
  CHECK(got.compare(0, 20, "ledmatrix-update-v1\n") == 0 && strcmp(CANONICAL_PREFIX, "ledmatrix-update-v1") == 0);
  // empty notes: the last line is exactly "notes="
  CHECK(parse(baseJson("", "\"\""), m) == ManifestError::None && canonicalBytes(m, buf, sizeof buf, len));
  got.assign(buf, len);
  CHECK(got.size() > 7 && got.substr(got.size() - 7) == "\nnotes=");
  // a missing notes field and a null one give the same bytes as empty notes
  std::string a = got;
  Manifest m2;
  std::string noNotes = "{\"version\":\"0.11.0\",\"file\":\"ledmatrix-0.11.0.bin\",\"size\":1412537,\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\"}";
  CHECK(parse(noNotes, m2) == ManifestError::None && canonicalBytes(m2, buf, sizeof buf, len) && std::string(buf, len) == a);
  CHECK(parse(baseJson("", "null"), m2) == ManifestError::None && canonicalBytes(m2, buf, sizeof buf, len) && std::string(buf, len) == a);
  // the size is the plain decimal number: no sign, no leading zeros, no exponent
  for (unsigned long size : {102401ul, 1412537ul, 2097152ul}) {
    std::string j = "{\"version\":\"0.11.0\",\"file\":\"ledmatrix-0.11.0.bin\",\"size\":" + std::to_string(size) + ",\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\"}";
    CHECK(parse(j, m2) == ManifestError::None && canonicalBytes(m2, buf, sizeof buf, len));
    CHECK(std::string(buf, len).find("\nsize=" + std::to_string(size) + "\n") != std::string::npos);
  }
  // the version and the file as the manifest wrote them (leading zeros are accepted by the manifest rules, and kept)
  std::string z = "{\"version\":\"0001.0002.0003\",\"file\":\"ledmatrix-0001.0002.0003.bin\",\"size\":1412537,\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\"}";
  CHECK(parse(z, m2) == ManifestError::None && canonicalBytes(m2, buf, sizeof buf, len));
  CHECK(std::string(buf, len).find("\nversion=0001.0002.0003\nfile=ledmatrix-0001.0002.0003.bin\n") != std::string::npos);

  // the longest possible text fits, with room to spare (the contract says under 1,024 bytes)
  std::string rocket;
  for (int i = 0; i < 200; i++) rocket += "\xF0\x9F\x9A\x80";
  std::string worst = "{\"version\":\"9999.9999.9999\",\"file\":\"ledmatrix-9999.9999.9999.bin\",\"size\":2097152,\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\",\"notes\":\"" + rocket + "\"}";
  CHECK(parse(worst, m2) == ManifestError::None);
  CHECK(canonicalBytes(m2, buf, sizeof buf, len) && len < CANONICAL_MAX && len > 900);
  printf("    the longest canonical text is %zu bytes (limit %zu)\n", len, CANONICAL_MAX);
  size_t exact = len;
  CHECK(!canonicalBytes(m2, buf, exact, len) && len == 0);       // no room for the NUL
  CHECK(canonicalBytes(m2, buf, exact + 1, len) && len == exact);
  CHECK(!canonicalBytes(m2, buf, 10, len));
  CHECK(!canonicalBytes(m2, buf, 0, len));
  CHECK(!canonicalBytes(m2, nullptr, 100, len));
  // a signed manifest with the longest notes stays far below the 2,048-byte answer limit (the contract says under 1.2 KB)
  std::string ws = "{\"version\":\"9999.9999.9999\",\"file\":\"ledmatrix-9999.9999.9999.bin\",\"size\":2097152,\"sha256\":\"2ba21a04f86b6da51b269a6eca124355345286cb4efff64048bda4518b05d2fa\",\"notes\":\"" +
                   [] { std::string r; for (int i = 0; i < 200; i++) r += "\xF0\x9F\x9A\x80"; return r; }() + "\",\"keyid\":\"f864ea3b\",\"sig\":\"" + std::string(128, 'a') + "\"}";
  CHECK(ws.size() < 1300 && ws.size() <= otalogic::MANIFEST_MAX_BYTES);
  CHECK(parse(ws, m2) == ManifestError::None && m2.sigFields == SigFields::Present);

  // the builder's own guard: a field with a control character is refused even if it never came from parseManifest
  Manifest bad = m2;
  strcpy(bad.notes, "one\ntwo");
  CHECK(!canonicalBytes(bad, buf, sizeof buf, len));
  bad = m2;
  strcpy(bad.version, "0.11.0\n");
  CHECK(!canonicalBytes(bad, buf, sizeof buf, len));
  bad = m2;
  bad.file[3] = 0x7F;
  CHECK(!canonicalBytes(bad, buf, sizeof buf, len));
  bad = m2;
  bad.sha256[0] = 0x01;
  CHECK(!canonicalBytes(bad, buf, sizeof buf, len));
}

// ---- every vector ------------------------------------------------------------------------------------------------------------------------------------
static void runVectors(Crypto &real, StandIn *table, const char *label) {
  printf("%s: every vector through parseManifest and checkManifest\n", label);
  std::vector<std::string> pins = pinnedHexes();
  Keys pinned = keysFor(pins);
  CHECK(pinned.count == 2);
  if (table) {  // the stand-in learns the signatures that are valid: the vectors that say "ok"
    for (JsonObjectConst v : vec["vectors"].as<JsonArrayConst>()) {
      if (std::string(v["expect"].as<const char *>()) != "ok") continue;
      std::string id = v["manifest"]["keyid"].as<std::string>();
      table->learn(pubOfId(id), v["canonical_sha256"].as<std::string>(), v["manifest"]["sig"].as<std::string>());
    }
  }
  Counting counting(real);
  std::map<std::string, int> seen;
  int n = 0;
  for (JsonObjectConst v : vec["vectors"].as<JsonArrayConst>()) {
    std::string name = v["name"].as<std::string>(), expect = v["expect"].as<std::string>();
    seen[expect]++;
    n++;
    std::string text = manifestText(v);
    Manifest m;
    ManifestError e = otalogic::parseManifest(text.c_str(), text.size(), m);
    if (expect == "manifest_invalid") {  // refused by the existing rules before any signature is looked at
      CHECKV(e != ManifestError::None, name);
      continue;
    }
    CHECKV(e == ManifestError::None, name);
    if (e != ManifestError::None) continue;
    SigFields wantShape = expect == "unsigned" ? SigFields::Absent : expect == "malformed" ? SigFields::Malformed : SigFields::Present;
    CHECKV(m.sigFields == wantShape, name);

    // the canonical bytes are the vector's, and so are their hash and hex
    if (v["canonical"].is<const char *>()) {
      char buf[CANONICAL_MAX];
      size_t len = 0;
      CHECKV(canonicalBytes(m, buf, sizeof buf, len), name);
      std::string want = v["canonical"].as<std::string>();
      CHECKV(std::string(buf, len) == want, name);
      CHECKV(hexOf(std::string(buf, len)) == v["canonical_hex"].as<std::string>(), name);
      uint8_t d[32];
      sha256((const uint8_t *)buf, len, d);
      CHECKV(hex(d, 32) == v["canonical_sha256"].as<std::string>(), name);
    }

    // enforcement on: the verdict is the vector's
    counting.reset();
    SigResult r = checkManifest(true, m, pinned, counting);
    CHECKV(r.sig == expectedSig(expect), name + ": got " + sigName(r.sig));
    if (r.sig != expectedSig(expect)) continue;
    if (expect == "ok") {
      CHECKV(strcmp(r.keyid, v["manifest"]["keyid"].as<const char *>()) == 0, name);
      CHECKV(counting.verifyCalls == 1 && counting.pubs[0] == pubOfId(r.keyid), name);
    } else {
      CHECKV(r.keyid[0] == 0, name);
    }
    // the cheap refusals run no verification; r or s out of range does not either; anything else is verified exactly once (the id picks one key)
    bool cheap = expect == "unsigned" || expect == "malformed" || expect == "unknown_key";
    bool outOfRange = name == "bad-signature-all-zero" || name == "bad-signature-r-equals-n" || name == "bad-signature-s-equals-n";
    if (cheap || outOfRange) CHECKV(counting.verifyCalls == 0, name);
    else CHECKV(counting.verifyCalls == 1, name);
    if (expect == "unsigned" || expect == "malformed") CHECKV(counting.shaCalls == 0, name);  // not even the key ids are derived for a shape refusal
    // a refusal is never "could not check", and the words are the contract's
    CHECKV(failFor(r.sig) == (expect == "ok" ? Fail::None : expect == "unsigned" ? Fail::Unsigned : expect == "malformed" ? Fail::ManifestInvalid : expect == "unknown_key" ? Fail::UnknownKey : Fail::BadSignature), name);

    // a development build looks at nothing: the same manifest is NotChecked and no cryptography runs
    counting.reset();
    SigResult off = checkManifest(false, m, pinned, counting);
    CHECKV(off.sig == Sig::NotChecked && off.keyid[0] == 0 && counting.shaCalls == 0 && counting.verifyCalls == 0, name);
  }
  printf("    %d vectors\n", n);
  CHECK(n >= 32);
  for (const char *k : {"ok", "unsigned", "malformed", "unknown_key", "bad_signature", "manifest_invalid"}) CHECKV(seen[k] > 0, k);
  CHECK(seen["ok"] == 5 && seen["unsigned"] == 1 && seen["malformed"] == 9 && seen["unknown_key"] == 2 && seen["bad_signature"] == 13 && seen["manifest_invalid"] == 2);
}

// ---- three-valued results, the order of the checks, the policy -------------------------------------------------------------------------------------
static Manifest firstOk() {
  for (JsonObjectConst v : vec["vectors"].as<JsonArrayConst>()) {
    if (std::string(v["name"].as<const char *>()) != "ok-basic") continue;
    std::string text = manifestText(v);
    Manifest m;
    otalogic::parseManifest(text.c_str(), text.size(), m);
    return m;
  }
  return Manifest();
}

static void testThreeValued() {
  SECTION("three-valued: a check that could not run is not a bad signature");
  Manifest m = firstOk();
  CHECK(m.sigFields == SigFields::Present && strcmp(m.keyid, "f864ea3b") == 0);
  std::vector<std::string> pins = pinnedHexes();
  Keys pinned = keysFor(pins);
  StandIn s;
  Counting c(s);
  s.forceVerify = true;
  s.forced = Verify::CouldNot;
  SigResult r = checkManifest(true, m, pinned, c);
  CHECK(r.sig == Sig::CheckFailed && c.verifyCalls == 1 && r.keyid[0] == 0);
  CHECK(failFor(Sig::CheckFailed) == Fail::SignatureCheckFailed && failFor(Sig::CheckFailed) != failFor(Sig::BadSignature));
  s.forced = Verify::Invalid;
  CHECK(checkManifest(true, m, pinned, c).sig == Sig::BadSignature);
  s.forced = Verify::Valid;
  r = checkManifest(true, m, pinned, c);
  CHECK(r.sig == Sig::Ok && strcmp(r.keyid, "f864ea3b") == 0);
  s.forceVerify = false;

  // SHA-256 failing (the hardware, an allocation) is a fault, never an attack, and nothing is verified
  s.shaFails = true;
  c.reset();
  CHECK(checkManifest(true, m, pinned, c).sig == Sig::CheckFailed && c.verifyCalls == 0);
  s.shaFails = false;
  s.shaFailsForBig = true;  // the key ids work, the hash of the canonical bytes does not
  c.reset();
  CHECK(checkManifest(true, m, pinned, c).sig == Sig::CheckFailed && c.verifyCalls == 0 && c.shaCalls >= 3);
  s.shaFailsForBig = false;
  // ...but the shape refusals are still given when SHA-256 is broken: they need none
  Manifest u = m;
  u.sigFields = SigFields::Absent;
  s.shaFails = true;
  CHECK(checkManifest(true, u, pinned, c).sig == Sig::Unsigned);
  u.sigFields = SigFields::Malformed;
  CHECK(checkManifest(true, u, pinned, c).sig == Sig::Malformed);
  s.shaFails = false;

  SECTION("key id: every one of its 8 characters matters; a near miss is an unknown key and costs no verification");
  for (const KeyInfo &ki : keyInfos) {
    if (!ki.pinned) continue;
    for (size_t pos = 0; pos < 8; pos++) {
      Manifest near = m;
      memcpy(near.keyid, ki.id.c_str(), 9);
      near.keyid[pos] = near.keyid[pos] == '0' ? '1' : '0';
      c.reset();
      CHECK(checkManifest(true, near, pinned, c).sig == Sig::UnknownKey && c.verifyCalls == 0);
    }
  }
  Manifest shortId = m;
  shortId.keyid[7] = 0;  // 7 characters: a prefix of the real id is not the id
  c.reset();
  CHECK(checkManifest(true, shortId, pinned, c).sig == Sig::UnknownKey && c.verifyCalls == 0);

  SECTION("two pinned keys that collide in id: both are tried, in order, and a fault does not hide behind a refusal");
  CollidingIds col;
  Counting cc(col);
  std::vector<std::string> two = {keyInfos[0].pubHex, keyInfos[1].pubHex};  // A and B (different keys, the same id for this crypto)
  Keys both = keysFor(two);
  Manifest k = m;
  strcpy(k.keyid, "a5a5a5a5");
  struct Run { std::vector<Verify> script; Sig want; int calls; };
  const Verify V = Verify::Valid, I = Verify::Invalid, X = Verify::CouldNot;
  std::vector<Run> runs = {
      {{V}, Sig::Ok, 1},            // the first valid one ends it
      {{I, V}, Sig::Ok, 2},         // the second key verifies
      {{X, V}, Sig::Ok, 2},         // a fault on one key does not stop the other from being tried
      {{I, I}, Sig::BadSignature, 2},
      {{X, I}, Sig::CheckFailed, 2},   // one of the two could not run: the verdict cannot be "bad signature"
      {{I, X}, Sig::CheckFailed, 2},
      {{X, X}, Sig::CheckFailed, 2},
  };
  for (const Run &run : runs) {
    cc.reset();
    col.script = run.script;
    SigResult rr = checkManifest(true, k, both, cc);
    CHECK(rr.sig == run.want && cc.verifyCalls == run.calls);
    CHECK(cc.pubs.size() >= 1 && cc.pubs[0] == two[0] && (cc.pubs.size() < 2 || cc.pubs[1] == two[1]));  // in the order they are pinned
  }
  // with distinct ids only the key whose id matches is tried: B's id, B is tried (once), A is not
  Counting cd(s);
  Manifest kb = m;
  strcpy(kb.keyid, "db10c343");
  cd.reset();
  r = checkManifest(true, kb, pinned, cd);
  CHECK(r.sig == Sig::BadSignature && cd.verifyCalls == 1 && cd.pubs[0] == keyInfos[1].pubHex);  // A's signature under B's id: refused

  SECTION("policy and the build's keys");
  Keys none;
  cd.reset();
  CHECK(checkManifest(true, m, none, cd).sig == Sig::UnknownKey && cd.verifyCalls == 0);  // (the build refuses to compile like this)
  Keys three;
  three.count = 3;
  CHECK(checkManifest(true, m, three, cd).sig == Sig::CheckFailed);
  std::vector<std::string> brokenKey = {"04" + std::string(128, 'g')};
  CHECK(checkManifest(true, m, keysFor(brokenKey), cd).sig == Sig::CheckFailed);
  std::vector<std::string> mixed = {keyInfos[0].pubHex, "not a key"};
  CHECK(checkManifest(true, m, keysFor(mixed), cd).sig == Sig::CheckFailed);
  // the policy for every state of the fields: NotChecked, whatever the manifest says
  for (SigFields f : {SigFields::Absent, SigFields::Present, SigFields::Malformed}) {
    Manifest p = m;
    p.sigFields = f;
    cd.reset();
    CHECK(checkManifest(false, p, pinned, cd).sig == Sig::NotChecked && cd.shaCalls == 0 && cd.verifyCalls == 0);
    CHECK(checkManifest(false, p, none, cd).sig == Sig::NotChecked);   // a development build needs no keys at all
    CHECK(checkManifest(false, p, three, cd).sig == Sig::NotChecked);
  }
  CHECK(REQUIRED == (UPDATE_REQUIRE_SIGNATURE != 0));
  CHECK(strcmp(modeName(), UPDATE_REQUIRE_SIGNATURE ? "required" : "off") == 0);
  CHECK(!hasControlByte(modeName()));
}

static void testWords() {
  SECTION("the words: Sig -> Fail -> raw text -> sentence");
  struct Row { Sig s; Fail f; const char *refused; const char *raw; const char *sentence; bool refusal; };
  static const Row rows[] = {
      {Sig::Ok, Fail::None, nullptr, "", "", false},
      {Sig::NotChecked, Fail::None, nullptr, "", "", false},
      {Sig::Unsigned, Fail::Unsigned, "unsigned", "firmware manifest is not signed",
       "The service offers a firmware update that is not signed, so it was not offered. This board accepts only updates signed with the key built into it.", true},
      {Sig::Malformed, Fail::ManifestInvalid, "malformed", "firmware manifest is not valid",
       "The firmware details from the service are not valid, so no update is offered.", true},
      {Sig::UnknownKey, Fail::UnknownKey, "unknown_key", "firmware signed with an unknown key",
       "The firmware update is signed with a key this board does not know, so it was refused and nothing was changed. A USB flash may be needed.", true},
      {Sig::BadSignature, Fail::BadSignature, "bad_signature", "firmware signature is not valid",
       "The signature on the firmware update does not check out, so it was refused and nothing was changed. Check that the service address is yours.", true},
      {Sig::CheckFailed, Fail::SignatureCheckFailed, "check_failed", "could not check the firmware signature",
       "The board could not check the signature on the firmware update (it ran short of memory). Nothing was changed. It tries again at the next check.", true},
  };
  std::set<std::string> codes, sentences;
  for (const Row &r : rows) {
    CHECK(failFor(r.s) == r.f);
    CHECK(isRefusal(r.s) == r.refusal);
    if (r.refused) CHECK(strcmp(refusedName(r.s), r.refused) == 0 && codes.insert(r.refused).second);
    else CHECK(refusedName(r.s) == nullptr);
    if (r.f == Fail::None) continue;
    CHECK(strcmp(otalogic::failRaw(r.f), r.raw) == 0);
    char why[160], detail[80];
    problemWords(r.raw, why, sizeof why, detail, sizeof detail);
    CHECKV(strcmp(why, r.sentence) == 0, r.raw);
    CHECK(strlen(why) <= 159);
    CHECK(strlen(r.sentence) <= 159);
    sentences.insert(why);
    // the detail travels after the macro, the sentence does not change
    std::string withDetail = std::string(otalogic::failRaw(r.f)) + ": " + refusalDetail(r.s);
    char why2[160];
    problemWords(withDetail.c_str(), why2, sizeof why2, detail, sizeof detail);
    CHECK(strcmp(why2, r.sentence) == 0);
    if (r.s == Sig::Malformed) CHECK(std::string(detail).find("the signature fields are malformed") != std::string::npos);
    else CHECK(strcmp(refusalDetail(r.s), "") == 0);
  }
  CHECK(sentences.size() == 5);  // five different sentences for the five refusals
  CHECK(strcmp(refusalDetail(Sig::Malformed), "the signature fields are malformed") == 0);
  // the recognising phrases of the contract
  for (const char *phrase : {"update that is not signed", "signed with a key this board does not know", "signature on the firmware update does not check out",
                             "could not check the signature on the firmware update"}) {
    char why[160], detail[80];
    problemWords(phrase, why, sizeof why, detail, sizeof detail);
    CHECKV(strcmp(why, phrase) != 0, phrase);  // worded, not passed through
  }
  CHECK(strlen(otalogic::failRaw(Fail::Unsigned)) < 60 && strlen(otalogic::failRaw(Fail::UnknownKey)) < 60 &&
        strlen(otalogic::failRaw(Fail::BadSignature)) < 60 && strlen(otalogic::failRaw(Fail::SignatureCheckFailed)) < 60);
}

// ---- what the portal gets and what the image says -----------------------------------------------------------------------------------------------------
static std::string jsonOf(JsonDocument &d) {
  std::string s;
  serializeJson(d, s);
  return s;
}
static bool hasLongHexRun(const std::string &s) {  // a signature or a key would show as a long run of hex
  size_t run = 0;
  for (char ch : s) {
    run = ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) ? run + 1 : 0;
    if (run > 8) return true;
  }
  return false;
}

static void testJsonAndMarker() {
  SECTION("/api/update: the signing object and available.signedBy");
  PinnedIds ids;
  ids.count = 2;
  strcpy(ids.id[0], "f864ea3b");
  strcpy(ids.id[1], "db10c343");
  JsonDocument d;
  fillSigningJson(d["signing"].to<JsonObject>(), true, ids, Sig::Ok);
  CHECK(jsonOf(d) == "{\"signing\":{\"required\":true,\"keys\":[\"f864ea3b\",\"db10c343\"],\"refused\":null}}");
  const std::pair<Sig, const char *> codes[] = {{Sig::Unsigned, "unsigned"}, {Sig::Malformed, "malformed"}, {Sig::UnknownKey, "unknown_key"},
                                                {Sig::BadSignature, "bad_signature"}, {Sig::CheckFailed, "check_failed"}};
  for (const auto &c : codes) {
    JsonDocument e;
    fillSigningJson(e["signing"].to<JsonObject>(), true, ids, c.first);
    CHECK(jsonOf(e) == std::string("{\"signing\":{\"required\":true,\"keys\":[\"f864ea3b\",\"db10c343\"],\"refused\":\"") + c.second + "\"}}");
  }
  JsonDocument off;
  fillSigningJson(off["signing"].to<JsonObject>(), false, ids, Sig::NotChecked);
  CHECK(jsonOf(off) == "{\"signing\":{\"required\":false,\"keys\":[\"f864ea3b\",\"db10c343\"],\"refused\":null}}");
  PinnedIds noIds;
  JsonDocument e0;
  fillSigningJson(e0["signing"].to<JsonObject>(), true, noIds, Sig::Ok);
  CHECK(jsonOf(e0) == "{\"signing\":{\"required\":true,\"keys\":[],\"refused\":null}}");  // before the ids are derived: an empty list, never garbage
  PinnedIds one;
  one.count = 1;
  strcpy(one.id[0], "db10c343");
  JsonDocument e1;
  fillSigningJson(e1["signing"].to<JsonObject>(), true, one, Sig::Ok);
  CHECK(jsonOf(e1) == "{\"signing\":{\"required\":true,\"keys\":[\"db10c343\"],\"refused\":null}}");
  PinnedIds crazy;
  crazy.count = 9;  // a count past the array is cut, not read
  strcpy(crazy.id[0], "f864ea3b");
  strcpy(crazy.id[1], "db10c343");
  JsonDocument e9;
  fillSigningJson(e9["signing"].to<JsonObject>(), true, crazy, Sig::Ok);
  CHECK(jsonOf(e9) == "{\"signing\":{\"required\":true,\"keys\":[\"f864ea3b\",\"db10c343\"],\"refused\":null}}");

  JsonDocument a;
  JsonObject av = a["available"].to<JsonObject>();
  av["version"] = "0.11.0";
  fillSignedBy(av, "f864ea3b");
  CHECK(jsonOf(a) == "{\"available\":{\"version\":\"0.11.0\",\"signedBy\":\"f864ea3b\"}}");
  JsonDocument a2;
  JsonObject av2 = a2["available"].to<JsonObject>();
  fillSignedBy(av2, "");
  CHECK(jsonOf(a2) == "{\"available\":{\"signedBy\":null}}");
  JsonDocument a3;
  JsonObject av3 = a3["available"].to<JsonObject>();
  fillSignedBy(av3, nullptr);
  CHECK(jsonOf(a3) == "{\"available\":{\"signedBy\":null}}");
  // nothing secret or unprintable goes out: key ids are 8 hex characters and there is no longer run of hex anywhere
  for (const std::string &s : {jsonOf(d), jsonOf(off), jsonOf(a)}) CHECK(!hasControlByte(s) && !hasLongHexRun(s));

  SECTION("the image marker (shared/manifest-signing.md, section 4)");
  std::string tag(UPDATESIG_TAG);
  std::string want = std::string("\x01" "LEDMATRIX-FW-SIGNING" "\x02") + (UPDATE_REQUIRE_SIGNATURE ? "required" : "off") + "\x03";
  CHECK(tag == want);
  CHECK(hexOf(std::string(UPDATESIG_TAG_PREFIX)) == "014c45444d41545249582d46572d5349474e494e4702");  // \x01LEDMATRIX-FW-SIGNING\x02
  CHECK(tag.size() == strlen(UPDATESIG_TAG) && tag.front() == '\x01' && tag.back() == '\x03');
  CHECK(std::count(tag.begin(), tag.end(), '\x01') == 1 && std::count(tag.begin(), tag.end(), '\x02') == 1 && std::count(tag.begin(), tag.end(), '\x03') == 1);
#if UPDATE_REQUIRE_SIGNATURE
  CHECK(hexOf(tag) == "014c45444d41545249582d46572d5349474e494e47027265717569726564" "03");  // ...required\x03
#else
  CHECK(hexOf(tag) == "014c45444d41545249582d46572d5349474e494e4702" "6f6666" "03");          // ...off\x03
#endif
  // the version scanner of otalogic.h must not mistake it for the version marker, nor the other way round
  otalogic::VersionTagScanner sc;
  sc.feed((const uint8_t *)tag.data(), tag.size());
  CHECK(!sc.found());
  CHECK(tag.find("LEDMATRIX-FW-VERSION") == std::string::npos && std::string(OTA_VERSION_TAG_PREFIX).find("SIGNING") == std::string::npos);
  // the text that goes into JSON is not the marker
  CHECK(std::string(modeName()).find('\x01') == std::string::npos && std::string(modeName()) != tag);
}

// ---- the keys this build really pins -------------------------------------------------------------------------------------------------------------------
static void testRealKeys(Crypto &crypto) {
  SECTION("update_keys.h: the keys this build pins");
  Keys real = pinnedKeys();
  CHECK(real.count == (size_t)UPDATE_KEY_COUNT && real.count >= 1 && real.count <= MAX_KEYS);
  if (!REQUIRED) CHECK(real.count <= MAX_KEYS);
  for (size_t i = 0; i < real.count; i++) {
    CHECK(keyTextOk(real.hex[i]));
    uint8_t pub[PUBKEY_LEN];
    CHECK(decodePublicKey(real.hex[i], pub));
    for (const KeyInfo &k : keyInfos) CHECK(k.pubHex != real.hex[i]);  // the vectors' test keys are never release keys
  }
  if (real.count == 2) CHECK(strcmp(real.hex[0], real.hex[1]) != 0);
  PinnedIds ids;
  CHECK(derivePinnedIds(real, crypto, ids) && ids.count == real.count);
  printf("    pinned key ids:");
  for (size_t i = 0; i < ids.count; i++) printf(" %s", ids.id[i]);
  printf("\n");
  if (ids.count == 2) CHECK(strcmp(ids.id[0], ids.id[1]) != 0);
}

#ifdef WITH_MBEDTLS
// ---- the board's own code against a host mbedTLS -----------------------------------------------------------------------------------------------------
static void testMbedtls() {
  SECTION("mbedTLS: SHA-256 agrees with this file's own, at the block edges");
  MbedCrypto mc;
  for (size_t n : {0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 200, 1000}) {
    std::string msg(n, 'x');
    for (size_t i = 0; i < n; i++) msg[i] = (char)('a' + (i * 7) % 26);
    uint8_t a[32], b[32];
    CHECK(mc.sha256((const uint8_t *)msg.data(), n, a));
    sha256((const uint8_t *)msg.data(), n, b);
    CHECK(memcmp(a, b, 32) == 0);
  }

  SECTION("mbedTLS: return codes -> the three values");
  CHECK(verdictFromEcp(0) == Verify::Valid);
  CHECK(verdictFromEcp(MBEDTLS_ERR_ECP_VERIFY_FAILED) == Verify::Invalid);
  CHECK(verdictFromEcp(MBEDTLS_ERR_ECP_BAD_INPUT_DATA) == Verify::Invalid);
  CHECK(verdictFromEcp(MBEDTLS_ERR_ECP_INVALID_KEY) == Verify::Invalid);
  CHECK(verdictFromEcp(MBEDTLS_ERR_ECP_ALLOC_FAILED) == Verify::CouldNot);        // out of memory is not an attack
  CHECK(verdictFromEcp(MBEDTLS_ERR_MPI_ALLOC_FAILED) == Verify::CouldNot);
  CHECK(verdictFromEcp(MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE) == Verify::CouldNot);
  CHECK(verdictFromEcp(MBEDTLS_ERR_ECP_IN_PROGRESS) == Verify::CouldNot);
  CHECK(verdictFromEcp(MBEDTLS_ERR_MPI_NOT_ACCEPTABLE) == Verify::CouldNot);
  CHECK(verdictFromEcp(MBEDTLS_ERR_MPI_BAD_INPUT_DATA) == Verify::CouldNot);
  CHECK(verdictFromEcp(-1) == Verify::CouldNot && verdictFromEcp(1) == Verify::CouldNot);

  SECTION("mbedTLS: every vector's signature straight through p256VerifyMbedtls (r or s out of range: the library's own check)");
  int valid = 0, invalid = 0;
  for (JsonObjectConst v : vec["vectors"].as<JsonArrayConst>()) {
    std::string name = v["name"].as<std::string>(), expect = v["expect"].as<std::string>();
    std::string mt = manifestText(v);
    Manifest m;
    if (otalogic::parseManifest(mt.c_str(), mt.size(), m) != ManifestError::None || m.sigFields != SigFields::Present) continue;
    std::string pub = pubOfId(m.keyid);
    if (pub.empty()) continue;  // a key id nobody has (the "relabelled" vector)
    char buf[CANONICAL_MAX];
    size_t len = 0;
    CHECK(canonicalBytes(m, buf, sizeof buf, len));
    uint8_t digest[32];
    sha256((const uint8_t *)buf, len, digest);
    std::vector<uint8_t> p = unhex(pub);
    bool wantValid = expect == "ok" || name == "unknown-key-valid-signature";  // key C's signature is genuine; C is just not pinned
    Verify got = p256VerifyMbedtls(p.data(), digest, m.sig);
    CHECKV(got == (wantValid ? Verify::Valid : Verify::Invalid), name);
    (wantValid ? valid : invalid)++;
  }
  printf("    %d valid, %d invalid, straight through the library\n", valid, invalid);
  CHECK(valid == 6 && invalid == 13);

  SECTION("mbedTLS: keys that are not usable are Invalid, not a fault");
  std::vector<uint8_t> pubA = unhex(keyInfos[0].pubHex), dig(32, 0x11), sg(64, 0x22);
  CHECK(p256VerifyMbedtls(pubA.data(), dig.data(), sg.data()) == Verify::Invalid);   // a real key, a signature that is not one
  std::vector<uint8_t> offCurve = pubA;
  offCurve[64] ^= 1;                                                                   // one bit off: not a point of the curve
  CHECK(p256VerifyMbedtls(offCurve.data(), dig.data(), sg.data()) == Verify::Invalid);
  std::vector<uint8_t> origin(65, 0);
  origin[0] = 0x04;                                                                    // (0, 0)
  CHECK(p256VerifyMbedtls(origin.data(), dig.data(), sg.data()) == Verify::Invalid);
  std::vector<uint8_t> bigX = pubA;
  for (int i = 1; i <= 32; i++) bigX[i] = 0xFF;                                        // X not below p
  CHECK(p256VerifyMbedtls(bigX.data(), dig.data(), sg.data()) == Verify::Invalid);
  std::vector<uint8_t> zeros(64, 0);
  CHECK(p256VerifyMbedtls(pubA.data(), dig.data(), zeros.data()) == Verify::Invalid);

  SECTION("mbedTLS: the real keys in update_keys.h are points on the curve");
  Keys real = pinnedKeys();
  for (size_t i = 0; i < real.count; i++) {
    uint8_t pub[PUBKEY_LEN];
    CHECK(decodePublicKey(real.hex[i], pub));
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    CHECK(mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0);
    CHECK(mbedtls_ecp_point_read_binary(&grp, &Q, pub, PUBKEY_LEN) == 0);
    CHECK(mbedtls_ecp_check_pubkey(&grp, &Q) == 0);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
  }
  for (const KeyInfo &k : keyInfos) {  // and so are the vectors' (a sanity check on this very test)
    uint8_t pub[PUBKEY_LEN];
    CHECK(decodePublicKey(k.pubHex.c_str(), pub));
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    CHECK(mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 && mbedtls_ecp_point_read_binary(&grp, &Q, pub, PUBKEY_LEN) == 0 && mbedtls_ecp_check_pubkey(&grp, &Q) == 0);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
  }

  SECTION("mbedTLS: time of one verification on this laptop (relative only: the board is slower)");
  {
    std::vector<uint8_t> p = unhex(keyInfos[0].pubHex);
    for (JsonObjectConst v : vec["vectors"].as<JsonArrayConst>()) {
      if (std::string(v["name"].as<const char *>()) != "ok-basic") continue;
      std::vector<uint8_t> d = unhex(v["canonical_sha256"].as<std::string>()), s = unhex(v["manifest"]["sig"].as<std::string>());
      auto t0 = std::chrono::steady_clock::now();
      const int N = 50;
      bool all = true;
      for (int i = 0; i < N; i++) all = all && p256VerifyMbedtls(p.data(), d.data(), s.data()) == Verify::Valid;
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
      CHECK(all);
      printf("    %.0f microseconds per verification (host)\n", us);
    }
  }
}
#endif

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "shared/manifest-signing-vectors.json";
  std::ifstream f(path);
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (text.empty() || deserializeJson(vec, text)) {
    printf("cannot read the vectors file %s\n", path);
    return 2;
  }
  for (JsonObjectConst k : vec["keys"].as<JsonArrayConst>())
    keyInfos.push_back({k["name"].as<std::string>(), k["id"].as<std::string>(), k["public_key_hex"].as<std::string>(), k["pinned"].as<bool>()});
  printf("updatesig: %s mode, enforcement %s\n",
#ifdef WITH_MBEDTLS
         "real mbedTLS", REQUIRED ? "on" : "off (development build)");
#else
         "stand-in crypto", REQUIRED ? "on" : "off (development build)");
#endif

  testHex();
  testKeysAndIds();
  testRange();
  testManifestFields();
  testCanonical();
  testWords();
  testJsonAndMarker();
#ifdef WITH_MBEDTLS
  MbedCrypto mc;
  runVectors(mc, nullptr, "mbedTLS");
  testMbedtls();
  testRealKeys(mc);
#else
  StandIn table;
  runVectors(table, &table, "stand-in");
  testRealKeys(table);
#endif
  testThreeValued();

  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
