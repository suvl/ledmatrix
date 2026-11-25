// Device authentication, scheme v1 (tickets 37 and 39): the board proves to the owner's service that it is the owner's device by
// signing every request. The contract is shared/device-auth.md; the service (Go), tools/provision.py and this file must agree
// byte for byte, and tools/test_deviceauth.cpp checks this file against the shared known-answer vectors.
//
// Pure logic: no Arduino or ESP headers (ArduinoJson only parses the service's refusal), so it is tested on the laptop.
// The board's own code is identity.cpp (reads the record from flash, owns the software signer) and live.cpp (sends the request).
//
// What is in here:
//   CRC-32, device ID rules, the identity record in the `ident` partition (one 4 KB sector, and the two-copy rule),
//   the canonical string and the three header lines (every output has an explicit capacity and is never overrun),
//   the Signer interface (software HMAC now, the eFuse HMAC unit later, same callers),
//   the clock offset for a board whose clock the service refuses, and the parser for the service's 401 body.
//
// Secrets: the only secret is the 32-byte device key inside IdentRecord. Nothing here prints it, puts it in a text or copies it
// further; every buffer that held it is wiped with secureZero.
#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace devauth {

static const size_t DEVICE_ID_LEN = 12;  // 12 lowercase hex characters: the factory MAC
static const size_t KEY_LEN = 32;
static const size_t SIG_LEN = 32;
static const size_t HOST_MAX = 63;       // the same limit as the saved service address
static const size_t TARGET_MAX = 255;    // path and query as sent; the board's longest is about 130
static const size_t TIMESTAMP_MAX_DIGITS = 12;
// "v1\n" + method (7) + "\n" + host (63) + "\n" + target (255) + "\n" + timestamp (12) + "\n" + body hash (64) + NUL, rounded up
static const size_t CANONICAL_CAP = 448;
// "X-Device: " + 12 + CRLF, "X-Timestamp: " + 12 + CRLF, "X-Signature: v1=" + 64 + CRLF, and a NUL
static const size_t HEADERS_CAP = 160;

