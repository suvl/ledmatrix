// Host test for arduino/ledmatrix/deviceauth.h (device authentication, scheme v1; tickets 37 and 39). It reads the shared
// known-answer vectors at run time, so the Go service, tools/provision.py and this code all prove the same bytes
// (shared/device-auth.md, shared/device-auth-vectors.json). It carries its own SHA-256 and HMAC for the host (the board uses
// mbedtls), which also double-checks the vectors. Run from the repo root; the one argument is the vectors file:
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" tools/test_deviceauth.cpp -o <scratch>/test_deviceauth && <scratch>/test_deviceauth shared/device-auth-vectors.json
// Also worth running now and then with the sanitizers (they catch buffer overruns and out-of-range indexes):
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Iarduino/ledmatrix -I"$HOME/Documents/Arduino/libraries/ArduinoJson/src" tools/test_deviceauth.cpp -o <scratch>/test_deviceauth_asan && <scratch>/test_deviceauth_asan shared/device-auth-vectors.json
// Every host name, ID, key and secret in here is a placeholder (example.com style, the vectors' own values).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "deviceauth.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(name) printf("%s\n", name)

// ---- SHA-256 and HMAC-SHA-256, the host's own (FIPS 180-4, RFC 2104) -------------------------------------------------------
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

static void hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t n, uint8_t out[32]) {
  uint8_t k[64] = {0}, ipad[64], opad[64];
  if (keyLen > 64) sha256(key, keyLen, k); else memcpy(k, key, keyLen);
  for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  uint8_t inner[32];
  Sha256 a; a.update(ipad, 64); a.update(msg, n); a.final(inner);
  Sha256 b; b.update(opad, 64); b.update(inner, 32); b.final(out);
}

static std::string hex(const uint8_t *p, size_t n) {
  static const char d[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}
static std::vector<uint8_t> unhex(const std::string &s) {
  std::vector<uint8_t> v;
  auto nib = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)(nib(s[i]) << 4 | nib(s[i + 1])));
  return v;
}
static std::string hmacHex(const std::vector<uint8_t> &key, const std::string &msg) {
  uint8_t out[32];
  hmacSha256(key.data(), key.size(), (const uint8_t *)msg.data(), msg.size(), out);
  return hex(out, 32);
}
static std::string trimAscii(const std::string &s) {  // the contract: surrounding space, tab, CR, LF
  size_t a = 0, b = s.size();
  while (a < b && strchr(" \t\r\n", s[a])) a++;
  while (b > a && strchr(" \t\r\n", s[b - 1])) b--;
  return s.substr(a, b - a);
}

// A table-driven CRC-32, written differently from the one in deviceauth.h, to check it against.
static uint32_t crcTable(const uint8_t *p, size_t n) {
  static uint32_t t[256];
  if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---- guard bytes around an output buffer: an overrun on either side shows up as a changed guard -------------------------------
template <size_t N> struct Guard {
  unsigned char pre[32];
  char buf[N];
  unsigned char post[32];
  Guard() { memset(pre, 0xA5, sizeof pre); memset(post, 0xA5, sizeof post); memset(buf, 'Z', N); }
  bool guardsOk() const {
    for (unsigned char b : pre) if (b != 0xA5) return false;
    for (unsigned char b : post) if (b != 0xA5) return false;
    return true;
  }
};

// ---- signers for the test: the host's HMAC under a key, a counter that wraps another one, and one that always fails ---------
struct HostSigner : devauth::Signer {
  std::vector<uint8_t> key;
  explicit HostSigner(std::vector<uint8_t> k) : key(std::move(k)) {}
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override { hmacSha256(key.data(), key.size(), msg, len, out); return true; }
};
struct CountingSigner : devauth::Signer {  // a drop-in for any signer: callers only know the interface
  devauth::Signer &inner;
  int calls = 0;
  size_t lastLen = 0;
  explicit CountingSigner(devauth::Signer &s) : inner(s) {}
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override { calls++; lastLen = len; return inner.sign(msg, len, out); }
};
struct FailingSigner : devauth::Signer {
  bool sign(const uint8_t *, size_t, uint8_t[devauth::SIG_LEN]) override { return false; }
};

// ---- the vectors file ----------------------------------------------------------------------------------------------------------
static JsonDocument vec;

static bool loadVectors(const char *path) {
  std::ifstream f(path);
  if (!f) return false;
  return !deserializeJson(vec, f);
}

// ---- helpers for records -------------------------------------------------------------------------------------------------------
static const char *ID = "aabbccddeeff";

// One sector as provision.py writes it: the record at offset 0, the rest 0xFF. The CRC comes from the table-driven version.
static std::vector<uint8_t> makeSector(const char *id, uint8_t flags, const uint8_t key[32], bool goodCrc = true) {
  std::vector<uint8_t> s(devauth::SECTOR_BYTES, 0xFF);
  s[0] = 'I'; s[1] = 'D'; s[2] = 'N'; s[3] = 'T';
  s[4] = 1; s[5] = 0;
  s[6] = 48; s[7] = 0;
  memcpy(&s[12], id, 12);
  s[24] = flags;
  s[25] = s[26] = s[27] = 0;
  memcpy(&s[28], key, 32);
  uint32_t crc = crcTable(&s[12], 48);
  if (!goodCrc) crc ^= 1;
  for (int i = 0; i < 4; i++) s[8 + i] = (uint8_t)(crc >> (8 * i));
  return s;
}
static void setCrc(std::vector<uint8_t> &s) {  // recompute the CRC after a payload edit, so only the edited field is wrong
  uint32_t crc = crcTable(&s[12], 48);
  for (int i = 0; i < 4; i++) s[8 + i] = (uint8_t)(crc >> (8 * i));
}
static bool allZero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i]) return false; return true; }
static bool recWiped(const devauth::IdentRecord &r) { return r.id[0] == 0 && r.flags == 0 && allZero(r.key, 32); }

static uint8_t KEY_A[32], KEY_B[32];

