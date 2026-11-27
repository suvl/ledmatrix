// Firmware update logic (slice 10, ticket 33; the contract is shared/firmware-update.md). Pure logic with no Arduino or ESP headers
// (ArduinoJson only reads the manifest), so it is tested on the laptop (tools/test_otalogic.cpp). The parts that touch the chip, the
// network and the portal are ota.cpp, live.cpp (the HTTPS stream) and net.cpp (the routes).
//
// What is in here:
//   Version        three numbers, digits only; compare; "is this version offered" (strictly newer, and newer than any version that
//                  already failed on this board)
//   Manifest       the service's answer, parsed and checked exactly as the contract says (the same rules as the service's own check); the
//                  two signature fields it may carry are only shaped here (strict hex), the check itself is updatesig.h
//   CodeAuth       the 6-digit code shown on the panel: random from an injected source, 3 minutes, 3 tries, constant-time compare,
//                  one pending at a time, and the throttle after a code is used up by wrong tries
//   Flow           the update state machine (idle ... probation) and which portal calls are legal in which state
//   CheckSchedule  when the board looks for an update by itself (about 5 minutes after the clock is set, then daily)
//   Health         the probation checklist: what must have happened within 3 minutes of a boot after an update
//   probationStep  the boot-time decision of the rollback mechanism: on probation or not, the boot counter (kept in NVS, carrying the image's
//                  identity), "kept restarting"
//   imageHeaderOk  a first look at the image header (the real validation is the ESP-IDF one, in ota.cpp)
//   Fail, failRaw  every way an update can fail, and the raw text (problems.h) that becomes its plain sentence
//
// Time is always passed in (milliseconds, the board's millis()) and compared with signed 32-bit differences, so a wrap after 49 days
// is harmless. The random source is passed in too. Nothing here prints, logs or returns the code except CodeAuth::codeText, which the
// panel alone uses.
#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "problems.h"

namespace otalogic {

// ---- versions ---------------------------------------------------------------------------------------------------------------
struct Version {
  uint16_t major = 0, minor = 0, patch = 0;
};
static const size_t VERSION_TEXT_MAX = 14;  // "9999.9999.9999"

// MAJOR.MINOR.PATCH: exactly three numbers of 1 to 4 digits, digits and dots only (no sign, no space, no tag). `n` is the length of
// the text; leading zeros are accepted and read as numbers (the service accepts them too). `out` is untouched on failure.
inline bool parseVersion(const char *s, size_t n, Version &out) {
  if (!s || n == 0 || n > VERSION_TEXT_MAX) return false;
  uint32_t parts[3] = {0, 0, 0};
  int part = 0, digits = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') {
      if (++digits > 4) return false;
      parts[part] = parts[part] * 10 + (uint32_t)(c - '0');
    } else if (c == '.') {
      if (digits == 0 || part == 2) return false;
      part++;
      digits = 0;
    } else {
      return false;  // a letter, a space, a sign, a NUL inside the text
    }
  }
  if (part != 2 || digits == 0) return false;
  out.major = (uint16_t)parts[0];
  out.minor = (uint16_t)parts[1];
  out.patch = (uint16_t)parts[2];
  return true;
}
inline bool parseVersion(const char *s, Version &out) {
  if (!s) return false;
  size_t n = 0;
  while (n <= VERSION_TEXT_MAX && s[n]) n++;  // looks at no more than 15 bytes of an unbounded text
  return parseVersion(s, n, out);
}

inline int compareVersions(const Version &a, const Version &b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return 0;
}

// "x.y.z" without leading zeros, into at least 15 bytes.
inline void formatVersion(const Version &v, char *out, size_t cap) {
  snprintf(out, cap, "%u.%u.%u", (unsigned)(v.major % 10000), (unsigned)(v.minor % 10000), (unsigned)(v.patch % 10000));  // a parsed version never has more than 4 digits
}

// Is this published version offered to the owner? Strictly newer than the running one, and (when a version has already failed on this
// board and was rolled back) strictly newer than that one too: the failed version, and anything older, is not offered again until a
// newer one exists.
inline bool isOffered(const Version &running, const Version &published, const Version *failed) {
  if (compareVersions(published, running) <= 0) return false;
  if (failed && compareVersions(published, *failed) <= 0) return false;
  return true;
}

// ---- the manifest -----------------------------------------------------------------------------------------------------------
static const size_t MANIFEST_MAX_BYTES = 2048;       // the answer is read into a buffer this big; more is rejected
static const uint32_t IMAGE_MIN_BYTES = 100 * 1024;  // the size must be ABOVE this (the service's rule)
static const uint32_t IMAGE_MAX_BYTES = 2097152;     // one app slot
static const size_t NOTES_MAX_CHARS = 200;           // characters, not bytes
static const size_t NOTES_MAX_BYTES = 4 * NOTES_MAX_CHARS;

// The signature fields of a manifest (shared/manifest-signing.md, 5.1 step 3). `Absent`: neither "keyid" nor "sig" (a null counts as absent).
// `Present`: both, each a string of exactly the right length in lowercase hex. `Malformed`: anything else (one of the two, a number, a wrong
// length, upper case, a character that is not hex). parseManifest never refuses a manifest for this: whether it matters is the policy's
// business (updatesig.h): a development build ignores the fields, a signing build refuses `Absent` and `Malformed`.
enum class SigFields : uint8_t { Absent, Present, Malformed };
static const size_t KEYID_HEX_LEN = 8;     // the first 4 bytes of SHA-256 of the public key, as lowercase hex
static const size_t SIG_RAW_LEN = 64;      // r then s, 32 bytes each, big endian
static const size_t SIG_HEX_LEN = 128;     // ...as lowercase hex on the wire

struct Manifest {
  char version[VERSION_TEXT_MAX + 2] = "";   // as the service wrote it
  Version v;
  char file[32] = "";                        // ledmatrix-<version>.bin
  uint32_t size = 0;
  char sha256[65] = "";                      // 64 lowercase hex
  char notes[NOTES_MAX_BYTES + 1] = "";
  SigFields sigFields = SigFields::Absent;
  char keyid[KEYID_HEX_LEN + 1] = "";        // 8 lowercase hex, only when sigFields is Present
  uint8_t sig[SIG_RAW_LEN] = {};             // decoded from the 128 hex characters, only when sigFields is Present
};

enum class ManifestError : uint8_t { None, TooLong, NotJson, NotObject, Version, File, Size, Sha256, Notes, NotesControl };