// SHA-256 of the empty body, hex: the board sends only GET, so this is always its body hash.
static const char EMPTY_BODY_SHA256_HEX[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// Writes zeros that the compiler may not drop (a plain memset on a buffer that is about to go out of scope can be removed).
inline void secureZero(void *p, size_t n) {
  volatile uint8_t *v = (volatile uint8_t *)p;
  while (n--) *v++ = 0;
}

// ---- CRC-32 -----------------------------------------------------------------------------------------------------------
// The plain zlib/IEEE CRC-32 (what Python's zlib.crc32 and esp_crc32_le(0, ...) give). Bitwise: 48 bytes, once, at boot.
inline uint32_t crc32Ieee(const uint8_t *data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// ---- the device ID ------------------------------------------------------------------------------------------------------
// Valid means exactly [0-9a-f]{12}: lower case, no colons, no other length. `s` need not be NUL-terminated; `n` is its length.
inline bool validDeviceId(const char *s, size_t n) {
  if (!s || n != DEVICE_ID_LEN) return false;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}
inline bool validDeviceId(const char *s) {
  if (!s) return false;
  size_t n = 0;
  while (n <= DEVICE_ID_LEN && s[n]) n++;  // looks at no more than 13 bytes of an unbounded text
  return validDeviceId(s, n);
}

// The chip's factory MAC (6 bytes) as 12 lowercase hex characters plus a NUL.
inline void formatDeviceId(const uint8_t mac[6], char out[DEVICE_ID_LEN + 1]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 6; i++) {
    out[2 * i] = hex[mac[i] >> 4];
    out[2 * i + 1] = hex[mac[i] & 15];
  }
  out[DEVICE_ID_LEN] = 0;
}

// ---- the identity record -----------------------------------------------------------------------------------------------
// One sector of the `ident` partition (little-endian):
//    0  4  magic 0x544E4449 ("IDNT")      4  2  version, 1            6  2  payload length, 48
//    8  4  CRC-32 of the payload         12 12  device ID, ASCII     24  1  flags (bit 0: key is burned into the eFuse)
//   25  3  reserved, zero                28 32  interim software key (all zero once the key is burned)
// The rest of the 4 KB sector is 0xFF. The payload is bytes 12 to 59.
static const size_t SECTOR_BYTES = 4096;
static const uint32_t IDENT_MAGIC = 0x544E4449u;
static const uint16_t IDENT_VERSION = 1;
static const uint16_t IDENT_PAYLOAD_LEN = 48;
static const size_t IDENT_HEADER_LEN = 12;
static const size_t IDENT_RECORD_LEN = IDENT_HEADER_LEN + IDENT_PAYLOAD_LEN;  // 60: what has to be read from a sector
static const uint8_t IDENT_FLAG_BURNED = 1;  // the key is in the chip's eFuse: the software key must not be used

struct IdentRecord {
  char id[DEVICE_ID_LEN + 1] = "";
  uint8_t flags = 0;
  uint8_t key[KEY_LEN] = {};  // the secret: never printed, never copied out of here
  bool burned() const { return (flags & IDENT_FLAG_BURNED) != 0; }
  void wipe() {
    secureZero(id, sizeof(id));
    flags = 0;
    secureZero(key, sizeof(key));
  }
};

enum class RecordStatus : uint8_t { Ok, Blank, Short, BadMagic, BadVersion, BadLength, BadCrc, BadId };

inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Reads the record at the start of `sector` (`len` bytes are readable; IDENT_RECORD_LEN are needed). `out` is wiped first and
// holds a record only when this returns Ok. Blank = nothing was ever written here (no sector, or the first 12 bytes are 0xFF).
inline RecordStatus parseRecord(const uint8_t *sector, size_t len, IdentRecord &out) {
  out.wipe();
  if (!sector) return RecordStatus::Blank;
  if (len < IDENT_HEADER_LEN) return RecordStatus::Short;
  bool blank = true;
  for (size_t i = 0; i < IDENT_HEADER_LEN; i++)
    if (sector[i] != 0xFF) blank = false;
  if (blank) return RecordStatus::Blank;
  if (le32(sector) != IDENT_MAGIC) return RecordStatus::BadMagic;
  if (le16(sector + 4) != IDENT_VERSION) return RecordStatus::BadVersion;
  if (le16(sector + 6) != IDENT_PAYLOAD_LEN) return RecordStatus::BadLength;
  if (len < IDENT_RECORD_LEN) return RecordStatus::Short;
  if (crc32Ieee(sector + IDENT_HEADER_LEN, IDENT_PAYLOAD_LEN) != le32(sector + 8)) return RecordStatus::BadCrc;
  if (!validDeviceId((const char *)sector + 12, DEVICE_ID_LEN)) return RecordStatus::BadId;
  memcpy(out.id, sector + 12, DEVICE_ID_LEN);
  out.id[DEVICE_ID_LEN] = 0;
  out.flags = sector[24];
  memcpy(out.key, sector + 28, KEY_LEN);
  return RecordStatus::Ok;
}

// What the board knows about its own identity. The names are what /status reports.
enum class Identity : uint8_t { Ok, Missing, Invalid, OtherBoard, HardwareKey, HardwareKeyUnprotected };

inline const char *identityName(Identity i) {
  switch (i) {
    case Identity::Ok: return "ok";
    case Identity::Missing: return "missing";
    case Identity::Invalid: return "invalid";
    case Identity::OtherBoard: return "other_board";
    case Identity::HardwareKey: return "hardware_key";
    case Identity::HardwareKeyUnprotected: return "hardware_unprotected";
  }
  return "invalid";
}

// The two-copy rule and the checks that make a record usable on this chip. `s0` and `s1` are the first bytes of sector 0 and
// sector 1 (nullptr when there is no such sector). Sector 0 is used when it is valid, else sector 1. A valid record is then
// compared with the chip's own ID (`chipId`, 12 lowercase hex characters): a record copied from another board is refused.
//   Missing      neither sector was ever written (or there is no ident partition)
//   Invalid      something is there but no copy is valid, or the chip's own ID is not known
//   OtherBoard   a valid record, but for another chip
//   HardwareKey  a valid record whose key is burned into the eFuse, and this firmware has no hardware signer
//                (chooseSigner below also gives HardwareKey when the chip has no key although the record says it is burned,
//                 and HardwareKeyUnprotected when it has one that is not fully protected)
// `rec` holds the record only when this returns Ok; in every other case it is wiped.
inline Identity loadIdentity(const uint8_t *s0, size_t n0, const uint8_t *s1, size_t n1, const char *chipId, bool haveHardwareSigner,
                             IdentRecord &rec) {
  RecordStatus a = parseRecord(s0, n0, rec);
  if (a != RecordStatus::Ok) {
    RecordStatus b = parseRecord(s1, n1, rec);
    if (b != RecordStatus::Ok) {
      rec.wipe();
      return (a == RecordStatus::Blank && b == RecordStatus::Blank) ? Identity::Missing : Identity::Invalid;
    }
  }
  if (!validDeviceId(chipId)) {
    rec.wipe();
    return Identity::Invalid;
  }
  if (memcmp(rec.id, chipId, DEVICE_ID_LEN) != 0) {
    rec.wipe();
    return Identity::OtherBoard;
  }
  if (rec.burned() && !haveHardwareSigner) {
    rec.wipe();
    return Identity::HardwareKey;
  }
  return Identity::Ok;
}

// ---- the signed request --------------------------------------------------------------------------------------------------
// Lower-cases `host` (ASCII letters only) into `out` and checks it: 1 to 63 characters from [a-z0-9.-]. The board writes this
// text in its Host header and signs the same text. False (and out[0] = 0) when it is empty, too long or has other characters
// (a damaged saved value must never reach a header line).
inline bool lowerHost(const char *host, char *out, size_t cap) {
  if (!cap) return false;
  out[0] = 0;
  if (!host) return false;
  size_t n = 0;
  for (; host[n]; n++) {
    char c = host[n];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok || n >= HOST_MAX || n + 1 >= cap) {
      out[0] = 0;  // nothing half-written is left behind
      return false;
    }
    out[n] = c;
  }
  if (n == 0) return false;
  out[n] = 0;
  return true;
}