// ---- tests -----------------------------------------------------------------------------------------------------------------------
static void testPrimitives() {
  SECTION("SHA-256, HMAC and CRC-32: the host's own and the firmware's");
  uint8_t d[32];
  sha256((const uint8_t *)"abc", 3, d);
  CHECK(hex(d, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  sha256((const uint8_t *)"", 0, d);
  CHECK(hex(d, 32) == devauth::EMPTY_BODY_SHA256_HEX);
  // RFC 4231 test case 2 from the vectors file
  JsonVariantConst a = vec["primitive_rfc4231_case2"];
  CHECK(hmacHex(unhex(a["key_hex"].as<const char *>()), a["message"].as<const char *>()) == a["hmac"].as<const char *>());
  // CRC-32: the check value of the standard, and a second known text
  CHECK(devauth::crc32Ieee((const uint8_t *)"123456789", 9) == 0xCBF43926u);
  CHECK(devauth::crc32Ieee((const uint8_t *)"", 0) == 0);
  CHECK(devauth::crc32Ieee((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);
  uint8_t bytes[300];
  for (int i = 0; i < 300; i++) bytes[i] = (uint8_t)(i * 7 + 3);
  for (size_t n : {1u, 2u, 47u, 48u, 49u, 60u, 300u}) CHECK(devauth::crc32Ieee(bytes, n) == crcTable(bytes, n));
  // secureZero
  uint8_t z[40];
  memset(z, 0xEE, sizeof z);
  devauth::secureZero(z, 39);
  CHECK(allZero(z, 39) && z[39] == 0xEE);
  devauth::secureZero(z, 0);
  CHECK(z[39] == 0xEE);
}

static void testVectorKey() {
  SECTION("the master secret, the device ID and the device key (vectors)");
  std::string master = vec["master_secret"].as<const char *>();
  std::string padded = vec["master_secret_with_whitespace"].as<const char *>();
  std::string id = vec["device_id"].as<const char *>();
  std::string keyHex = vec["device_key_hex"].as<const char *>();
  CHECK(master.size() >= 32);
  CHECK(trimAscii(padded) == master);
  CHECK(padded != master);
  std::string trimmed = trimAscii(padded);
  std::vector<uint8_t> m(master.begin(), master.end()), mp(trimmed.begin(), trimmed.end());
  CHECK(hmacHex(m, id) == keyHex);
  CHECK(hmacHex(mp, id) == keyHex);  // the whitespace-trimmed master gives the same key
  // an untrimmed master is another key: the trimming is part of the contract
  std::vector<uint8_t> raw(padded.begin(), padded.end());
  CHECK(hmacHex(raw, id) != keyHex);
  CHECK(id == ID);
}

static void testDeviceId() {
  SECTION("device ID: exactly 12 lowercase hex characters");
  CHECK(devauth::validDeviceId("aabbccddeeff"));
  CHECK(devauth::validDeviceId("000000000000"));
  CHECK(devauth::validDeviceId("0123456789ab"));
  CHECK(devauth::validDeviceId(vec["device_id"].as<const char *>()));
  for (JsonVariantConst bad : vec["bad_device_ids"].as<JsonArrayConst>()) {
    CHECK(!devauth::validDeviceId(bad.as<const char *>()));
    CHECK(!devauth::validDeviceId(bad.as<const char *>(), strlen(bad.as<const char *>())));
  }
  CHECK(!devauth::validDeviceId(nullptr));
  CHECK(!devauth::validDeviceId(nullptr, 12));
  CHECK(!devauth::validDeviceId("aabbccddeeff", 11));
  CHECK(!devauth::validDeviceId("aabbccddeeff", 13));
  CHECK(!devauth::validDeviceId("aabbccddeeFF"));
  CHECK(!devauth::validDeviceId("aabbccddee ff"));
  CHECK(!devauth::validDeviceId("aabbccddee-f"));
  CHECK(!devauth::validDeviceId(" abbccddeeff"));
  const char withNul[13] = {'a', 'a', 'b', 'b', 'c', 'c', 'd', 'd', 'e', 'e', 'f', '\0', 'x'};  // a NUL in the middle of 12 bytes
  CHECK(!devauth::validDeviceId(withNul, 12));
  CHECK(!devauth::validDeviceId(withNul));
  // a text that is not terminated within 13 bytes is looked at no further
  const char longText[] = "aabbccddeeffaabbccddeeffaabbccddeeff";
  CHECK(!devauth::validDeviceId(longText));
  // the chip's MAC as an ID
  char out[13];
  const uint8_t mac1[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, mac2[6] = {0x00, 0x01, 0x0a, 0xf0, 0x1f, 0xff};
  devauth::formatDeviceId(mac1, out);
  CHECK(strcmp(out, "aabbccddeeff") == 0 && devauth::validDeviceId(out));
  devauth::formatDeviceId(mac2, out);
  CHECK(strcmp(out, "00010af01fff") == 0 && devauth::validDeviceId(out));
}

static void testRecordParse() {
  SECTION("identity record: one sector");
  devauth::IdentRecord r;
  std::vector<uint8_t> good = makeSector(ID, 0, KEY_A);
  CHECK(devauth::parseRecord(good.data(), good.size(), r) == devauth::RecordStatus::Ok);
  CHECK(strcmp(r.id, ID) == 0 && r.flags == 0 && !r.burned() && memcmp(r.key, KEY_A, 32) == 0);
  // a layout check against the contract's table, independent of makeSector
  CHECK(good[0] == 0x49 && good[1] == 0x44 && good[2] == 0x4E && good[3] == 0x54);  // 0x544E4449 little-endian = "IDNT"
  CHECK(devauth::le32(good.data()) == devauth::IDENT_MAGIC && devauth::IDENT_MAGIC == 0x544E4449u);
  CHECK(devauth::IDENT_RECORD_LEN == 60 && devauth::SECTOR_BYTES == 4096);
  // exactly the record's bytes are enough; the rest of the sector is never needed
  CHECK(devauth::parseRecord(good.data(), 60, r) == devauth::RecordStatus::Ok);
  CHECK(devauth::parseRecord(good.data(), 59, r) == devauth::RecordStatus::Short);
  CHECK(recWiped(r));
  CHECK(devauth::parseRecord(good.data(), 12, r) == devauth::RecordStatus::Short);
  CHECK(devauth::parseRecord(good.data(), 11, r) == devauth::RecordStatus::Short);
  CHECK(devauth::parseRecord(good.data(), 1, r) == devauth::RecordStatus::Short);
  CHECK(devauth::parseRecord(good.data(), 0, r) == devauth::RecordStatus::Short);  // a sector that could not be read
  CHECK(devauth::parseRecord(nullptr, 4096, r) == devauth::RecordStatus::Blank);   // there is no such sector
  // burned flag, and every other flag bit is ignored
  std::vector<uint8_t> burned = makeSector(ID, 1, KEY_A);
  CHECK(devauth::parseRecord(burned.data(), burned.size(), r) == devauth::RecordStatus::Ok && r.burned());
  std::vector<uint8_t> other = makeSector(ID, 0xFE, KEY_A);
  CHECK(devauth::parseRecord(other.data(), other.size(), r) == devauth::RecordStatus::Ok && !r.burned() && r.flags == 0xFE);
  // reserved bytes are inside the CRC and not interpreted
  std::vector<uint8_t> res = good;
  res[26] = 7;
  CHECK(devauth::parseRecord(res.data(), res.size(), r) == devauth::RecordStatus::BadCrc);
  setCrc(res);
  CHECK(devauth::parseRecord(res.data(), res.size(), r) == devauth::RecordStatus::Ok);
  // blank: nothing was ever written
  std::vector<uint8_t> blank(4096, 0xFF);
  CHECK(devauth::parseRecord(blank.data(), blank.size(), r) == devauth::RecordStatus::Blank);
  CHECK(devauth::parseRecord(blank.data(), 12, r) == devauth::RecordStatus::Blank);
  std::vector<uint8_t> zeros(4096, 0);
  CHECK(devauth::parseRecord(zeros.data(), zeros.size(), r) == devauth::RecordStatus::BadMagic);
  // bad magic: each byte
  for (int i = 0; i < 4; i++) {
    std::vector<uint8_t> s = good;
    s[i] ^= 0x01;
    CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::BadMagic);
    CHECK(recWiped(r));
  }
  // version
  for (int v : {0, 2, 255}) {
    std::vector<uint8_t> s = good;
    s[4] = (uint8_t)v;
    CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::BadVersion);
  }
  { std::vector<uint8_t> s = good; s[5] = 1; CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::BadVersion); }
  // payload length: not 48
  for (int l : {0, 1, 47, 49, 255, 0x130}) {
    std::vector<uint8_t> s = good;
    s[6] = (uint8_t)(l & 0xFF); s[7] = (uint8_t)(l >> 8);
    CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::BadLength);
  }
  { std::vector<uint8_t> s = good; s[6] = 0xFF; s[7] = 0xFF; CHECK(devauth::parseRecord(s.data(), 60, r) == devauth::RecordStatus::BadLength); }  // 65535: never read past the buffer
  // CRC: a wrong checksum, and a flipped bit in every payload byte
  { std::vector<uint8_t> s = makeSector(ID, 0, KEY_A, false); CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::BadCrc); CHECK(recWiped(r)); }
  for (int i = 8; i < 60; i++) {
    std::vector<uint8_t> s = good;
    s[i] ^= 0x10;
    devauth::RecordStatus st = devauth::parseRecord(s.data(), s.size(), r);
    CHECK(st == devauth::RecordStatus::BadCrc);
    CHECK(recWiped(r));
  }
  // a valid CRC over an ID that is not valid (upper case, colons, spaces): refused, and never half-copied
  for (const char *bad : {"AABBCCDDEEFF", "aa:bb:cc:dd:", "aabbccddeeg0", "            "}) {
    std::vector<uint8_t> s = makeSector(bad, 0, KEY_A);
    CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::BadId);
    CHECK(recWiped(r));
  }
  // a failed parse wipes a record that an earlier parse filled in
  CHECK(devauth::parseRecord(good.data(), good.size(), r) == devauth::RecordStatus::Ok && !recWiped(r));
  CHECK(devauth::parseRecord(blank.data(), blank.size(), r) == devauth::RecordStatus::Blank && recWiped(r));
  // the ID in the record is NUL-terminated by the parser
  CHECK(devauth::parseRecord(good.data(), good.size(), r) == devauth::RecordStatus::Ok && r.id[12] == 0);
}