// One short phrase per reason, at most 46 characters, so "firmware manifest is not valid: <reason>" fits the problem list's detail.
inline const char *manifestErrorText(ManifestError e) {
  switch (e) {
    case ManifestError::None: return "";
    case ManifestError::TooLong: return "the answer is too long";
    case ManifestError::NotJson: return "the answer is not JSON";
    case ManifestError::NotObject: return "the answer is not a JSON object";
    case ManifestError::Version: return "version is missing or not three numbers";
    case ManifestError::File: return "file is missing or not ledmatrix-<version>.bin";
    case ManifestError::Size: return "size is missing or out of range";
    case ManifestError::Sha256: return "sha256 is missing or not 64 lowercase hex";
    case ManifestError::Notes: return "notes are not text or are over 200 characters";
    case ManifestError::NotesControl: return "notes hold a control character";
  }
  return "";
}

// Exactly n characters, each 0-9 or a-f. Upper case is not hex here: every value on the wire has one spelling (manifest-signing.md, section 1).
inline bool isLowerHex(const char *s, size_t n) {
  if (!s) return false;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}
inline bool isLowerHex64(const char *s, size_t n) { return n == 64 && isLowerHex(s, n); }

// Strict hex decoding: n must be even, n / 2 must fit in outCap, every character lowercase hex (so no upper case, no "0x", no space, no sign,
// no NUL). `out` is only complete when true comes back.
inline bool hexDecodeLower(const char *s, size_t n, uint8_t *out, size_t outCap) {
  if (!s || !out || n % 2 != 0 || n / 2 > outCap || !isLowerHex(s, n)) return false;
  for (size_t i = 0; i < n / 2; i++) {
    uint8_t v[2];
    for (int k = 0; k < 2; k++) {
      char c = s[2 * i + k];
      v[k] = (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    out[i] = (uint8_t)(v[0] << 4 | v[1]);
  }
  return true;
}

// Reads the service's answer to GET /v1/firmware/manifest and checks every rule of the contract:
//   version  text, three numbers of 1 to 4 digits
//   file     exactly ledmatrix-<version>.bin (the version's own text), so it can never be a host or a free path
//   size     a whole number above 100 KB (102400) and at most 2,097,152
//   sha256   64 lowercase hex characters
//   notes    text of at most 200 characters (a missing or null "notes" is the empty text, as the service reads it); a number, a
//            list or an object is rejected; no control character (no byte below 0x20, no 0x7F): the notes are signed and shown
//   keyid, sig   the signature fields, see SigFields: shaped here, never a reason to refuse the manifest (a development build ignores them)
// A field of the wrong type is as wrong as a missing one. Keys this firmware does not know are ignored, so a later service can add a
// field without locking older boards out of the update that would fix them. `out` is only complete when None comes back.
inline ManifestError parseManifest(const char *body, size_t len, Manifest &out) {
  if (!body || len == 0) return ManifestError::NotJson;
  if (len > MANIFEST_MAX_BYTES) return ManifestError::TooLong;
  JsonDocument doc;
  if (deserializeJson(doc, body, len)) return ManifestError::NotJson;
  JsonObjectConst obj = doc.as<JsonObjectConst>();
  if (obj.isNull()) return ManifestError::NotObject;
  Manifest m;

  JsonVariantConst ver = obj["version"];
  if (!ver.is<const char *>()) return ManifestError::Version;
  {
    JsonString s = ver.as<JsonString>();
    if (s.size() != strlen(s.c_str()) || !parseVersion(s.c_str(), s.size(), m.v)) return ManifestError::Version;
    memcpy(m.version, s.c_str(), s.size());
    m.version[s.size()] = 0;
  }

  JsonVariantConst file = obj["file"];
  if (!file.is<const char *>()) return ManifestError::File;
  {
    JsonString s = file.as<JsonString>();
    char want[sizeof(m.file)];
    int wn = snprintf(want, sizeof(want), "ledmatrix-%s.bin", m.version);
    if (wn < 0 || (size_t)wn >= sizeof(want) || s.size() != (size_t)wn || memcmp(s.c_str(), want, (size_t)wn) != 0) return ManifestError::File;
    memcpy(m.file, want, (size_t)wn + 1);
  }

  JsonVariantConst size = obj["size"];
  if (!size.is<long long>()) return ManifestError::Size;  // a whole number: not a string, not 1.5e6, not null
  {
    long long n = size.as<long long>();
    if (n <= (long long)IMAGE_MIN_BYTES || n > (long long)IMAGE_MAX_BYTES) return ManifestError::Size;
    m.size = (uint32_t)n;
  }

  JsonVariantConst sha = obj["sha256"];
  if (!sha.is<const char *>()) return ManifestError::Sha256;
  {
    JsonString s = sha.as<JsonString>();
    if (s.size() != strlen(s.c_str()) || !isLowerHex64(s.c_str(), s.size())) return ManifestError::Sha256;
    memcpy(m.sha256, s.c_str(), 64);
    m.sha256[64] = 0;
  }

  JsonVariantConst notes = obj["notes"];
  if (!notes.isNull()) {  // missing or null is the empty text (the service reads it that way)
    if (!notes.is<const char *>()) return ManifestError::Notes;
    JsonString s = notes.as<JsonString>();
    if (s.size() != strlen(s.c_str()) || s.size() > NOTES_MAX_BYTES) return ManifestError::Notes;
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); i++)
      if (((uint8_t)s.c_str()[i] & 0xC0) != 0x80) chars++;  // every byte that is not a UTF-8 continuation byte starts a character
    if (chars > NOTES_MAX_CHARS) return ManifestError::Notes;
    for (size_t i = 0; i < s.size(); i++) {
      uint8_t b = (uint8_t)s.c_str()[i];
      if (b < 0x20 || b == 0x7F) return ManifestError::NotesControl;  // the notes are signed and shown: no line feed, no escape, nothing that is not text
    }
    memcpy(m.notes, s.c_str(), s.size());
    m.notes[s.size()] = 0;
  }

  // The signature fields: both or neither, each in its one valid spelling. A null is as good as absent (the contract's rule for both fields).
  {
    JsonVariantConst kid = obj["keyid"], sg = obj["sig"];
    bool haveKid = !kid.isNull(), haveSig = !sg.isNull();
    if (haveKid || haveSig) {
      m.sigFields = SigFields::Malformed;
      if (haveKid && haveSig && kid.is<const char *>() && sg.is<const char *>()) {
        JsonString k = kid.as<JsonString>(), g = sg.as<JsonString>();
        if (k.size() == KEYID_HEX_LEN && g.size() == SIG_HEX_LEN && k.size() == strlen(k.c_str()) && g.size() == strlen(g.c_str()) &&
            isLowerHex(k.c_str(), k.size()) && hexDecodeLower(g.c_str(), g.size(), m.sig, sizeof(m.sig))) {
          memcpy(m.keyid, k.c_str(), KEYID_HEX_LEN);
          m.keyid[KEYID_HEX_LEN] = 0;
          m.sigFields = SigFields::Present;
        }
      }
    }
  }
  out = m;
  return ManifestError::None;
}