inline bool validMethod(const char *m) {
  size_t n = 0;
  for (; m && m[n]; n++)
    if (m[n] < 'A' || m[n] > 'Z' || n >= 7) return false;
  return n > 0;
}

// The request target as it appears in the request line: starts with '/', 1 to 255 printable ASCII characters without a space.
// (A newline in it would blur the six lines of the canonical string, so it is refused here.)
inline bool validTarget(const char *t) {
  if (!t || t[0] != '/') return false;
  size_t n = 0;
  for (; t[n]; n++)
    if ((uint8_t)t[n] < 0x21 || (uint8_t)t[n] > 0x7E || n >= TARGET_MAX) return false;
  return true;
}

inline bool validBodyHash(const char *h) {  // 64 lowercase hex characters
  if (!h) return false;
  size_t n = 0;
  for (; h[n]; n++)
    if (!((h[n] >= '0' && h[n] <= '9') || (h[n] >= 'a' && h[n] <= 'f')) || n >= 64) return false;
  return n == 64;
}

// Decimal text of `v` into `out` (at least 21 bytes); returns the number of digits.
inline size_t formatUnsigned(uint64_t v, char *out) {
  char tmp[21];
  size_t n = 0;
  do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
  out[n] = 0;
  return n;
}

// Appends n bytes at out[pos]; false when they and the closing NUL would not fit in cap.
inline bool appendBytes(char *out, size_t cap, size_t &pos, const char *s, size_t n) {
  if (pos + n + 1 > cap) return false;
  memcpy(out + pos, s, n);
  pos += n;
  out[pos] = 0;
  return true;
}
inline bool appendText(char *out, size_t cap, size_t &pos, const char *s) { return appendBytes(out, cap, pos, s, strlen(s)); }