static void testLoadIdentity() {
  SECTION("identity: the two-copy rule and the checks for this chip");
  using devauth::Identity;
  devauth::IdentRecord r;
  std::vector<uint8_t> a = makeSector(ID, 0, KEY_A), b = makeSector(ID, 0, KEY_B);
  std::vector<uint8_t> bad = a;
  bad[20] ^= 0xFF;  // damaged
  std::vector<uint8_t> blank(4096, 0xFF);
  auto load = [&](const std::vector<uint8_t> *s0, const std::vector<uint8_t> *s1, const char *chip, bool hw = false) {
    return devauth::loadIdentity(s0 ? s0->data() : nullptr, s0 ? s0->size() : 0, s1 ? s1->data() : nullptr, s1 ? s1->size() : 0, chip, hw, r);
  };
  // sector 0 first, sector 1 when sector 0 is not valid (the two keys differ here only to tell which one was used)
  CHECK(load(&a, &b, ID) == Identity::Ok && memcmp(r.key, KEY_A, 32) == 0);
  CHECK(load(&bad, &b, ID) == Identity::Ok && memcmp(r.key, KEY_B, 32) == 0);
  CHECK(load(&blank, &b, ID) == Identity::Ok && memcmp(r.key, KEY_B, 32) == 0);
  CHECK(load(nullptr, &b, ID) == Identity::Ok && memcmp(r.key, KEY_B, 32) == 0);
  CHECK(load(&a, &bad, ID) == Identity::Ok && memcmp(r.key, KEY_A, 32) == 0);
  CHECK(load(&a, nullptr, ID) == Identity::Ok);
  CHECK(strcmp(r.id, ID) == 0);
  // a damaged copy is not used even if its ID would match
  std::vector<uint8_t> badMagic = a;
  badMagic[0] = 'X';
  CHECK(load(&badMagic, &b, ID) == Identity::Ok && memcmp(r.key, KEY_B, 32) == 0);
  // both bad: invalid, and nothing of a key is left in `rec`
  CHECK(load(&bad, &bad, ID) == Identity::Invalid && recWiped(r));
  CHECK(load(&bad, &blank, ID) == Identity::Invalid);
  CHECK(load(&blank, &bad, ID) == Identity::Invalid);
  CHECK(load(&bad, nullptr, ID) == Identity::Invalid);
  CHECK(load(nullptr, &bad, ID) == Identity::Invalid);
  // a sector that could not be read (zero bytes handed over) is a damaged one, not a missing one
  CHECK(devauth::loadIdentity(a.data(), 0, a.data(), 0, ID, false, r) == Identity::Invalid && recWiped(r));
  CHECK(devauth::loadIdentity(a.data(), 0, b.data(), 60, ID, false, r) == Identity::Ok && memcmp(r.key, KEY_B, 32) == 0);
  // nothing was ever written: missing
  CHECK(load(&blank, &blank, ID) == Identity::Missing && recWiped(r));
  CHECK(load(nullptr, nullptr, ID) == Identity::Missing);
  CHECK(load(&blank, nullptr, ID) == Identity::Missing);
  // a valid record for another chip: refused, wiped. Sector 0 being valid, sector 1 is not looked at.
  CHECK(load(&a, &a, "112233445566") == Identity::OtherBoard && recWiped(r));
  std::vector<uint8_t> forOther = makeSector("112233445566", 0, KEY_A);
  CHECK(load(&forOther, &a, ID) == Identity::OtherBoard && recWiped(r));
  CHECK(load(&bad, &forOther, ID) == Identity::OtherBoard);
  CHECK(load(&bad, &forOther, "112233445566") == Identity::Ok);
  // the chip's own ID is not usable text: cannot tell, so not ok
  CHECK(load(&a, &a, "") == Identity::Invalid);
  CHECK(load(&a, &a, "AABBCCDDEEFF") == Identity::Invalid);
  CHECK(load(&a, &a, "aabbccddeef") == Identity::Invalid);
  CHECK(devauth::loadIdentity(a.data(), a.size(), a.data(), a.size(), nullptr, false, r) == Identity::Invalid);
  // a burned key needs the hardware signer
  std::vector<uint8_t> burned = makeSector(ID, 1, KEY_A);
  CHECK(load(&burned, &burned, ID, false) == Identity::HardwareKey && recWiped(r));
  CHECK(load(&burned, &burned, ID, true) == Identity::Ok && r.burned());
  CHECK(load(&a, &a, ID, true) == Identity::Ok && !r.burned());  // with a hardware signer present a software key still works
  CHECK(load(&burned, &burned, "112233445566", false) == Identity::OtherBoard);  // the wrong board is the first thing said
  // the names /status reports
  CHECK(strcmp(devauth::identityName(Identity::Ok), "ok") == 0);
  CHECK(strcmp(devauth::identityName(Identity::Missing), "missing") == 0);
  CHECK(strcmp(devauth::identityName(Identity::Invalid), "invalid") == 0);
  CHECK(strcmp(devauth::identityName(Identity::OtherBoard), "other_board") == 0);
  CHECK(strcmp(devauth::identityName(Identity::HardwareKey), "hardware_key") == 0);
  CHECK(strcmp(devauth::identityName(Identity::HardwareKeyUnprotected), "hardware_unprotected") == 0);
}