// The path the download is fetched from: /v1/firmware/ plus the manifest's file (which parseManifest has checked).
inline bool downloadPath(const Manifest &m, char *out, size_t cap) {
  int n = snprintf(out, cap, "/v1/firmware/%s", m.file);
  return n > 0 && (size_t)n < cap;
}

// Is the SHA-256 the board computed (as 64 lowercase hex) the one the manifest announced? Both must be exactly 64 characters.
inline bool sha256Matches(const char *computedHex, const char *manifestHex) {
  if (!computedHex || !manifestHex) return false;
  size_t a = 0, b = 0;
  while (a <= 64 && computedHex[a]) a++;  // looks at no more than 65 bytes of an unbounded text
  while (b <= 64 && manifestHex[b]) b++;
  if (a != 64 || b != 64) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < 64; i++) diff |= (uint8_t)(computedHex[i] ^ manifestHex[i]);
  return diff == 0;
}

inline void sha256ToHex(const uint8_t digest[32], char out[65]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    out[2 * i] = hex[digest[i] >> 4];
    out[2 * i + 1] = hex[digest[i] & 15];
  }
  out[64] = 0;
}

// ---- the image header -------------------------------------------------------------------------------------------------------
// The first 24 bytes of an app image (esp_image_header_t). This is only a first look, made before the first byte goes to flash, so
// a file that is plainly not an ESP32-S3 program is refused without touching the slot. The real validation (segments, checksum,
// the appended SHA-256, the app description) is the ESP-IDF's and runs in ota.cpp before the boot slot is switched.
static const uint8_t IMAGE_MAGIC = 0xE9;
static const uint16_t CHIP_ID_ESP32S3 = 9;
static const size_t IMAGE_HEADER_LEN = 24;
inline bool imageHeaderOk(const uint8_t *p, size_t n) {
  if (!p || n < IMAGE_HEADER_LEN) return false;
  if (p[0] != IMAGE_MAGIC) return false;
  if (p[1] == 0 || p[1] > 16) return false;                              // segment count
  if (p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0) return false;   // entry point
  uint16_t chip = (uint16_t)(p[12] | (p[13] << 8));
  return chip == CHIP_ID_ESP32S3;
}

// ---- ways to fail -----------------------------------------------------------------------------------------------------------
enum class Fail : uint8_t {
  None,
  NoFirmware,        // the service has no firmware published (404)
  ManifestInvalid,
  NoService,         // no service address saved (a check was asked for)
  NoClock,           // the clock is not set, secure connections cannot work
  DownloadFailed,
  UploadIncomplete,
  WrongSize,
  WrongChecksum,
  WrongVersion,      // the image says it is another version than the manifest announced
  BadImage,
  TooBig,
  FlashFailed,
  WrongCode,         // 3 wrong tries
  CodeExpired,
  RolledBack,
  CannotRollBack,    // the new image failed its probation and there is no previous image to go back to
  CannotConfirm,     // the new image passed its probation but the board could not record it
  // The signed manifest (shared/manifest-signing.md, updatesig.h): a refusal is soft, nothing is downloaded or written. A manifest whose
  // signature fields are malformed is ManifestInvalid with SIGNATURE_FIELDS_MALFORMED as the detail.
  Unsigned,                // no keyid and no sig, and this build requires a signature
  UnknownKey,              // keyid is not one of the pinned keys
  BadSignature,            // a pinned key, and the signature does not verify
  SignatureCheckFailed,    // the check could not run (out of memory, an internal error): not an attack, tried again at the next check
};
static const char SIGNATURE_FIELDS_MALFORMED[] = "the signature fields are malformed";  // the detail after PROBLEM_RAW_UPDATE_MANIFEST_INVALID

// The raw text (problems.h) whose plain sentence says this. A detail may be appended after ": " by the caller.
inline const char *failRaw(Fail f) {
  switch (f) {
    case Fail::None: return "";
    case Fail::NoFirmware: return PROBLEM_RAW_UPDATE_NO_FIRMWARE;
    case Fail::ManifestInvalid: return PROBLEM_RAW_UPDATE_MANIFEST_INVALID;
    case Fail::NoService: return PROBLEM_RAW_UPDATE_NO_SERVICE;
    case Fail::NoClock: return PROBLEM_RAW_UPDATE_NO_CLOCK;
    case Fail::DownloadFailed: return PROBLEM_RAW_UPDATE_DOWNLOAD_FAILED;
    case Fail::UploadIncomplete: return PROBLEM_RAW_UPDATE_UPLOAD_INCOMPLETE;
    case Fail::WrongSize: return PROBLEM_RAW_UPDATE_WRONG_SIZE;
    case Fail::WrongChecksum: return PROBLEM_RAW_UPDATE_WRONG_CHECKSUM;
    case Fail::WrongVersion: return PROBLEM_RAW_UPDATE_WRONG_VERSION;
    case Fail::BadImage: return PROBLEM_RAW_UPDATE_BAD_IMAGE;
    case Fail::TooBig: return PROBLEM_RAW_UPDATE_TOO_BIG;
    case Fail::FlashFailed: return PROBLEM_RAW_UPDATE_FLASH_FAILED;
    case Fail::WrongCode: return PROBLEM_RAW_UPDATE_WRONG_CODE;
    case Fail::CodeExpired: return PROBLEM_RAW_UPDATE_CODE_EXPIRED;
    case Fail::RolledBack: return PROBLEM_RAW_UPDATE_ROLLED_BACK;
    case Fail::CannotRollBack: return PROBLEM_RAW_UPDATE_NO_ROLLBACK;
    case Fail::CannotConfirm: return PROBLEM_RAW_UPDATE_NOT_CONFIRMED;
    case Fail::Unsigned: return PROBLEM_RAW_UPDATE_UNSIGNED;
    case Fail::UnknownKey: return PROBLEM_RAW_UPDATE_UNKNOWN_KEY;
    case Fail::BadSignature: return PROBLEM_RAW_UPDATE_BAD_SIGNATURE;
    case Fail::SignatureCheckFailed: return PROBLEM_RAW_UPDATE_CHECK_FAILED;
  }
  return "";
}