// The canonical string: six lines joined with a single '\n', no trailing newline:
//   v1 / METHOD / host / request target / timestamp / lowercase hex SHA-256 of the body
// `host` must already be the lower-case text from lowerHost. The timestamp has at most 12 digits. `out` is NUL-terminated and
// `len` is the length without the NUL. False when an input is not valid or the result does not fit in cap: then out is "".
inline bool buildCanonical(char *out, size_t cap, size_t &len, const char *method, const char *host, const char *target,
                           uint64_t timestamp, const char *bodySha256Hex) {
  len = 0;
  if (cap) out[0] = 0;
  char lowered[HOST_MAX + 1];
  if (!cap || !validMethod(method) || !host || !lowerHost(host, lowered, sizeof(lowered)) || strcmp(lowered, host) != 0 ||
      !validTarget(target) || !validBodyHash(bodySha256Hex))
    return false;
  char ts[21];
  if (formatUnsigned(timestamp, ts) > TIMESTAMP_MAX_DIGITS) return false;
  size_t pos = 0;
  bool ok = appendText(out, cap, pos, "v1\n") && appendText(out, cap, pos, method) && appendText(out, cap, pos, "\n") &&
            appendText(out, cap, pos, host) && appendText(out, cap, pos, "\n") && appendText(out, cap, pos, target) &&
            appendText(out, cap, pos, "\n") && appendText(out, cap, pos, ts) && appendText(out, cap, pos, "\n") &&
            appendText(out, cap, pos, bodySha256Hex);
  if (!ok) {
    out[0] = 0;
    return false;
  }
  len = pos;
  return true;
}

inline bool appendHex(char *out, size_t cap, size_t &pos, const uint8_t *d, size_t n) {
  static const char hex[] = "0123456789abcdef";
  if (pos + 2 * n + 1 > cap) return false;
  for (size_t i = 0; i < n; i++) {
    out[pos++] = hex[d[i] >> 4];
    out[pos++] = hex[d[i] & 15];
  }
  out[pos] = 0;
  return true;
}

// The three header lines, each ending in CRLF, ready to go into the request:
//   X-Device: <id>   X-Timestamp: <seconds>   X-Signature: v1=<64 lowercase hex>
// False when an input is not valid or it does not fit in cap: then out is "".
inline bool buildHeaders(char *out, size_t cap, size_t &len, const char *deviceId, uint64_t timestamp, const uint8_t sig[SIG_LEN]) {
  len = 0;
  if (cap) out[0] = 0;
  char ts[21];
  if (!cap || !validDeviceId(deviceId) || !sig || formatUnsigned(timestamp, ts) > TIMESTAMP_MAX_DIGITS) return false;
  size_t pos = 0;
  bool ok = appendText(out, cap, pos, "X-Device: ") && appendText(out, cap, pos, deviceId) && appendText(out, cap, pos, "\r\n") &&
            appendText(out, cap, pos, "X-Timestamp: ") && appendText(out, cap, pos, ts) && appendText(out, cap, pos, "\r\n") &&
            appendText(out, cap, pos, "X-Signature: v1=") && appendHex(out, cap, pos, sig, SIG_LEN) && appendText(out, cap, pos, "\r\n");
  if (!ok) {
    out[0] = 0;
    return false;
  }
  len = pos;
  return true;
}