static void testHostTargetRules() {
  SECTION("host, target, method, timestamp: what may go into a request");
  char out[devauth::HOST_MAX + 1];
  CHECK(devauth::lowerHost("ledmatrix.example.com", out, sizeof out) && strcmp(out, "ledmatrix.example.com") == 0);
  CHECK(devauth::lowerHost("LedMatrix.EXAMPLE.com", out, sizeof out) && strcmp(out, "ledmatrix.example.com") == 0);
  CHECK(devauth::lowerHost("a", out, sizeof out) && strcmp(out, "a") == 0);
  CHECK(devauth::lowerHost("1.2.3.4", out, sizeof out) && strcmp(out, "1.2.3.4") == 0);
  CHECK(devauth::lowerHost("my-host.example.com", out, sizeof out));
  CHECK(!devauth::lowerHost("", out, sizeof out) && out[0] == 0);
  CHECK(!devauth::lowerHost(nullptr, out, sizeof out) && out[0] == 0);
  for (const char *bad : {"host:443", "ho st", "host/path", "host\r\nX-Evil: 1", "h\xC3\xA9llo", "host_name", "[::1]", "host.example.com\n"}) {
    CHECK(!devauth::lowerHost(bad, out, sizeof out));
    CHECK(out[0] == 0);  // nothing half-written is left behind
  }
  std::string h63(63, 'a'), h64(64, 'a');
  CHECK(devauth::lowerHost(h63.c_str(), out, sizeof out));
  CHECK(!devauth::lowerHost(h64.c_str(), out, sizeof out));
  // a buffer too small for the host and its NUL
  char tiny[8];
  CHECK(devauth::lowerHost("abcdefg", tiny, sizeof tiny) && strcmp(tiny, "abcdefg") == 0);
  CHECK(!devauth::lowerHost("abcdefgh", tiny, sizeof tiny) && tiny[0] == 0);
  CHECK(!devauth::lowerHost("a", tiny, 0));
  CHECK(!devauth::lowerHost("a", tiny, 1) && tiny[0] == 0);
  CHECK(devauth::lowerHost("a", tiny, 2));

  CHECK(devauth::validMethod("GET") && devauth::validMethod("POST") && devauth::validMethod("OPTIONS"));
  CHECK(!devauth::validMethod("") && !devauth::validMethod("get") && !devauth::validMethod("GE T") && !devauth::validMethod(nullptr));
  CHECK(!devauth::validMethod("ABCDEFGH") && devauth::validMethod("ABCDEFG"));

  CHECK(devauth::validTarget("/") && devauth::validTarget("/v1/flights?lat=38.7&lon=-9.1") && devauth::validTarget("/v1/headlines?n=3&q=a%20b"));
  CHECK(!devauth::validTarget("") && !devauth::validTarget(nullptr) && !devauth::validTarget("v1/x") && !devauth::validTarget("*"));
  CHECK(!devauth::validTarget("/a b") && !devauth::validTarget("/a\nb") && !devauth::validTarget("/a\rb") && !devauth::validTarget("/a\tb"));
  CHECK(!devauth::validTarget("/a\x7F") && !devauth::validTarget("/\xC3\xA9"));
  std::string t255 = "/" + std::string(254, 'x'), t256 = "/" + std::string(255, 'x');
  CHECK(devauth::validTarget(t255.c_str()) && !devauth::validTarget(t256.c_str()));

  CHECK(devauth::validBodyHash(devauth::EMPTY_BODY_SHA256_HEX));
  CHECK(!devauth::validBodyHash("") && !devauth::validBodyHash(nullptr) && !devauth::validBodyHash("abc"));
  CHECK(!devauth::validBodyHash("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
  CHECK(!devauth::validBodyHash("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b8550"));
  CHECK(!devauth::validBodyHash("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b85g"));

  char ts[21];
  CHECK(devauth::formatUnsigned(0, ts) == 1 && strcmp(ts, "0") == 0);
  CHECK(devauth::formatUnsigned(1796000000ull, ts) == 10 && strcmp(ts, "1796000000") == 0);
  CHECK(devauth::formatUnsigned(18446744073709551615ull, ts) == 20 && strcmp(ts, "18446744073709551615") == 0);
}

static void testVectorRequests() {
  SECTION("the vector requests: canonical string, body hash, signature, headers");
  std::vector<uint8_t> key = unhex(vec["device_key_hex"].as<const char *>());
  HostSigner host(key);
  std::string id = vec["device_id"].as<const char *>();
  int n = 0;
  for (JsonVariantConst rq : vec["requests"].as<JsonArrayConst>()) {
    n++;
    std::string name = rq["name"].as<const char *>();
    std::string method = rq["method"].as<const char *>(), hostName = rq["host"].as<const char *>(), uri = rq["uri"].as<const char *>();
    std::string body = rq["body"].as<const char *>();
    uint64_t ts = rq["timestamp"].as<uint64_t>();
    // the body hash in the vector is the real SHA-256 of the body
    uint8_t bh[32];
    sha256((const uint8_t *)body.data(), body.size(), bh);
    CHECK(hex(bh, 32) == rq["body_sha256"].as<const char *>());
    std::string bodyHash = rq["body_sha256"].as<const char *>();
    // the canonical string
    char canon[devauth::CANONICAL_CAP];
    size_t len = 0;
    CHECK(devauth::buildCanonical(canon, sizeof canon, len, method.c_str(), hostName.c_str(), uri.c_str(), ts, bodyHash.c_str()));
    std::string want = rq["canonical"].as<const char *>();
    CHECK(std::string(canon, len) == want);
    CHECK(len == want.size() && canon[len] == 0);
    if (std::string(canon, len) != want) printf("    request \"%s\"\n", name.c_str());
    // six lines, no trailing newline
    int lines = 1;
    for (size_t i = 0; i < len; i++) if (canon[i] == '\n') lines++;
    CHECK(lines == 6 && canon[len - 1] != '\n');
    // the signature over it
    CHECK(hmacHex(key, want) == rq["signature"].as<const char *>());
    uint8_t sig[32];
    CHECK(host.sign((const uint8_t *)canon, len, sig) && hex(sig, 32) == rq["signature"].as<const char *>());
    // the whole thing through signRequest and a Signer, and the headers it writes
    CountingSigner counting(host);
    char hdr[devauth::HEADERS_CAP];
    size_t hlen = 0;
    CHECK(devauth::signRequest(counting, id.c_str(), method.c_str(), hostName.c_str(), uri.c_str(), ts, bodyHash.c_str(), hdr, sizeof hdr, hlen));
    CHECK(counting.calls == 1 && counting.lastLen == want.size());
    std::string wantHdr = "X-Device: " + id + "\r\nX-Timestamp: " + std::to_string(ts) + "\r\nX-Signature: v1=" + rq["signature"].as<const char *>() + "\r\n";
    CHECK(std::string(hdr, hlen) == wantHdr);
    CHECK(hlen == strlen(hdr));
    CHECK(wantHdr.find("v1=") != std::string::npos);
  }
  CHECK(n == 4);
  // the board's own case: GET, empty body, the constant hash
  CHECK(std::string(devauth::EMPTY_BODY_SHA256_HEX) == vec["requests"][0]["body_sha256"].as<const char *>());
}

static void testLengthTable() {
  SECTION("the length table: the signer under every message length that matters for the hardware unit");
  std::vector<uint8_t> key = unhex(vec["device_key_hex"].as<const char *>());
  HostSigner host(key);
  CountingSigner signer(host);  // used only through the interface
  devauth::Signer &s = signer;
  int rows = 0;
  for (JsonVariantConst row : vec["length_table"]["rows"].as<JsonArrayConst>()) {
    size_t len = row["length"].as<size_t>();
    std::vector<uint8_t> msg(len);
    for (size_t i = 0; i < len; i++) msg[i] = (uint8_t)((i * 7 + 3) & 0xff);
    uint8_t out[32];
    CHECK(s.sign(msg.data(), len, out));
    CHECK(hex(out, 32) == row["hmac"].as<const char *>());
    // the same message at an unaligned address gives the same answer (a hardware signer has to copy; this one need not)
    std::vector<uint8_t> shifted(len + 3);
    memcpy(shifted.data() + 1, msg.data(), len);
    uint8_t out2[32];
    CHECK(s.sign(shifted.data() + 1, len, out2) && memcmp(out, out2, 32) == 0);
    rows++;
  }
  CHECK(rows == 11 && signer.calls == 22);
}

static void testCapacity() {
  SECTION("capacity: nothing is written past the cap, and what does not fit is refused");
  std::vector<uint8_t> key = unhex(vec["device_key_hex"].as<const char *>());
  const char *host = "ledmatrix.example.com", *uri = "/v1/flights?lat=38.7&lon=-9.1";
  uint64_t ts = 1796000000;
  char want[devauth::CANONICAL_CAP];
  size_t wlen = 0;
  CHECK(devauth::buildCanonical(want, sizeof want, wlen, "GET", host, uri, ts, devauth::EMPTY_BODY_SHA256_HEX));
  // every cap from 0 to the exact length (which has no room for the NUL) is refused; the exact length plus one fits
  for (size_t cap = 0; cap <= wlen + 2; cap++) {
    Guard<devauth::CANONICAL_CAP + 4> g;
    size_t len = 99;
    bool ok = devauth::buildCanonical(g.buf, cap, len, "GET", host, uri, ts, devauth::EMPTY_BODY_SHA256_HEX);
    CHECK(g.guardsOk());
    for (size_t i = cap; i < sizeof g.buf; i++) if (g.buf[i] != 'Z') { CHECK(false); break; }  // nothing past the cap
    if (cap >= wlen + 1) {
      CHECK(ok && len == wlen && memcmp(g.buf, want, wlen + 1) == 0);
    } else {
      CHECK(!ok && len == 0);
      if (cap) CHECK(g.buf[0] == 0);
    }
  }
  // the same for the header lines
  uint8_t sig[32];
  memcpy(sig, key.data(), 32);
  char whdr[devauth::HEADERS_CAP];
  size_t hlen = 0;
  CHECK(devauth::buildHeaders(whdr, sizeof whdr, hlen, ID, ts, sig));
  CHECK(hlen == strlen(whdr) && hlen < devauth::HEADERS_CAP);
  for (size_t cap = 0; cap <= hlen + 2; cap++) {
    Guard<devauth::HEADERS_CAP + 4> g;
    size_t len = 99;
    bool ok = devauth::buildHeaders(g.buf, cap, len, ID, ts, sig);
    CHECK(g.guardsOk());
    for (size_t i = cap; i < sizeof g.buf; i++) if (g.buf[i] != 'Z') { CHECK(false); break; }
    if (cap >= hlen + 1) CHECK(ok && len == hlen && strcmp(g.buf, whdr) == 0);
    else { CHECK(!ok && len == 0); if (cap) CHECK(g.buf[0] == 0); }
  }
  // signRequest into a too small buffer: refused, empty
  HostSigner host_(key);
  for (size_t cap : {(size_t)0, (size_t)1, (size_t)24, (size_t)60, hlen}) {  // hlen: room for the text but not for the NUL
    Guard<devauth::HEADERS_CAP + 4> g;
    size_t len = 99;
    bool ok = devauth::signRequest(host_, ID, "GET", host, uri, ts, devauth::EMPTY_BODY_SHA256_HEX, g.buf, cap, len);
    CHECK(!ok && len == 0 && g.guardsOk());
    if (cap) CHECK(g.buf[0] == 0);
  }
  {
    Guard<devauth::HEADERS_CAP + 4> g;
    size_t len = 0;
    CHECK(devauth::signRequest(host_, ID, "GET", host, uri, ts, devauth::EMPTY_BODY_SHA256_HEX, g.buf, hlen + 1, len) && len == hlen && g.guardsOk());
  }
  // the largest request the rules allow still fits the buffers the firmware uses
  std::string h63(63, 'h'), t255 = "/" + std::string(254, 't');
  char big[devauth::CANONICAL_CAP];
  size_t blen = 0;
  CHECK(devauth::buildCanonical(big, sizeof big, blen, "OPTIONS", h63.c_str(), t255.c_str(), 999999999999ull, devauth::EMPTY_BODY_SHA256_HEX));
  CHECK(blen + 1 <= devauth::CANONICAL_CAP);
  char bighdr[devauth::HEADERS_CAP];
  size_t bhlen = 0;
  CHECK(devauth::signRequest(host_, "ffffffffffff", "OPTIONS", h63.c_str(), t255.c_str(), 999999999999ull, devauth::EMPTY_BODY_SHA256_HEX, bighdr, sizeof bighdr, bhlen));
  CHECK(bhlen + 1 <= devauth::HEADERS_CAP);
  // a signer that fails: no headers, and the failure is reported
  FailingSigner fail;
  char hdr[devauth::HEADERS_CAP] = "stale";
  size_t flen = 5;
  CHECK(!devauth::signRequest(fail, ID, "GET", host, uri, ts, devauth::EMPTY_BODY_SHA256_HEX, hdr, sizeof hdr, flen) && flen == 0 && hdr[0] == 0);
}

static void testInvalidInputs() {
  SECTION("canonical string and headers: invalid inputs are refused");
  char out[devauth::CANONICAL_CAP];
  size_t len = 0;
  const char *H = devauth::EMPTY_BODY_SHA256_HEX;
  CHECK(devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x", 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "get", "h.example.com", "/x", 1796000000, H) && out[0] == 0 && len == 0);
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "", "h.example.com", "/x", 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, nullptr, "h.example.com", "/x", 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "H.example.com", "/x", 1796000000, H));      // the host must already be lower case
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com:443", "/x", 1796000000, H));  // no port
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "", "/x", 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", nullptr, "/x", 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "x", 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x\n/y", 1796000000, H));  // a newline would blur the lines
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", nullptr, 1796000000, H));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x", 1796000000, "abc"));
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x", 1796000000, nullptr));
  CHECK(devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x", 999999999999ull, H));      // 12 digits
  CHECK(!devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x", 1000000000000ull, H));    // 13 digits
  CHECK(devauth::buildCanonical(out, sizeof out, len, "GET", "h.example.com", "/x", 0, H) && strstr(out, "\n0\n"));
  CHECK(!devauth::buildCanonical(nullptr, 0, len, "GET", "h.example.com", "/x", 1796000000, H));
  // headers
  uint8_t sig[32] = {0};
  char hdr[devauth::HEADERS_CAP];
  CHECK(devauth::buildHeaders(hdr, sizeof hdr, len, ID, 1796000000, sig));
  CHECK(strstr(hdr, "X-Signature: v1=0000000000000000000000000000000000000000000000000000000000000000\r\n"));
  CHECK(!devauth::buildHeaders(hdr, sizeof hdr, len, "AABBCCDDEEFF", 1796000000, sig) && hdr[0] == 0);
  CHECK(!devauth::buildHeaders(hdr, sizeof hdr, len, "", 1796000000, sig));
  CHECK(!devauth::buildHeaders(hdr, sizeof hdr, len, nullptr, 1796000000, sig));
  CHECK(!devauth::buildHeaders(hdr, sizeof hdr, len, ID, 1000000000000ull, sig));
  CHECK(!devauth::buildHeaders(hdr, sizeof hdr, len, ID, 1796000000, nullptr));
  CHECK(!devauth::buildHeaders(nullptr, 0, len, ID, 1796000000, sig));
  // the signature goes out as 64 lowercase hex characters
  uint8_t all[32];
  for (int i = 0; i < 32; i++) all[i] = (uint8_t)(0xA0 + i);
  CHECK(devauth::buildHeaders(hdr, sizeof hdr, len, ID, 1, all));
  CHECK(strstr(hdr, "v1=a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf\r\n"));
  // a signer is not asked to sign something that was refused
  HostSigner ones(std::vector<uint8_t>(32, 1));
  CountingSigner none(ones);
  char h2[devauth::HEADERS_CAP];
  size_t l2 = 0;
  CHECK(!devauth::signRequest(none, ID, "GET", "H.example.com", "/x", 1, H, h2, sizeof h2, l2) && none.calls == 0);
  CHECK(!devauth::signRequest(none, "bad", "GET", "h.example.com", "/x", 1, H, h2, sizeof h2, l2) && h2[0] == 0 && l2 == 0);
  // a refused canonical string never gets as far as the signature, but a refused header does (the ID is checked last):
  // either way nothing is returned
}

static void testClockOffset() {
  SECTION("clock offset: store, apply, clear, and the system clock being set");
  devauth::ClockOffset c;
  CHECK(!c.have && c.apply(1000, 5) == 1000);  // none stored: the own clock
  // the board's clock reads 100 (before NTP), the service says 1796000123
  c.store(1796000123, 100, 50);
  CHECK(c.have && c.offset == 1796000023);
  CHECK(c.apply(100, 50) == 1796000123);
  CHECK(c.apply(160, 110) == 1796000183);       // a minute later on both clocks
  CHECK(c.apply(161, 110) == 1796000184);       // rounding: one second more on the system clock
  CHECK(c.apply(158, 110) == 1796000181);       // or one less
  CHECK(c.have);
  // NTP sets the system clock: it jumps, the uptime does not, so the offset is dropped
  CHECK(c.apply(1796000200, 150) == 1796000200 && !c.have);
  CHECK(c.apply(1796000210, 160) == 1796000210);  // and stays dropped
  // a jump the other way (clock set back) is a jump too
  c.store(1000, 2000, 10);
  CHECK(c.apply(1500, 20) == 1500 && !c.have);
  // the limit: 5 seconds of difference is rounding, 6 is a jump
  c.store(5000, 1000, 100);
  CHECK(c.apply(1100 + 5, 200) == 5000 + 100 + 5);
  CHECK(c.have);
  c.store(5000, 1000, 100);
  CHECK(c.apply(1100 + 6, 200) == 1106 && !c.have);
  c.store(5000, 1000, 100);
  CHECK(c.apply(1100 - 6, 200) == 1094 && !c.have);
  // explicit clear
  c.store(5000, 1000, 100);
  c.clear();
  CHECK(!c.have && c.offset == 0 && c.apply(1234, 300) == 1234);
  // a service clock behind the board's: a negative offset
  c.store(900, 1000, 100);
  CHECK(c.offset == -100 && c.apply(1010, 110) == 910);
  // the uptime is a 64-bit count (esp_timer_get_time() / 1000000) that never wraps: values past 2^32 seconds, and past what a
  // 32-bit millis() / 1000 could hold (4294967 s, 49 days), give the same difference
  c.store(5000, 1000, 5000000000LL);
  CHECK(c.apply(1005, 5000000005LL) == 5000 + 5);
  CHECK(c.have && c.apply(1000 + 3600, 5000000000LL + 3600) == 5000 + 3600);
  c.store(5000, 1000, 4294967LL);  // the second where a millis() / 1000 would have wrapped: no step at all
  CHECK(c.apply(1000 + 10, 4294967LL + 10) == 5010 && c.have);
  CHECK(c.apply(1000 + 20, 4294967LL + 20) == 5020 && c.have);
  // storing again replaces the offset and the reference point
  c.store(7000, 2000, 500);
  CHECK(c.apply(2010, 510) == 7010);
}

// A signer that gives the wrong answer for one message length (to prove that every length in the verification is compared).
struct WrongAtLength : devauth::Signer {
  devauth::Signer &inner;
  size_t badLen;
  WrongAtLength(devauth::Signer &s, size_t l) : inner(s), badLen(l) {}
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override {
    bool ok = inner.sign(msg, len, out);
    if (len == badLen) out[31] ^= 1;
    return ok;
  }
};
struct FailsAtLength : devauth::Signer {  // reports failure for one length
  devauth::Signer &inner;
  size_t badLen;
  FailsAtLength(devauth::Signer &s, size_t l) : inner(s), badLen(l) {}
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override { return len != badLen && inner.sign(msg, len, out); }
};
struct SucceedsWithoutWriting : devauth::Signer {  // claims success and leaves the output alone
  bool sign(const uint8_t *, size_t, uint8_t[devauth::SIG_LEN]) override { return true; }
};

static void testSignerChoice() {
  SECTION("signer choice: software, hardware, or none; and the check between the two");
  using devauth::Identity;
  using devauth::SignerCheck;
  using devauth::SignerKind;
  // The truth table, written out by hand: every combination of (record burned, key in the chip, key fully protected, answers equal).
  // `protected` and `matches` only count when they can: protected only with a key; matches only when the comparison is run.
  struct Row { bool burned, key, prot, match; SignerKind kind; SignerCheck check; Identity problem; };
  static const Row rows[] = {
      // flag clear: today's software signer unless a complete, matching key is in the chip
      {false, false, false, false, SignerKind::Software, SignerCheck::NotRun, Identity::Ok},
      {false, false, false, true, SignerKind::Software, SignerCheck::NotRun, Identity::Ok},
      {false, false, true, false, SignerKind::Software, SignerCheck::NotRun, Identity::Ok},  // protection without a key means nothing
      {false, false, true, true, SignerKind::Software, SignerCheck::NotRun, Identity::Ok},
      {false, true, false, false, SignerKind::Software, SignerCheck::Unprotected, Identity::Ok},  // a key that is not fully protected is never used
      {false, true, false, true, SignerKind::Software, SignerCheck::Unprotected, Identity::Ok},   // not even when its answers match
      {false, true, true, false, SignerKind::Software, SignerCheck::Mismatch, Identity::Ok},
      {false, true, true, true, SignerKind::Hardware, SignerCheck::Match, Identity::Ok},
      // flag set: the chip's key, fully protected, or nothing (there is no software key to fall back on)
      {true, false, false, false, SignerKind::None, SignerCheck::NotRun, Identity::HardwareKey},
      {true, false, false, true, SignerKind::None, SignerCheck::NotRun, Identity::HardwareKey},
      {true, false, true, false, SignerKind::None, SignerCheck::NotRun, Identity::HardwareKey},
      {true, false, true, true, SignerKind::None, SignerCheck::NotRun, Identity::HardwareKey},
      {true, true, false, false, SignerKind::None, SignerCheck::Unprotected, Identity::HardwareKeyUnprotected},
      {true, true, false, true, SignerKind::None, SignerCheck::Unprotected, Identity::HardwareKeyUnprotected},
      {true, true, true, false, SignerKind::Hardware, SignerCheck::NotRun, Identity::Ok},  // the comparison is not asked for: the software key is gone
      {true, true, true, true, SignerKind::Hardware, SignerCheck::NotRun, Identity::Ok},
  };
  CHECK(sizeof rows / sizeof rows[0] == 16);  // all of them
  int hardware = 0;
  for (const Row &r : rows) {
    devauth::SignerPlan p = devauth::chooseSigner(r.burned, r.key, r.prot, r.match);
    bool same = p.kind == r.kind && p.check == r.check && p.problem == r.problem;
    checks++;
    if (!same) { fails++; printf("    FAIL: burned %d key %d protected %d matches %d -> kind %d check %d problem %d\n", r.burned, r.key, r.prot, r.match, (int)p.kind, (int)p.check, (int)p.problem); }
    if (p.kind == SignerKind::Hardware) hardware++;
    // the hardware is used only with a key that is there and fully protected, and after a mismatch or an unprotected key never
    if (p.kind == SignerKind::Hardware) CHECK(r.key && r.prot);
    if (p.check == SignerCheck::Mismatch || p.check == SignerCheck::Unprotected) CHECK(p.kind != SignerKind::Hardware);
    // a problem exists exactly when there is no signer
    CHECK((p.kind == SignerKind::None) == (p.problem != Identity::Ok));
  }
  CHECK(hardware == 3);
  // the comparison is run only when both keys could be used: no burned flag, a key, and all its protection
  for (int b = 0; b < 2; b++)
    for (int k = 0; k < 2; k++)
      for (int pr = 0; pr < 2; pr++) CHECK(devauth::needsSignerCheck(b, k, pr) == (!b && k && pr));
  CHECK(devauth::needsSignerCheck(false, true, true));
  // names for /status
  CHECK(strcmp(devauth::signerKindName(SignerKind::Software), "software") == 0);
  CHECK(strcmp(devauth::signerKindName(SignerKind::Hardware), "hardware") == 0);
  CHECK(strcmp(devauth::signerKindName(SignerKind::None), "none") == 0);
  CHECK(strcmp(devauth::signerCheckName(SignerCheck::NotRun), "not_run") == 0);
  CHECK(strcmp(devauth::signerCheckName(SignerCheck::Match), "match") == 0);
  CHECK(strcmp(devauth::signerCheckName(SignerCheck::Mismatch), "mismatch") == 0);
  CHECK(strcmp(devauth::signerCheckName(SignerCheck::Unprotected), "unprotected") == 0);
  CHECK(strcmp(devauth::identityName(Identity::HardwareKeyUnprotected), "hardware_unprotected") == 0);
}

static void testSignersAgree() {
  SECTION("signers agree: the same HMAC for every length of the length table");
  std::vector<uint8_t> key = unhex(vec["device_key_hex"].as<const char *>());
  // the lengths the board checks are exactly the vectors' length table, in order
  std::vector<size_t> table;
  for (JsonVariantConst row : vec["length_table"]["rows"].as<JsonArrayConst>()) table.push_back(row["length"].as<size_t>());
  CHECK(table.size() == devauth::VERIFY_COUNT);
  for (size_t i = 0; i < table.size() && i < devauth::VERIFY_COUNT; i++) CHECK(devauth::VERIFY_LENGTHS[i] == table[i]);
  CHECK(devauth::VERIFY_MAX == 200 && devauth::VERIFY_MAX == *std::max_element(table.begin(), table.end()));
  // the verification message is the table's message: signing it under the vector key gives the vector's answers
  HostSigner host(key);
  size_t row = 0;
  for (JsonVariantConst r : vec["length_table"]["rows"].as<JsonArrayConst>()) {
    uint8_t msg[devauth::VERIFY_MAX], out[32];
    devauth::verifyMessage(msg, devauth::VERIFY_LENGTHS[row]);
    CHECK(host.sign(msg, devauth::VERIFY_LENGTHS[row], out) && hex(out, 32) == r["hmac"].as<const char *>());
    row++;
  }
  // two signers with the same key agree; every call is made for every length, on both
  HostSigner a(key), b(key);
  CountingSigner ca(a), cb(b);
  CHECK(devauth::signersAgree(ca, cb));
  CHECK(ca.calls == (int)devauth::VERIFY_COUNT && cb.calls == (int)devauth::VERIFY_COUNT);
  // another key: they do not
  std::vector<uint8_t> other = key;
  other[0] ^= 1;
  HostSigner c(other);
  CHECK(!devauth::signersAgree(a, c));
  CHECK(!devauth::signersAgree(c, a));
  // one wrong answer, at any one length, is enough (either signer)
  for (size_t len : table) {
    WrongAtLength w(b, len);
    CHECK(!devauth::signersAgree(a, w));
    CHECK(!devauth::signersAgree(w, a));
    FailsAtLength f(b, len);
    CHECK(!devauth::signersAgree(a, f));
    CHECK(!devauth::signersAgree(f, a));
  }
  // a length outside the table is not compared
  WrongAtLength notChecked(b, 7);
  CHECK(devauth::signersAgree(a, notChecked));
  // a signer that fails outright, and two that claim success without writing anything
  FailingSigner fail;
  CHECK(!devauth::signersAgree(a, fail) && !devauth::signersAgree(fail, a) && !devauth::signersAgree(fail, fail));
  SucceedsWithoutWriting n1, n2;
  CHECK(!devauth::signersAgree(n1, n2));
  CHECK(!devauth::signersAgree(a, n1) && !devauth::signersAgree(n1, a));
  // the chooser fed with the result: the whole decision for a board with the burn done
  CHECK(devauth::chooseSigner(false, true, true, devauth::signersAgree(a, b)).kind == devauth::SignerKind::Hardware);
  CHECK(devauth::chooseSigner(false, true, true, devauth::signersAgree(a, c)).kind == devauth::SignerKind::Software);
  CHECK(devauth::chooseSigner(false, true, true, devauth::signersAgree(a, c)).check == devauth::SignerCheck::Mismatch);
  // an unprotected key is not even compared: the caller never runs signersAgree (needsSignerCheck is false), and the answer is software
  CHECK(!devauth::needsSignerCheck(false, true, false));
  CHECK(devauth::chooseSigner(false, true, false, devauth::signersAgree(a, b)).kind == devauth::SignerKind::Software);
}

static void testRetryDelay() {
  SECTION("retry delay: a refused board waits longer, never less than the normal retry");
  const uint32_t MIN = 60UL * 1000;
  CHECK(devauth::AUTH_RETRY_MS == 5 * MIN);
  // the normal retries in live.cpp: flights 15 s, weather 60 s, headlines 5 minutes
  CHECK(devauth::retryDelayMs(true, 15000) == 5 * MIN);
  CHECK(devauth::retryDelayMs(true, 60000) == 5 * MIN);
  CHECK(devauth::retryDelayMs(true, 5 * MIN) == 5 * MIN);
  // not refused: the normal retry, unchanged
  CHECK(devauth::retryDelayMs(false, 15000) == 15000);
  CHECK(devauth::retryDelayMs(false, 60000) == 60000);
  CHECK(devauth::retryDelayMs(false, 5 * MIN) == 5 * MIN);
  CHECK(devauth::retryDelayMs(false, 0) == 0);
  // never faster than the normal schedule, even when that is longer than the refused one
  CHECK(devauth::retryDelayMs(true, 10 * MIN) == 10 * MIN);
  CHECK(devauth::retryDelayMs(true, 5 * MIN + 1) == 5 * MIN + 1);
  CHECK(devauth::retryDelayMs(true, 4294967295u) == 4294967295u);
  for (uint32_t normal : {0u, 1u, 15000u, 60000u, 5 * MIN - 1, 5 * MIN, 6 * MIN, 30 * MIN})
    for (bool refused : {false, true}) CHECK(devauth::retryDelayMs(refused, normal) >= normal);
  // it stays inside the problem list's "still failing" window (10 minutes), so the one-hour message can still appear
  CHECK(devauth::AUTH_RETRY_MS <= 10 * MIN);
}

static devauth::Refusal parse(const std::string &body, bool *ok = nullptr) {
  devauth::Refusal r;
  bool k = devauth::parseRefusal(body.c_str(), body.size(), r);
  if (ok) *ok = k;
  return r;
}

static void testRefusal() {
  SECTION("refusal body: reason, server time, garbage");
  bool ok;
  devauth::Refusal r;
  // the examples from the contract
  r = parse("{\"error\":\"The request is signed with the wrong key.\",\"reason\":\"bad_signature\"}", &ok);
  CHECK(ok && r.reason == devauth::Reason::BadSignature && !r.hasServerTime);
  r = parse("{\"error\":\"The board's clock is more than 2 minutes off.\",\"reason\":\"clock_skew\",\"serverTime\":1796000123}", &ok);
  CHECK(ok && r.reason == devauth::Reason::ClockSkew && r.hasServerTime && r.serverTime == 1796000123);
  // each reason
  struct { const char *code; devauth::Reason why; } all[] = {{"missing", devauth::Reason::Missing}, {"malformed", devauth::Reason::Malformed},
      {"bad_signature", devauth::Reason::BadSignature}, {"revoked", devauth::Reason::Revoked}, {"clock_skew", devauth::Reason::ClockSkew}};
  for (auto &a : all) {
    r = parse(std::string("{\"error\":\"x\",\"reason\":\"") + a.code + "\"}", &ok);
    CHECK(ok && r.reason == a.why);
    CHECK(strcmp(devauth::reasonName(a.why), a.code) == 0);
    r = parse(std::string("{\"reason\":\"") + a.code + "\"}", &ok);  // the sentence is not needed
    CHECK(ok && r.reason == a.why);
    r = parse(std::string("{\"reason\":\"") + a.code + "\",\"error\":\"x\",\"extra\":[1,2,{\"a\":null}]}", &ok);  // other fields are ignored
    CHECK(ok && r.reason == a.why);
  }
  CHECK(strcmp(devauth::reasonName(devauth::Reason::Unknown), "unknown") == 0);
  // a code this firmware does not know: a refusal, reason Unknown
  r = parse("{\"reason\":\"rate_limited\"}", &ok);
  CHECK(ok && r.reason == devauth::Reason::Unknown && !r.hasServerTime);
  r = parse("{\"reason\":\"\"}", &ok);
  CHECK(ok && r.reason == devauth::Reason::Unknown);
  r = parse("{\"reason\":\"BAD_SIGNATURE\"}", &ok);  // codes are exact
  CHECK(ok && r.reason == devauth::Reason::Unknown);
  r = parse("{\"reason\":\"bad_signature \"}", &ok);
  CHECK(ok && r.reason == devauth::Reason::Unknown);
  // the server time: only for clock_skew, only a plausible whole number of seconds
  r = parse("{\"reason\":\"clock_skew\",\"serverTime\":1796000123}", &ok);
  CHECK(ok && r.hasServerTime && r.serverTime == 1796000123);
  r = parse("{\"reason\":\"clock_skew\",\"serverTime\":4102444800}", &ok);
  CHECK(ok && r.hasServerTime);
  r = parse("{\"reason\":\"clock_skew\",\"serverTime\":4102444801}", &ok);
  CHECK(ok && !r.hasServerTime && r.reason == devauth::Reason::ClockSkew);
  r = parse("{\"reason\":\"clock_skew\",\"serverTime\":1700000000}", &ok);
  CHECK(ok && r.hasServerTime);
  r = parse("{\"reason\":\"clock_skew\",\"serverTime\":1699999999}", &ok);
  CHECK(ok && !r.hasServerTime);
  for (const char *bad : {"0", "-1", "-1796000123", "1796000123.5", "1.796e9", "\"1796000123\"", "null", "true", "[]", "{}", "99999999999", "4294967296", "18446744073709551615"}) {
    r = parse(std::string("{\"reason\":\"clock_skew\",\"serverTime\":") + bad + "}", &ok);
    CHECK(ok && r.reason == devauth::Reason::ClockSkew && !r.hasServerTime && r.serverTime == 0);
  }
  r = parse("{\"reason\":\"clock_skew\"}", &ok);  // no time with the refusal: no retry is possible
  CHECK(ok && r.reason == devauth::Reason::ClockSkew && !r.hasServerTime);
  // a server time that comes with any other reason is never used
  for (const char *code : {"missing", "malformed", "bad_signature", "revoked", "other"}) {
    r = parse(std::string("{\"reason\":\"") + code + "\",\"serverTime\":1796000123}", &ok);
    CHECK(ok && !r.hasServerTime && r.serverTime == 0);
  }
  // not a refusal this board can use: false, and the result is reset
  for (const char *bad : {"", " ", "garbage", "<html><body>401</body></html>", "{", "}", "{}", "[]", "[{\"reason\":\"revoked\"}]", "\"revoked\"", "null", "42",
                          "{\"error\":\"nothing else\"}", "{\"reason\":null}", "{\"reason\":5}", "{\"reason\":true}", "{\"reason\":[\"revoked\"]}",
                          "{\"reason\":{\"a\":1}}", "{\"reason\":\"revoked\"", "{\"reason\":\"revoked\",}", "{\"reason\":\"rev",
                          "Unauthorized"}) {
    r = devauth::Refusal();
    r.reason = devauth::Reason::Revoked;  // junk that must be cleared
    r.hasServerTime = true;
    r.serverTime = 5;
    bool k = devauth::parseRefusal(bad, strlen(bad), r);
    CHECK(!k && r.reason == devauth::Reason::Unknown && !r.hasServerTime && r.serverTime == 0);
  }
  { devauth::Refusal x; x.reason = devauth::Reason::Revoked; CHECK(!devauth::parseRefusal(nullptr, 10, x) && x.reason == devauth::Reason::Unknown); }
  { devauth::Refusal x; CHECK(!devauth::parseRefusal("{\"reason\":\"revoked\"}", 0, x)); }
  // only `len` bytes are read: a body longer than the length given is cut, and the text after it is never looked at
  { devauth::Refusal x; const char *b = "{\"reason\":\"revoked\"}{\"reason\":\"missing\"}"; CHECK(devauth::parseRefusal(b, 20, x) && x.reason == devauth::Reason::Revoked); }
  { devauth::Refusal x; const char *b = "{\"reason\":\"revoked\"}"; CHECK(!devauth::parseRefusal(b, 19, x)); }  // cut inside the object
  // the size limit and the nesting limit
  std::string pad = "{\"reason\":\"revoked\",\"error\":\"" + std::string(devauth::REFUSAL_BODY_MAX - 40, 'x') + "\"}";
  CHECK(pad.size() <= devauth::REFUSAL_BODY_MAX);
  r = parse(pad, &ok);
  CHECK(ok && r.reason == devauth::Reason::Revoked);
  std::string too = "{\"reason\":\"revoked\",\"error\":\"" + std::string(devauth::REFUSAL_BODY_MAX, 'x') + "\"}";
  r = parse(too, &ok);
  CHECK(!ok && r.reason == devauth::Reason::Unknown);
  std::string deep = "{\"reason\":\"revoked\",\"x\":";
  for (int i = 0; i < 50; i++) deep += "[";
  for (int i = 0; i < 50; i++) deep += "]";
  deep += "}";
  r = parse(deep, &ok);
  CHECK(!ok);
  // escapes in the sentence are fine; an escaped code is read as its text
  r = parse("{\"error\":\"it\\u0027s \\\"quoted\\\"\\n\",\"reason\":\"re\\u0076oked\"}", &ok);
  CHECK(ok && r.reason == devauth::Reason::Revoked);
  // whitespace around the JSON is fine
  r = parse("\r\n {\"reason\" : \"missing\"} \r\n", &ok);
  CHECK(ok && r.reason == devauth::Reason::Missing);
}

static void testNoSecretsLeft() {
  SECTION("the key does not outlive the call");
  // signRequest wipes its signature buffer; the record wipes its key; neither text it builds holds the key
  std::vector<uint8_t> key = unhex(vec["device_key_hex"].as<const char *>());
  HostSigner signer(key);
  char hdr[devauth::HEADERS_CAP];
  size_t len = 0;
  CHECK(devauth::signRequest(signer, ID, "GET", "ledmatrix.example.com", "/v1/logo/ZZ", 1796000120, devauth::EMPTY_BODY_SHA256_HEX, hdr, sizeof hdr, len));
  std::string h(hdr, len), k = hex(key.data(), 32);
  CHECK(h.find(k) == std::string::npos);
  CHECK(h.find(std::string((const char *)key.data(), 32)) == std::string::npos);
  devauth::IdentRecord r;
  std::vector<uint8_t> s = makeSector(ID, 0, key.data());
  CHECK(devauth::parseRecord(s.data(), s.size(), r) == devauth::RecordStatus::Ok && memcmp(r.key, key.data(), 32) == 0);
  r.wipe();
  CHECK(recWiped(r));
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "shared/device-auth-vectors.json";
  if (!loadVectors(path)) {
    printf("cannot read the vectors file \"%s\" (pass its path as the one argument)\n", path);
    return 2;
  }
  for (int i = 0; i < 32; i++) { KEY_A[i] = (uint8_t)(0x10 + i); KEY_B[i] = (uint8_t)(0xE0 - i); }
  testPrimitives();
  testVectorKey();
  testDeviceId();
  testRecordParse();
  testLoadIdentity();
  testHostTargetRules();
  testVectorRequests();
  testLengthTable();
  testCapacity();
  testInvalidInputs();
  testClockOffset();
  testSignerChoice();
  testSignersAgree();
  testRetryDelay();
  testRefusal();
  testNoSecretsLeft();
  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