// ---- the authorisation code -------------------------------------------------------------------------------------------------
enum class Source : uint8_t { None, Service, Upload };
inline const char *sourceName(Source s) { return s == Source::Service ? "service" : s == Source::Upload ? "upload" : nullptr; }

static const uint32_t CODE_LIFETIME_MS = 3UL * 60 * 1000;
static const int CODE_TRIES = 3;
static const uint32_t THROTTLE_FIRST_S = 60;   // after a code is used up by wrong tries: no new start for this long, then double
static const uint32_t THROTTLE_MAX_S = 3600;
static const size_t CODE_DIGITS = 6;

// Where the code's random numbers come from: the chip's hardware generator on the board, a script in the tests.
struct RandomSource {
  virtual uint32_t next32() = 0;
  virtual ~RandomSource() {}
};

enum class StartResult : uint8_t { Started, Throttled };
enum class CheckResult : uint8_t {
  Ok,         // the code was right: it is spent, the update may go on
  Wrong,      // wrong, and tries are left
  UsedUp,     // wrong, and that was the last try: the code is gone and the throttle starts
  Expired,    // the 3 minutes were over: the code is gone
  NoPending   // there was no code
};

// Constant-time comparison of what was typed with the 6 digits of the code: always looks at the same number of bytes, whatever
// was typed, and never stops at the first difference. Anything but exactly the 6 digits is simply different.
inline bool codesEqual(const char *typed, const char *code) {
  size_t n = 0;
  if (typed)
    while (n <= CODE_DIGITS && typed[n]) n++;  // no more than 7 bytes of an unbounded text
  uint8_t diff = (uint8_t)(typed == nullptr || n != CODE_DIGITS);
  for (size_t i = 0; i < CODE_DIGITS; i++) {
    uint8_t t = (typed && i < n) ? (uint8_t)typed[i] : 0;
    diff |= (uint8_t)(t ^ (uint8_t)code[i]);
  }
  return diff == 0;
}

// Milliseconds from `then` to `now`, with the clock's wrap handled by unsigned subtraction, so it is right for any span up to 49.7 days
// (a signed 32-bit difference flips after 24.9 days). A `then` a few seconds ahead of `now` (a time read in another task just before
// the event was recorded) counts as 0 instead of as 49 days.
inline uint32_t sinceMs(uint32_t nowMs, uint32_t thenMs) {
  uint32_t d = nowMs - thenMs;
  return d > 0xFFFFF000u ? 0 : d;
}

class CodeAuth {
 public:
  // The owner pressed Update: a new random code replaces any pending one. Refused (Throttled) for a while after a code was used up
  // by wrong tries. Wrong tries already made on a code that this replaces still count (see strikes_), so starting again over and
  // over is no way round the 3 tries.
  StartResult start(Source src, uint32_t nowMs, RandomSource &rng) {
    if (expired(nowMs)) {
      clear();
      strikes_ = 0;
    }
    endThrottleIfOver(nowMs);
    if (locked_) return StartResult::Throttled;
    unsigned n = draw(rng);
    snprintf(code_, sizeof(code_), "%06u", n);
    pending_ = true;
    src_ = src;
    startedMs_ = nowMs;
    triesLeft_ = CODE_TRIES - strikes_;
    return StartResult::Started;
  }

  // The owner typed a code (the portal's confirm, or the upload's form field). Whatever the text is, a wrong one costs a try.
  CheckResult check(const char *typed, uint32_t nowMs) {
    if (!pending_) return CheckResult::NoPending;
    if (expired(nowMs)) {
      clear();
      strikes_ = 0;
      return CheckResult::Expired;
    }
    if (codesEqual(typed, code_)) {
      clear();
      strikes_ = 0;
      level_ = 0;  // a correct code resets the throttle
      return CheckResult::Ok;
    }
    strikes_++;
    triesLeft_--;
    if (triesLeft_ <= 0) {
      clear();
      strikes_ = 0;
      lockFor(nowMs);
      return CheckResult::UsedUp;
    }
    return CheckResult::Wrong;
  }

  // Cancel (or the board gives up on the code): the code is dropped, its wrong tries stay counted.
  bool cancel() {
    bool was = pending_;
    clear();
    return was;
  }

  // Called now and then (every half second on the board): true once, when a pending code has just run out of time (it is gone then).
  // It also ends a throttle that is over, so no old lock can ever come back as the clock runs on.
  bool tick(uint32_t nowMs) {
    endThrottleIfOver(nowMs);
    if (!expired(nowMs)) return false;
    clear();
    strikes_ = 0;
    return true;
  }

  bool pending() const { return pending_; }
  Source source() const { return pending_ ? src_ : Source::None; }
  int triesLeft() const { return pending_ ? triesLeft_ : 0; }
  uint32_t secondsLeft(uint32_t nowMs) const {
    if (!pending_) return 0;
    uint32_t elapsed = sinceMs(nowMs, startedMs_);
    return elapsed >= CODE_LIFETIME_MS ? 0 : (CODE_LIFETIME_MS - elapsed + 999) / 1000;
  }
  uint32_t throttleSecondsLeft(uint32_t nowMs) const {
    if (!locked_) return 0;
    uint32_t elapsed = sinceMs(nowMs, lockStartMs_);
    return elapsed >= lockLenMs_ ? 0 : (lockLenMs_ - elapsed + 999) / 1000;
  }
  // The six digits, for the panel only (the one place that may show them). False, with out empty, when no code is pending.
  bool codeText(char out[CODE_DIGITS + 1]) const {
    if (!pending_) {
      out[0] = 0;
      return false;
    }
    memcpy(out, code_, CODE_DIGITS + 1);
    return true;
  }

 private:
  bool pending_ = false;
  Source src_ = Source::None;
  char code_[CODE_DIGITS + 1] = "";
  uint32_t startedMs_ = 0;
  int triesLeft_ = 0;
  int strikes_ = 0;       // wrong tries made since the last success, used-up code or expiry: a code that replaces another has fewer tries
  int level_ = 0;         // how many times a code was used up since the last success: 0 means the next throttle is 60 s
  bool locked_ = false;        // a throttle is running (cleared as soon as it is over: see endThrottleIfOver)
  uint32_t lockStartMs_ = 0;
  uint32_t lockLenMs_ = 0;