// ---- the signer -------------------------------------------------------------------------------------------------------------
// HMAC-SHA256 under the device key. Today a software signer (identity.cpp, mbedtls_md_hmac with the key from the record);
// later the eFuse HMAC unit (esp_hmac_calculate) behind the same interface, so no caller changes. A hardware signer needs the
// message and the output 4-byte aligned: signRequest below uses aligned buffers for both, and a signer that is handed another
// pointer must copy. `msg` may be any length, including zero.
struct Signer {
  virtual ~Signer() {}
  virtual bool sign(const uint8_t *msg, size_t len, uint8_t out[SIG_LEN]) = 0;
};

// Builds the canonical string for the request, signs it and writes the three header lines. False when an input is not valid,
// the signer fails or the headers do not fit in hdrCap: then hdr is "". The signature is wiped from the stack before returning.
inline bool signRequest(Signer &signer, const char *deviceId, const char *method, const char *host, const char *target,
                        uint64_t timestamp, const char *bodySha256Hex, char *hdr, size_t hdrCap, size_t &hdrLen) {
  hdrLen = 0;
  if (hdrCap) hdr[0] = 0;
  alignas(4) char canonical[CANONICAL_CAP];
  alignas(4) uint8_t sig[SIG_LEN];
  size_t clen = 0;
  bool ok = buildCanonical(canonical, sizeof(canonical), clen, method, host, target, timestamp, bodySha256Hex) &&
            signer.sign((const uint8_t *)canonical, clen, sig) && buildHeaders(hdr, hdrCap, hdrLen, deviceId, timestamp, sig);
  secureZero(sig, sizeof(sig));
  if (!ok && hdrCap) hdr[0] = 0;
  if (!ok) hdrLen = 0;
  return ok;
}

// ---- which signer: the saved software key, or the key burned into the chip --------------------------------------------------
// The record's flag bit 0 says whether the key is burned into the chip's eFuse (BLOCK_KEY5, purpose HMAC_UP). The chip's key
// counts as usable only when it is there (HMAC_UP, block in use) AND fully protected: read protection, write protection and the
// write protection of the purpose field. A key without all three is a burn that was not finished: it is never used. The rules:
//   flag set                          hardware only. No key in the chip: Identity::HardwareKey. A key that is not fully protected:
//                                     Identity::HardwareKeyUnprotected. Either way there is no software key left to fall back on.
//   flag clear, no key in the chip    software, the saved key (the behaviour before the burn).
//   flag clear, key not fully         software; the chip's key is never used; the board says so (SignerCheck::Unprotected).
//   protected
//   flag clear, a protected key       the burn is done but the record still holds the software key: check that both signers give
//   in the chip                       the same answers (signersAgree). Equal: hardware, and the software key leaves RAM.
//                                     Different: software stays, the chip's key is never used, and the board says so (Mismatch).
enum class SignerKind : uint8_t { None, Software, Hardware };
enum class SignerCheck : uint8_t { NotRun, Match, Mismatch, Unprotected };

struct SignerPlan {
  SignerKind kind = SignerKind::None;
  SignerCheck check = SignerCheck::NotRun;
  Identity problem = Identity::Ok;  // why there is no signer (kind None)
};

// Is the comparison needed? Only when both keys could be used: the record still has its key and the chip has a protected one.
inline bool needsSignerCheck(bool flagBurned, bool hwKeyPresent, bool hwProtected) { return !flagBurned && hwKeyPresent && hwProtected; }

// hwProtected only counts when hwKeyPresent; hwMatchesSoft only counts when needsSignerCheck is true (the caller runs
// signersAgree only then).
inline SignerPlan chooseSigner(bool flagBurned, bool hwKeyPresent, bool hwProtected, bool hwMatchesSoft) {
  SignerPlan p;
  bool unprotected = hwKeyPresent && !hwProtected;
  if (unprotected) p.check = SignerCheck::Unprotected;
  if (flagBurned) {
    if (hwKeyPresent && hwProtected) p.kind = SignerKind::Hardware;
    else p.problem = unprotected ? Identity::HardwareKeyUnprotected : Identity::HardwareKey;
    return p;
  }
  if (!hwKeyPresent || unprotected) {
    p.kind = SignerKind::Software;
    return p;
  }
  p.kind = hwMatchesSoft ? SignerKind::Hardware : SignerKind::Software;
  p.check = hwMatchesSoft ? SignerCheck::Match : SignerCheck::Mismatch;
  return p;
}