  bool expired(uint32_t nowMs) const { return pending_ && sinceMs(nowMs, startedMs_) >= CODE_LIFETIME_MS; }
  void endThrottleIfOver(uint32_t nowMs) {
    if (locked_ && sinceMs(nowMs, lockStartMs_) >= lockLenMs_) locked_ = false;
  }

  void clear() {
    volatile char *v = code_;  // wiped so the code does not linger in RAM
    for (size_t i = 0; i < sizeof(code_); i++) v[i] = 0;
    pending_ = false;
    src_ = Source::None;
    triesLeft_ = 0;
  }

  void lockFor(uint32_t nowMs) {
    uint32_t secs = THROTTLE_FIRST_S;
    for (int i = 0; i < level_ && secs < THROTTLE_MAX_S; i++) secs *= 2;
    if (secs > THROTTLE_MAX_S) secs = THROTTLE_MAX_S;
    if (level_ < 30) level_++;
    locked_ = true;
    lockStartMs_ = nowMs;
    lockLenMs_ = secs * 1000;
  }

  // 0..999999 without bias: a draw from the top of the 32-bit range, which does not divide evenly, is thrown away and drawn again.
  static unsigned draw(RandomSource &rng) {
    static const uint32_t LIMIT = 4294000000u;  // the largest multiple of 1,000,000 not above 2^32
    uint32_t r = rng.next32();
    for (int i = 0; i < 32 && r >= LIMIT; i++) r = rng.next32();
    return (unsigned)(r % 1000000u);
  }
};

// ---- the update flow --------------------------------------------------------------------------------------------------------
enum class State : uint8_t { Idle, Checking, WaitingCode, Downloading, Installing, Restarting, Failed, Probation };
inline const char *stateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Checking: return "checking";
    case State::WaitingCode: return "waiting_code";
    case State::Downloading: return "downloading";
    case State::Installing: return "installing";
    case State::Restarting: return "restarting";
    case State::Failed: return "failed";
    case State::Probation: return "probation";
  }
  return "idle";
}

enum class Call : uint8_t { Check, Start, Confirm, Upload, Cancel };

// Why a portal call is not allowed right now (HTTP 409 with these words in `error`).
enum class Refusal : uint8_t { None, Busy, Probation, NoOffer, NoPending, WrongSource, AlreadyChecking };
inline const char *refusalText(Refusal r) {
  switch (r) {
    case Refusal::None: return "";
    case Refusal::Busy: return "An update is being installed right now.";
    case Refusal::Probation: return "The new firmware is still being checked; try again in a few minutes.";
    case Refusal::NoOffer: return "No update is available. Press Check now first.";
    case Refusal::NoPending: return "No update is waiting for a code. Press Update first.";
    case Refusal::WrongSource: return "The code on the panel is for the other kind of update. Press Update again.";
    case Refusal::AlreadyChecking: return "A check is already running.";
  }
  return "";
}

struct Flow {
  State base = State::Idle;  // never Checking: that is shown() when a check runs on an idle board
  bool checking = false;     // a check is in flight; it can run beside a pending code
  Source source = Source::None;
  uint32_t done = 0, total = 0;
  char error[160] = "";      // the last failure in plain words; cleared when the next check or update starts

  State shown() const { return (checking && base == State::Idle) ? State::Checking : base; }
  bool flashing() const { return base == State::Downloading || base == State::Installing || base == State::Restarting; }

  // Is this call allowed now? `haveOffer`: an update is on offer (a check found a newer version). `src`: what Start/Confirm/Upload are
  // for. `unconfirmed`: the running image is on probation and not yet marked good (probationActive()): while it is, nothing may write
  // to the other slot, because that slot holds the only image the board can go back to. This is the image's own state, not the flow's:
  // a failed rollback moves the flow to "failed" and the image is still unconfirmed. The checks are in the order of how plain the
  // reason is: busy first.
  Refusal allow(Call c, bool haveOffer, Source src = Source::None, bool unconfirmed = false) const {
    switch (c) {
      case Call::Check:
        if (flashing()) return Refusal::Busy;
        if (checking) return Refusal::AlreadyChecking;
        return Refusal::None;
      case Call::Start:
        if (flashing()) return Refusal::Busy;
        if (unconfirmed) return Refusal::Probation;
        if (src == Source::Service && !haveOffer) return Refusal::NoOffer;
        return Refusal::None;
      case Call::Confirm:
      case Call::Upload: {
        Source want = c == Call::Confirm ? Source::Service : Source::Upload;
        if (flashing()) return Refusal::Busy;
        if (unconfirmed) return Refusal::Probation;
        if (base != State::WaitingCode) return Refusal::NoPending;
        if (source != want) return Refusal::WrongSource;
        return Refusal::None;
      }
      case Call::Cancel:
        return flashing() ? Refusal::Busy : Refusal::None;
    }
    return Refusal::None;
  }

  void setError(const char *sentence) { snprintf(error, sizeof(error), "%s", sentence ? sentence : ""); }

  void beginCheck() {
    checking = true;
    if (base == State::Failed) {
      base = State::Idle;
      source = Source::None;
    }
    error[0] = 0;
  }
  void endCheck() { checking = false; }

  // A code is on the panel (a new start replaces the old one).
  void startAuth(Source s) {
    base = State::WaitingCode;
    source = s;
    done = total = 0;
    error[0] = 0;
  }
  // The code was dropped (cancel), or ran out, or was used up: back to normal; with `sentence` the portal says why.
  void authGone(const char *sentence) {
    if (base != State::WaitingCode) return;
    if (sentence && sentence[0]) {
      base = State::Failed;
      setError(sentence);
    } else {
      base = State::Idle;
      source = Source::None;
    }
  }
  // The right code was typed: the bytes start to flow.
  void authOk() {
    if (base != State::WaitingCode) return;
    base = State::Downloading;
    done = total = 0;
  }
  void progress(uint32_t d, uint32_t t) {
    if (base != State::Downloading) return;
    done = d;
    total = t;
  }
  void downloaded() {
    if (base == State::Downloading) base = State::Installing;
  }
  void installed() {
    if (base == State::Installing) base = State::Restarting;
  }
  // The update failed at any point: nothing was switched. The panel goes back to normal, the portal says why.
  void fail(const char *sentence) {
    if (base == State::Restarting) return;  // the new image is set: too late to fail
    base = State::Failed;
    setError(sentence);
  }
  // This boot is the first of an updated image on probation / the image was marked good.
  void bootProbation() { base = State::Probation; }
  void markedGood() {
    if (base == State::Probation) base = State::Idle;
  }
};

// The words the panel shows on its progress screen (at most 23 characters, UpdateView::line).
inline const char *progressWord(State s, Source src, bool fetchingManifest) {
  switch (s) {
    case State::Downloading: return fetchingManifest ? "Checking" : (src == Source::Upload ? "Receiving" : "Downloading");
    case State::Installing: return "Installing";
    case State::Restarting: return "Restarting";
    default: return "";
  }
}

// How far along, 0..100 (floor). A total of 0 is 0; more done than total is 100.
inline int progressPercent(uint32_t done, uint32_t total) {
  if (total == 0) return 0;
  if (done >= total) return 100;
  return (int)((uint64_t)done * 100 / total);
}

// ---- when the board looks for an update by itself ---------------------------------------------------------------------------
static const uint32_t FIRST_CHECK_MS = 5UL * 60 * 1000;         // about 5 minutes after the clock is set
static const uint32_t CHECK_EVERY_MS = 24UL * 60 * 60 * 1000;   // then daily
static const uint32_t CHECK_RETRY_MS = 60UL * 60 * 1000;        // after a check that failed: an hour (not in the contract: a failed first check must not cost a day)

struct CheckSchedule {
  bool armed = false;
  uint32_t dueAt = 0;
  bool requested = false;  // "Check now"

  // Call every tick with the clock state and whether a service address is saved. The first time the clock is set, the first check is
  // 5 minutes away. Without a saved service address there is nothing to check: the schedule waits, and starts its 5 minutes when an
  // address appears.
  void tick(uint32_t nowMs, bool clockSet, bool serviceSaved) {
    if (clockSet && !armed) {
      armed = true;
      dueAt = nowMs + FIRST_CHECK_MS;
    }
    if (armed && !serviceSaved) dueAt = nowMs + FIRST_CHECK_MS;
  }
  void checkNow() { requested = true; }
  // Is a check due? "Check now" is due at once; the daily one needs the clock set and a service address.
  bool due(uint32_t nowMs, bool clockSet, bool serviceSaved) const {
    if (requested) return true;
    return armed && clockSet && serviceSaved && (int32_t)(nowMs - dueAt) >= 0;
  }
  // A check ran (whoever asked for it): the next daily one counts from now.
  void done(uint32_t nowMs, bool ok) {
    requested = false;
    dueAt = nowMs + (ok ? CHECK_EVERY_MS : CHECK_RETRY_MS);
  }
};

// ---- the probation checklist ------------------------------------------------------------------------------------------------
// An image installed by the updater is marked good only when ALL of these have happened within 3 minutes of its boot: Wi-Fi joined,
// the portal answering, the render task drawn at least 100 frames with none over 1 s, and the service reached (one complete HTTP
// answer of any status; waived, see serviceCheckApplies). This is only the checklist; what happens at "good" or "failed" is the
// mechanism (ota.cpp), which runs it on a task of its own.
static const uint32_t PROBATION_MS = 3UL * 60 * 1000;
static const uint32_t PROBATION_MIN_FRAMES = 100;
static const uint32_t PROBATION_MIN_SOAK_MS = 60UL * 1000;  // never marked good sooner than this after boot, whatever has happened by then

struct HealthInputs {
  bool wifiJoined = false;
  bool portalAnswering = false;
  uint32_t framesDrawn = 0;    // the render task's counter since boot
  uint32_t framesOver1s = 0;   // frames that came more than 1 s after the one before, since boot
  bool serviceReached = false; // the saved service host has given one complete HTTP answer (any status) to a request of the board
  bool serviceExpected = true; // false waives serviceReached: see serviceCheckApplies
};

// Is this HTTP status a complete answer from the server? Any status line counts: 200, but also a 401 (the service refused this board's
// key: it was reached, and a board that is refused must still be able to take the update that fixes it), 404, 5xx.
inline bool isHttpAnswer(int status) { return status >= 100 && status <= 599; }

// Would the data task fetch anything from the service with these settings? (The conditions of its loop in live.cpp.) With an address
// saved but nothing switched on, no fetch ever happens.
inline bool anythingToFetch(bool showFlights, bool showWeather, bool showNews, bool hasLocation, bool hasTopics) {
  return ((showFlights || showWeather) && hasLocation) || (showNews && hasTopics);
}

// The "service reached" condition applies only when a service address is saved, the identity is ok (otherwise nothing could be sent:
// the identity problem has its own sentence), and something would be fetched. Otherwise it is waived.
inline bool serviceCheckApplies(bool serviceSaved, bool identityOk, bool somethingToFetch) {
  return serviceSaved && identityOk && somethingToFetch;
}

enum class HealthVerdict : uint8_t { Pending, Good, Failed };

// A condition that has been seen to hold within the 3 minutes stays met (these are things that have happened). A frame over 1 s can
// never be undone, so it fails at once. Good needs every condition met AND at least a minute since boot (PROBATION_MIN_SOAK_MS). At the
// first look after the 3 minutes, anything not yet met fails. The verdict is final.
class Health {
 public:
  void begin(uint32_t bootMs) {
    boot_ = bootMs;
    verdict_ = HealthVerdict::Pending;
    for (bool &m : met_) m = false;
    why_[0] = 0;
  }

  HealthVerdict evaluate(uint32_t nowMs, const HealthInputs &in) {
    if (verdict_ != HealthVerdict::Pending) return verdict_;
    if (in.framesOver1s > 0) return fail("The panel stalled for more than a second.");
    bool inTime = (uint32_t)(nowMs - boot_) <= PROBATION_MS;
    if (inTime) {
      if (in.wifiJoined) met_[0] = true;
      if (in.portalAnswering) met_[1] = true;
      if (in.framesDrawn >= PROBATION_MIN_FRAMES) met_[2] = true;
      if (in.serviceReached || !in.serviceExpected) met_[3] = true;
    }
    if (met_[0] && met_[1] && met_[2] && met_[3]) {
      // everything has happened, but a new image has to run a minute before it is kept: a late stall or crash still counts
      if ((uint32_t)(nowMs - boot_) < PROBATION_MIN_SOAK_MS) return HealthVerdict::Pending;
      verdict_ = HealthVerdict::Good;
      return verdict_;
    }
    if (inTime) return HealthVerdict::Pending;
    static const char *const reasons[4] = {
        "The board did not join Wi-Fi within 3 minutes of starting.",
        "The portal did not answer within 3 minutes of starting.",
        "The panel did not draw 100 frames within 3 minutes of starting.",
        "The service did not answer within 3 minutes of starting.",
    };
    for (int i = 0; i < 4; i++)
      if (!met_[i]) return fail(reasons[i]);
    return fail(reasons[0]);  // not reached
  }