inline const char *signerKindName(SignerKind k) {
  switch (k) {
    case SignerKind::Software: return "software";
    case SignerKind::Hardware: return "hardware";
    case SignerKind::None: break;
  }
  return "none";
}
inline const char *signerCheckName(SignerCheck c) {
  switch (c) {
    case SignerCheck::Match: return "match";
    case SignerCheck::Mismatch: return "mismatch";
    case SignerCheck::Unprotected: return "unprotected";
    case SignerCheck::NotRun: break;
  }
  return "not_run";
}

// The messages that prove two signers are the same function: the lengths of the shared vectors' length table
// (shared/device-auth-vectors.json) that hit every branch of the hardware unit's padding, message byte i = (i * 7 + 3) & 0xff.
static const size_t VERIFY_COUNT = 11;
static const uint16_t VERIFY_LENGTHS[VERIFY_COUNT] = {0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 200};
static const size_t VERIFY_MAX = 200;

inline void verifyMessage(uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)((i * 7 + 3) & 0xff);
}

// Signs every verification message with both signers and compares every result. False when any answer differs or either signer
// fails. Nothing is printed and nothing outlives the call; the answers are signatures over public messages, wiped anyway.
inline bool signersAgree(Signer &a, Signer &b) {
  alignas(4) uint8_t msg[VERIFY_MAX];
  alignas(4) uint8_t ra[SIG_LEN], rb[SIG_LEN];
  bool same = true;
  for (size_t i = 0; i < VERIFY_COUNT && same; i++) {
    size_t len = VERIFY_LENGTHS[i];
    verifyMessage(msg, len);
    memset(ra, 0xA5, sizeof(ra));  // a signer that claims success but writes nothing must not look equal to another that does the same
    memset(rb, 0x5A, sizeof(rb));
    same = a.sign(msg, len, ra) && b.sign(msg, len, rb) && memcmp(ra, rb, SIG_LEN) == 0;
  }
  secureZero(ra, sizeof(ra));
  secureZero(rb, sizeof(rb));
  return same;
}

// ---- the clock -------------------------------------------------------------------------------------------------------------
// The service refuses a request whose timestamp is more than 2 minutes off and says what time it is (clock_skew). The board
// stores serverTime minus its own clock and adds it when it signs. The offset is for signing only: the system clock is not set.
// When NTP sets the system clock afterwards, the stored offset is wrong, so it is dropped: the system clock then moved by
// more than the uptime did (a jump), which apply() sees. ownNow is time(), upS a monotonic count of seconds since boot that
// never wraps (the board passes esp_timer_get_time() / 1000000, 64 bits; millis() would wrap after 49 days).
static const int64_t CLOCK_JUMP_S = 5;  // slack for the whole-second rounding of the two clocks

struct ClockOffset {
  bool have = false;
  int64_t offset = 0;       // serverTime - own clock, seconds
  int64_t ownAtStore = 0;
  int64_t upAtStore = 0;

  void clear() {
    have = false;
    offset = 0;
    ownAtStore = 0;
    upAtStore = 0;
  }
  void store(int64_t serverTime, int64_t ownNow, int64_t upS) {
    have = true;
    offset = serverTime - ownNow;
    ownAtStore = ownNow;
    upAtStore = upS;
  }
  // The time to put in the request: the own clock, plus the offset when there is one and the own clock was not set since.
  int64_t apply(int64_t ownNow, int64_t upS) {
    if (!have) return ownNow;
    int64_t ran = ownNow - ownAtStore;
    int64_t up = upS - upAtStore;
    int64_t jump = ran - up;
    if (jump < 0) jump = -jump;
    if (jump > CLOCK_JUMP_S) {
      clear();
      return ownNow;
    }
    return ownNow + offset;
  }
};