  HealthVerdict verdict() const { return verdict_; }
  const char *why() const { return why_; }  // the plain sentence when the verdict is Failed

 private:
  uint32_t boot_ = 0;
  HealthVerdict verdict_ = HealthVerdict::Pending;
  bool met_[4] = {false, false, false, false};
  char why_[96] = "";

  HealthVerdict fail(const char *sentence) {
    verdict_ = HealthVerdict::Failed;
    snprintf(why_, sizeof(why_), "%s", sentence);
    return verdict_;
  }
};

// ---- the rollback mechanism's boot-time decision ---------------------------------------------------------------------------
// The board's bootloader does not roll back (bench test, shared/firmware-update.md), so the application does, with the OTA state record
// it keeps itself. An image on probation (the slot's state is NEW or PENDING_VERIFY: the updater's esp_ota_set_boot_partition sets NEW; an
// image flashed over USB is UNDEFINED and never on probation) counts its boots, and more than 3 boots of the same probation (3 crashes or power
// cuts) means it is not working: go back.
//
// The count is in NVS (ticket 47), so a power cut cannot defeat the rollback (the RTC no-init memory it used to live in is wiped by a power-on
// reset). It exists ONLY while an image is on probation, and it carries the identity of the image it counts for, so a count left by another image
// (a probation that ended in a way that did not clear it) never counts against a new one. Flash writes: at most one per boot on probation and one
// to clear it, so at most 4 for one update (boots 1 to 3 write; the 4th boot rolls back without writing; the image that comes back, or the
// image that was marked good, erases it). The pure decision is here; ota.cpp reads and writes the NVS key.
enum class ImageState : uint8_t { Undefined, New, PendingVerify, Valid, Invalid, Aborted };
inline bool isProbation(ImageState s) { return s == ImageState::New || s == ImageState::PendingVerify; }

static const uint32_t PROBATION_MAX_BOOTS = 3;
static const char WHY_RESTART_LOOP[] = "The new version kept restarting.";

// What is kept in NVS (one 64-bit item: the image in the high half, the count in the low half; an absent item is "no count"). `image` is never 0.
struct BootRecord {
  uint32_t image = 0;   // which image this count is for (imageIdentity)
  uint32_t boots = 0;   // the number of the last boot of that image on probation (1 to PROBATION_MAX_BOOTS)
};
inline uint64_t packBootRecord(const BootRecord &r) { return ((uint64_t)r.image << 32) | r.boots; }
inline BootRecord unpackBootRecord(uint64_t v) {
  BootRecord r;
  r.image = (uint32_t)(v >> 32);
  r.boots = (uint32_t)v;
  return r;
}

// Who is this image? From the first bytes of the SHA-256 of the app's ELF file (esp_app_desc_t::app_elf_sha256: it changes with any change of
// the program) and the address of the slot it runs from, so the same program in the other slot, or another program in the same slot, is a different
// image. FNV-1a over the address (4 bytes) and the digest bytes. Never 0 (0 means "no image" in a BootRecord).
inline uint32_t imageIdentity(const uint8_t *digest, size_t n, uint32_t slotAddress) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 4; i++) h = (h ^ (uint8_t)(slotAddress >> (8 * i))) * 16777619u;
  for (size_t i = 0; digest && i < n; i++) h = (h ^ digest[i]) * 16777619u;
  return h ? h : 1;
}

// What ota.cpp found in NVS: nothing there (the normal case), a count, or an error reading it (the store could not be read at all).
enum class CountRead : uint8_t { Missing, Found, Unreadable };
// What to do with the NVS item after this boot's decision: leave it, write `record`, or erase it.
enum class CountSave : uint8_t { Keep, Write, Erase };

struct ProbationStep {
  bool onProbation = false;
  uint32_t boots = 0;       // this boot's number within the probation: 1 for the first
  bool rollback = false;    // more than PROBATION_MAX_BOOTS: go back now
  CountSave save = CountSave::Keep;
  BootRecord record;        // what to write when save is Write
};

// Called first thing in setup(), with what ota.cpp read from NVS (`stored` matters only when read is Found).
//  - Not on probation: no count may exist; one that does (a probation that ended without clearing it) is erased, so the next probation starts at 1.
//  - On probation, the count goes up by one when it is a count of THIS image; otherwise it starts at 1: nothing was there, it is another
//    image's, it is not a number this firmware could have written (0, or above the maximum: a count is never kept once it asks for a rollback),
//    or NVS could not be read. A read failure never rolls back by itself: it counts as the first boot, the conservative reading.
//  - The 4th boot rolls back and writes nothing: if the rollback cannot be done (no previous image) the 4th boot is worked out again from the
//    count of 3 at the next restart, and the image that does come back erases the count.
inline ProbationStep probationStep(bool onProbation, uint32_t image, CountRead read, const BootRecord &stored) {
  ProbationStep r;
  if (!onProbation) {
    r.save = read == CountRead::Missing ? CountSave::Keep : CountSave::Erase;
    return r;
  }
  uint32_t boots = 1;
  if (read == CountRead::Found && image != 0 && stored.image == image && stored.boots >= 1 && stored.boots <= PROBATION_MAX_BOOTS) boots = stored.boots + 1;
  r.onProbation = true;
  r.boots = boots;
  r.rollback = boots > PROBATION_MAX_BOOTS;
  if (!r.rollback) {
    r.save = CountSave::Write;
    r.record.image = image;
    r.record.boots = boots;
  }
  return r;
}

// A saved "this version failed here" record stops mattering when this image (or a newer one) is the one running.
inline bool failureRecordObsolete(const Version &running, const Version &failed) { return compareVersions(running, failed) >= 0; }

// ---- the version an image says it is ---------------------------------------------------------------------------------------
// The app description inside an image (esp_app_desc_t) carries the core's build id here, not our version, so the firmware embeds its own
// marker: OTA_VERSION_TAG_PREFIX, FIRMWARE_VERSION and an end byte (ota.cpp). After an image has been written the updater reads the slot
// back and finds the marker, so a build that was published under the wrong version can never be installed: it would write the wrong
// version into the record of a failed update.
// CAUTION: the marker holds control bytes (0x01, 0x02, and 0x03 at its end). It is for reading back from flash only: never hand a pointer into it
// (or a string built from it) to JSON, a header or a log. Use FIRMWARE_VERSION for that. (ota::version() once returned a pointer into the
// marker and /status said "0.10.0\x03", which is not valid JSON.)
#define OTA_VERSION_TAG_PREFIX "\x01" "LEDMATRIX-FW-VERSION" "\x02"
static const char VERSION_TAG_PREFIX[] = OTA_VERSION_TAG_PREFIX;
static const size_t VERSION_TAG_PREFIX_LEN = sizeof(VERSION_TAG_PREFIX) - 1;
static const uint8_t VERSION_TAG_END = 0x03;