// ---- the refusal ---------------------------------------------------------------------------------------------------------------
// Status 401 with a JSON body {"error": "<a sentence>", "reason": "<code>", "serverTime": <seconds, only for clock_skew>}.
// The board acts on the code and ignores the sentence.
enum class Reason : uint8_t { Unknown, Missing, Malformed, BadSignature, Revoked, ClockSkew };

inline const char *reasonName(Reason r) {
  switch (r) {
    case Reason::Missing: return "missing";
    case Reason::Malformed: return "malformed";
    case Reason::BadSignature: return "bad_signature";
    case Reason::Revoked: return "revoked";
    case Reason::ClockSkew: return "clock_skew";
    case Reason::Unknown: break;
  }
  return "unknown";
}

struct Refusal {
  Reason reason = Reason::Unknown;
  bool hasServerTime = false;  // only ever true for clock_skew, with a plausible time
  int64_t serverTime = 0;
};

static const size_t REFUSAL_BODY_MAX = 1024;
static const int64_t SERVER_TIME_MIN = 1700000000;  // the same "the clock is set" line the rest of the firmware uses
static const int64_t SERVER_TIME_MAX = 4102444800;  // 1 Jan 2100

// Reads the body of a 401. True when it is a JSON object with a text `reason` (a code this firmware does not know is
// Reason::Unknown); false for anything else, with `out` reset. serverTime is taken only from a clock_skew refusal, and only when
// it is a whole number of seconds in a plausible range: the service reveals no time to a caller whose signature is wrong.
inline bool parseRefusal(const char *body, size_t len, Refusal &out) {
  out = Refusal();
  if (!body || len == 0 || len > REFUSAL_BODY_MAX) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body, len, DeserializationOption::NestingLimit(4))) return false;
  if (!doc.is<JsonObject>()) return false;
  JsonVariantConst why = doc["reason"];
  if (!why.is<const char *>()) return false;
  const char *r = why.as<const char *>();
  if (strcmp(r, "missing") == 0) out.reason = Reason::Missing;
  else if (strcmp(r, "malformed") == 0) out.reason = Reason::Malformed;
  else if (strcmp(r, "bad_signature") == 0) out.reason = Reason::BadSignature;
  else if (strcmp(r, "revoked") == 0) out.reason = Reason::Revoked;
  else if (strcmp(r, "clock_skew") == 0) out.reason = Reason::ClockSkew;
  if (out.reason == Reason::ClockSkew) {
    JsonVariantConst t = doc["serverTime"];
    if (t.is<uint32_t>()) {
      int64_t v = (int64_t)t.as<uint32_t>();
      if (v >= SERVER_TIME_MIN && v <= SERVER_TIME_MAX) {
        out.hasServerTime = true;
        out.serverTime = v;
      }
    }
  }
  return true;
}

// ---- the retry ------------------------------------------------------------------------------------------------------------------
// How long to wait before the next try of a fetch that failed. A board the service keeps refusing (a 401 reason other than the
// one-time clock_skew retry) or that cannot sign at all would otherwise ask again every 15 s or 60 s, for ever: it waits at least
// AUTH_RETRY_MS instead. Never shorter than the normal retry (normalMs), and a success goes back to the normal schedule at once,
// because the caller only asks this after a failure.
static const uint32_t AUTH_RETRY_MS = 5UL * 60 * 1000;  // the headlines' own retry; also inside problems.h's 10 minutes "still failing"

inline uint32_t retryDelayMs(bool authRefused, uint32_t normalMs) { return (authRefused && normalMs < AUTH_RETRY_MS) ? AUTH_RETRY_MS : normalMs; }

}  // namespace devauth