// Finds the first marker followed by a valid version and the end byte in a stream of bytes, in pieces of any size (a marker may be cut
// between two pieces). An occurrence that is not followed by a version (the prefix as a constant in the code that looks for it) is skipped.
class VersionTagScanner {
 public:
  void feed(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n && !found_; i++) {
      uint8_t b = p[i];
      if (inVersion_) {
        if (b == VERSION_TAG_END) {
          if (len_ > 0 && parseVersion(text_, len_, v_)) {
            text_[len_] = 0;
            found_ = true;
            return;
          }
          inVersion_ = false;
          match_ = 0;
        } else if (((b >= '0' && b <= '9') || b == '.') && len_ < VERSION_TEXT_MAX) {
          text_[len_++] = (char)b;
        } else {
          inVersion_ = false;  // not a version: look for a marker again, starting with this byte
          match_ = 0;
          step(b);
        }
        continue;
      }
      step(b);
    }
  }
  bool found() const { return found_; }
  const char *text() const { return found_ ? text_ : ""; }   // "0.10.1"
  Version version() const { return v_; }

 private:
  size_t match_ = 0;
  bool inVersion_ = false;
  char text_[VERSION_TEXT_MAX + 2] = "";
  size_t len_ = 0;
  bool found_ = false;
  Version v_;

  void step(uint8_t b) {  // the prefix starts with a byte that appears nowhere else in it, so a mismatch only ever restarts at 0 or 1
    if (b == (uint8_t)VERSION_TAG_PREFIX[match_]) {
      match_++;
      if (match_ == VERSION_TAG_PREFIX_LEN) {
        inVersion_ = true;
        len_ = 0;
        match_ = 0;
      }
    } else {
      match_ = b == (uint8_t)VERSION_TAG_PREFIX[0] ? 1 : 0;
    }
  }
};

// ---- chunked transfer coding -------------------------------------------------------------------------------------------------
// A chunk-size line is one to eight hex digits, then optionally ";" and an extension, then the line end. Anything else is an error:
// a garbage line must never be read as "0", the last chunk.
inline bool parseChunkSize(const char *line, size_t len, size_t &out) {
  if (!line) return false;
  size_t i = 0;
  uint64_t v = 0;
  size_t digits = 0;
  for (; i < len; i++) {
    char c = line[i];
    uint8_t d;
    if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
    else break;
    if (++digits > 8) return false;
    v = v * 16 + d;
  }
  if (digits == 0) return false;
  for (; i < len; i++) {  // after the digits: an extension (";..."), or white space and the line end
    char c = line[i];
    if (c == ';') break;
    if (c != '\r' && c != '\n' && c != ' ' && c != '\t') return false;
  }
  out = (size_t)v;
  return true;
}
// The line that ends a chunk's data is empty (the "\r" is all that is left of it).
inline bool isChunkEnd(const char *line, size_t len) { return len == 0 || (len == 1 && line[0] == '\r'); }

// ---- small things the portal's answers need --------------------------------------------------------------------------------
// How long ago, in whole seconds, from two readings of a clock that does not wrap (esp_timer, 64 bit): never negative, never more than
// a 32-bit number (a reading taken a moment after the other, in another task, is "0 seconds ago", not 4 billion).
inline uint32_t ageSeconds(int64_t nowS, int64_t thenS) {
  if (nowS <= thenS) return 0;
  int64_t d = nowS - thenS;
  return d > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)d;
}

// What a refused code call says (the portal shows `error` as it is).
static const char SENTENCE_CODE_WRONG[] = "That code is not right. Check the panel and try again.";
static const char SENTENCE_CODE_EXPIRED[] = "The code expired. Press Update to get a new one.";
inline void throttleSentence(uint32_t seconds, char *out, size_t cap) {
  snprintf(out, cap, "Too many wrong codes. Wait %u second%s, then press Update.", (unsigned)seconds, seconds == 1 ? "" : "s");
}
// A last resort so no refusal ever goes out without a sentence (an error the other words did not cover).
inline const char *refusalFallback(int http) {
  switch (http) {
    case 400: return "The request could not be understood.";
    case 403: return "The update was refused.";
    case 409: return "That is not possible right now.";
    case 413: return "The file is too big.";
    case 422: return "The firmware file was not accepted.";
    case 429: return "Too many tries. Wait a little and try again.";
    default: return "Something went wrong.";
  }
}

// ---- the panel while flash is written -----------------------------------------------------------------------------------------
// Erasing and writing flash stops the panel's scan interrupts for tens of milliseconds at a time (the cache is off), and a row that
// is lit when that happens stays lit: a flicker for the whole write. Measured on the real panel on 27 Nov 2026: steady (0.2 %
// variation of the picture's brightness) before and after, 20 to 40 times that for the 14 s of an update. No setting of the core
// changes it, so the render task switches the panel off cleanly (Protomatter stop(), outputs low) while an image is written and
// back on afterwards. The panel first says "Screen off" for HOLD_MS, so nobody takes the dark for a fault.
// The render task is the only caller of step(); `want` is the other tasks' volatile flag "an image is being written or checked".
class QuietGate {
 public:
  enum class Act : uint8_t { None, Blank, Light };
  static constexpr uint32_t HOLD_MS = 2500;  // how long the notice shows before the panel goes dark
  Act step(bool want, uint32_t nowMs) {
    if (want) {
      if (!wanting_) {
        wanting_ = true;
        since_ = nowMs;
      }
      if (!blank_ && (uint32_t)(nowMs - since_) >= HOLD_MS) {  // unsigned difference: the millisecond counter may wrap
        blank_ = true;
        return Act::Blank;
      }
      return Act::None;
    }
    wanting_ = false;
    if (blank_) {
      blank_ = false;
      return Act::Light;
    }
    return Act::None;
  }
  bool notice() const { return wanting_ && !blank_; }  // the "Screen off" line is on the panel (the panel is still lit)
  bool blank() const { return blank_; }                // the panel is off: the installer may write

 private:
  bool wanting_ = false;
  bool blank_ = false;
  uint32_t since_ = 0;
};

}  // namespace otalogic
