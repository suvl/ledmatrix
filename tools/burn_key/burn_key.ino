// One-off sketch: burns THIS board's device key into the chip's eFuse BLOCK_KEY5 with purpose HMAC_UP, for good (tickets 37, 39).
// Read tools/burn_key/README.md first. Burning is irreversible: the key can never be read, changed or removed afterwards.
//
// The key comes from the board's own `ident` partition (the record tools/provision.py wrote), so no key ever touches a host file,
// a command line or the network. Nothing in here prints a key.
//
// THE DEFAULT IS A DRY RUN. It runs every check, prints what it would do, and writes nothing. A real burn needs all of:
//   1. a build with BOTH  -DBURN_KEY5_FOR_REAL=1  and  -DBURN_DEVICE_ID="<this board's 12 hex characters>"  (the ID must be
//      the chip's own factory MAC, so a build is tied to one board; a build for another board refuses),
//   2. every check passing (record valid, key not yet burned, BLOCK_KEY5 unused, no HMAC key anywhere, software HMAC works),
//   3. the owner typing  BURN <the same 12 hex characters>  into the serial monitor within 3 minutes of the checks passing.
// Without all three nothing is written. A reset starts over from the checks, and the checks fail once the key block is used.
//
// The part that writes exists only inside "#if BURN_KEY5_FOR_REAL" blocks (tools/test_burn_key.py checks that no other code
// touches an eFuse for writing), so a dry-run build does not contain it.
#include <Arduino.h>
#include <esp_efuse.h>
#include <mbedtls/md.h>

#include "../../arduino/ledmatrix/identstore.h"
#include "../../arduino/ledmatrix/signers.h"

#ifndef BURN_KEY5_FOR_REAL
#define BURN_KEY5_FOR_REAL 0
#endif
#if BURN_KEY5_FOR_REAL != 0 && BURN_KEY5_FOR_REAL != 1
#error "BURN_KEY5_FOR_REAL must be 0 (dry run, the default) or 1"
#endif

#ifdef BURN_DEVICE_ID
constexpr bool burnIdIsValid(const char *s) {  // exactly 12 lowercase hex characters
  for (int i = 0; i < 12; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;  // also stops at the end of a short text
  return s[12] == 0;
}
static_assert(burnIdIsValid(BURN_DEVICE_ID), "BURN_DEVICE_ID must be exactly 12 lowercase hex characters, in quotes");
#elif BURN_KEY5_FOR_REAL
#error "A real burn is tied to one board: also pass -DBURN_DEVICE_ID=\"<the board's 12 hex characters>\""
#endif

namespace {

const esp_efuse_block_t BLOCK = signers::KEY_BLOCK;  // the one constant for the key block (signers.h)
// The texts below say BLOCK_KEY5 and KEY5. If signers.h ever points at another block, this stops the build until they are updated.
static_assert(signers::KEY_BLOCK == EFUSE_BLK_KEY5, "update the BLOCK_KEY5 / KEY5 texts in this sketch (and the README) for the new block");
const esp_efuse_purpose_t PURPOSE = ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
const uint32_t REPEAT_EVERY_MS = 15UL * 1000;

char chipId[devauth::DEVICE_ID_LEN + 1] = "";
devauth::IdentRecord rec;  // the record from the ident partition; its key is wiped before this sketch goes idle

enum class State : uint8_t { Stopped, DryRunDone, WaitConfirm, TimedOut, Burned, BurnFailed };
State state = State::Stopped;
char summary[256] = "";  // the last line, repeated now and then so a monitor opened late still sees how it ended
uint32_t lastRepeat = 0;
#if BURN_KEY5_FOR_REAL
const uint32_t CONFIRM_WITHIN_MS = 3UL * 60 * 1000;
uint32_t waitSince = 0;
char line[40];
size_t lineLen = 0;
#endif

// One check: prints its result, and on a failure the one plain sentence that says why and stops everything.
bool check(bool ok, const char *what, const char *ifFailed) {
  Serial.printf("BURN check %-44s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok) {
    snprintf(summary, sizeof(summary), "BURN STOPPED: %s Nothing was written.", ifFailed);
    Serial.println(summary);
  }
  return ok;
}

const char *purposeName(esp_efuse_purpose_t p) {
  switch (p) {
    case ESP_EFUSE_KEY_PURPOSE_USER: return "USER";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_UP: return "HMAC_UP";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_ALL: return "HMAC_DOWN_ALL";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_JTAG: return "HMAC_DOWN_JTAG";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE: return "HMAC_DOWN_DIGITAL_SIGNATURE";
    case ESP_EFUSE_KEY_PURPOSE_XTS_AES_256_KEY_1: return "XTS_AES_256_KEY_1";
    case ESP_EFUSE_KEY_PURPOSE_XTS_AES_256_KEY_2: return "XTS_AES_256_KEY_2";
    case ESP_EFUSE_KEY_PURPOSE_XTS_AES_128_KEY: return "XTS_AES_128_KEY";
    case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST0: return "SECURE_BOOT_DIGEST0";
    case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST1: return "SECURE_BOOT_DIGEST1";
    case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST2: return "SECURE_BOOT_DIGEST2";
    default: return "other";
  }
}

// The six key blocks as they are now. Read-only: the purpose and the protection bits, never the key bytes.
void printKeyBlocks() {
  for (int i = 0; i < 6; i++) {
    esp_efuse_block_t b = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + i);
    Serial.printf("BURN key block KEY%d: purpose %-9s unused %-3s read-protected %-3s write-protected %s\n", i,
                  purposeName(esp_efuse_get_key_purpose(b)), esp_efuse_key_block_unused(b) ? "yes" : "no",
                  esp_efuse_get_key_dis_read(b) ? "yes" : "no", esp_efuse_get_key_dis_write(b) ? "yes" : "no");
  }
}

bool allSame(const uint8_t *p, size_t n, uint8_t v) {
  for (size_t i = 0; i < n; i++)
    if (p[i] != v) return false;
  return true;
}

// The software HMAC: the RFC 4231 test case 2 answer, and the record's own key giving the same answer twice (one-shot and streaming).
bool softwareSelfTest() {
  static const uint8_t jefe[4] = {'J', 'e', 'f', 'e'};
  static const char text[] = "what do ya want for nothing?";
  static const uint8_t want[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
                                   0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  uint8_t out[32];
  if (mbedtls_md_hmac(info, jefe, sizeof(jefe), (const uint8_t *)text, strlen(text), out) != 0 || memcmp(out, want, 32) != 0) return false;
  signers::SoftwareSigner soft(rec.key);
  uint8_t a[32], b[32];
  alignas(4) uint8_t msg[devauth::VERIFY_MAX];
  devauth::verifyMessage(msg, 200);
  bool ok = soft.sign(msg, 200, a);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  ok = ok && mbedtls_md_setup(&ctx, info, 1) == 0 && mbedtls_md_hmac_starts(&ctx, rec.key, devauth::KEY_LEN) == 0 &&
       mbedtls_md_hmac_update(&ctx, msg, 200) == 0 && mbedtls_md_hmac_finish(&ctx, b) == 0 && memcmp(a, b, 32) == 0;
  mbedtls_md_free(&ctx);
  devauth::secureZero(a, sizeof(a));
  devauth::secureZero(b, sizeof(b));
  return ok;
}

// Every check, in order. False as soon as one fails (it has printed why).
bool runChecks() {
  printKeyBlocks();
  if (!check(identstore::readChipId(chipId), "the chip's own address can be read", "The chip's address could not be read.")) return false;
  Serial.printf("BURN chip address %s\n", chipId);
#ifdef BURN_DEVICE_ID
  if (!check(strcmp(chipId, BURN_DEVICE_ID) == 0, "this build is for this board", "This build was made for another board (its ID does not match this chip's address). Build again with this board's ID."))
    return false;
#endif
  devauth::Identity id = identstore::load(chipId, true, rec);
  if (!check(id != devauth::Identity::Missing, "the ident partition holds a record", "The ident partition has no identity record. Run tools/provision.py first.")) return false;
  if (!check(id != devauth::Identity::OtherBoard, "the record is for this chip", "The identity record is for another board.")) return false;
  if (!check(id == devauth::Identity::Ok, "the record is valid (magic, version, CRC, ID)", "The identity record is not valid. Run tools/provision.py again.")) return false;
  if (!check(!rec.burned(), "the record does not say \"burned\" yet", "The record already says the key is burned into the chip; there is nothing left to burn.")) return false;
  if (!check(!allSame(rec.key, 32, 0x00) && !allSame(rec.key, 32, 0xFF), "the record's key is a real key (not all 0x00 or 0xFF)", "The key in the record is all zeros or all ones; it was not provisioned with a key.")) return false;
  if (!check(softwareSelfTest(), "the software HMAC works with the record's key", "The software HMAC self-test failed.")) return false;
  if (!check(esp_efuse_key_block_unused(BLOCK), "BLOCK_KEY5 is unused", "BLOCK_KEY5 is already in use (it has content, protection or a purpose), so nothing can be burned there.")) return false;
  if (!check(!esp_efuse_get_keypurpose_dis_write(BLOCK), "the purpose of BLOCK_KEY5 can still be set", "The purpose field of BLOCK_KEY5 is write-protected, so it can no longer become HMAC_UP.")) return false;
  esp_efuse_block_t found;
  if (!check(!esp_efuse_find_purpose(PURPOSE, &found), "no block holds an HMAC key yet", "A key block already has the HMAC purpose; burning a second one would waste a block.")) return false;
  if (!check(esp_efuse_count_unused_key_blocks() >= 1, "at least one unused key block", "No unused key block is left.")) return false;
  return true;
}

#if BURN_KEY5_FOR_REAL
// THE WRITE. esp_efuse_write_key burns the key, the purpose and the protection bits together in one eFuse batch (the header says:
// "The burn of a key, protection bits, and a purpose happens in batch mode" and "This API also enables the read protection efuse bit
// for certain key blocks like XTS-AES, HMAC, ECDSA etc."). The write protection is checked afterwards, and added if it is missing.
bool burnAndVerify() {
  Serial.println("BURN writing BLOCK_KEY5 now, do not remove power");
  Serial.flush();
  alignas(4) uint8_t key[devauth::KEY_LEN];
  memcpy(key, rec.key, sizeof(key));
  esp_err_t err = esp_efuse_write_key(BLOCK, PURPOSE, key, sizeof(key));
  devauth::secureZero(key, sizeof(key));
  if (err != ESP_OK) {
    snprintf(summary, sizeof(summary), "BURN FAILED: the write returned %s. BLOCK_KEY5 may be partly written: see the state above and the README.", esp_err_to_name(err));
    Serial.println(summary);
    printKeyBlocks();
    return false;
  }
  bool purposeOk = esp_efuse_get_key_purpose(BLOCK) == PURPOSE;
  Serial.printf("BURN readback: purpose HMAC_UP %s\n", purposeOk ? "ok" : "NOT SET");
  // The protections must all be there. They normally are (esp_efuse_write_key sets them); a missing one is added, key first.
  if (purposeOk && !esp_efuse_get_key_dis_read(BLOCK)) { Serial.println("BURN adding the missing read protection"); esp_efuse_set_key_dis_read(BLOCK); }
  if (purposeOk && !esp_efuse_get_key_dis_write(BLOCK)) { Serial.println("BURN adding the missing write protection"); esp_efuse_set_key_dis_write(BLOCK); }
  if (purposeOk && !esp_efuse_get_keypurpose_dis_write(BLOCK)) { Serial.println("BURN adding the missing purpose write protection"); esp_efuse_set_keypurpose_dis_write(BLOCK); }
  bool readProt = esp_efuse_get_key_dis_read(BLOCK), writeProt = esp_efuse_get_key_dis_write(BLOCK), purposeProt = esp_efuse_get_keypurpose_dis_write(BLOCK);
  esp_efuse_block_t where = EFUSE_BLK_KEY0;
  bool findOk = esp_efuse_find_purpose(PURPOSE, &where) && where == BLOCK;
  Serial.printf("BURN readback: read-protected %s, write-protected %s, purpose write-protected %s, HMAC_UP found in KEY5 %s\n", readProt ? "yes" : "NO",
                writeProt ? "yes" : "NO", purposeProt ? "yes" : "NO", findOk ? "yes" : "NO");
  if (!(purposeOk && readProt && writeProt && purposeProt && findOk)) {
    snprintf(summary, sizeof(summary), "BURN FAILED: BLOCK_KEY5 does not read back as an HMAC key with read and write protection. Do not use it: see the README.");
    Serial.println(summary);
    return false;
  }
  // The chip must give the same answers as the saved key, for every message length the hardware unit treats differently.
  signers::SoftwareSigner soft(rec.key);
  signers::HardwareSigner hard(signers::KEY_ID);
  if (!devauth::signersAgree(soft, hard)) {
    snprintf(summary, sizeof(summary), "BURN FAILED: the chip's HMAC gives different answers than the saved key. Reset the board and flash the normal firmware: it repeats the comparison at boot and keeps the saved key if they differ.");
    Serial.println(summary);
    return false;
  }
  snprintf(summary, sizeof(summary), "BURN verified");
  Serial.println(summary);
  return true;
}
#endif

void nextSteps() {
  Serial.println("BURN next steps (details: tools/burn_key/README.md):");
  Serial.println("BURN   1. flash the normal firmware (arduino-cli upload of arduino/ledmatrix); the ident partition is kept");
  Serial.println("BURN   2. open http://<the board>/status : \"signer\" must be \"hardware\" and \"signer_check\" \"match\"");
  Serial.println("BURN   3. python3 -I tools/provision.py --burned --from-board <the board's host> --out <file outside the repository> --flash --port <port> --input-dir <build dir>");
  Serial.println("BURN      (--from-board checks /status first: it refuses unless the board signs with the chip. The record is written without the software key.)");
}

void finish(State s) {
  state = s;
  rec.wipe();  // the key is not needed any more
  lastRepeat = millis();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 20000; delay(10)) {}  // USB: wait for a serial monitor
  delay(500);
  Serial.println();
#if BURN_KEY5_FOR_REAL
  Serial.println("BURN burn_key: REAL BURN BUILD. It writes BLOCK_KEY5 only after every check passes AND you type the confirmation.");
#else
  Serial.println("BURN burn_key: DRY RUN. Every check runs, nothing is ever written.");
#endif
  if (!runChecks()) {
    finish(State::Stopped);
    return;
  }
#if BURN_KEY5_FOR_REAL
  Serial.println("BURN all checks passed.");
  Serial.printf("BURN TO BURN, type exactly:  BURN %s  and press Enter, within 3 minutes. Anything else, or a reset, burns nothing.\n", chipId);
  while (Serial.available()) Serial.read();  // only text typed after this prompt counts, never something typed before the results were shown
  state = State::WaitConfirm;
  waitSince = lastRepeat = millis();
#else
  snprintf(summary, sizeof(summary), "BURN DRY RUN done: every check passed. Nothing was written.");
  Serial.println(summary);
  Serial.println("BURN a real burn would now write this board's key from the ident record into BLOCK_KEY5 with purpose HMAC_UP,");
  Serial.println("BURN and turn on read protection and write protection for it, all in one eFuse write. It cannot be undone.");
  Serial.printf("BURN to do it, build again with  -DBURN_KEY5_FOR_REAL=1 -DBURN_DEVICE_ID=\"%s\"  (see the README), then type the confirmation.\n", chipId);
  finish(State::DryRunDone);
#endif
}

void loop() {
#if BURN_KEY5_FOR_REAL
  if (state == State::WaitConfirm) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r' || c == '\n') {
        line[lineLen] = 0;
        char want[24];
        snprintf(want, sizeof(want), "BURN %s", chipId);
        bool confirmed = lineLen > 0 && strcmp(line, want) == 0;
        if (lineLen > 0 && !confirmed) Serial.printf("BURN not confirmed. Type exactly:  BURN %s\n", chipId);
        lineLen = 0;
        if (confirmed) {
          bool ok = burnAndVerify();
          if (ok) nextSteps();
          finish(ok ? State::Burned : State::BurnFailed);
          return;
        }
      } else if (lineLen < sizeof(line) - 1) {
        line[lineLen++] = c;
      }
    }
    if (millis() - waitSince > CONFIRM_WITHIN_MS) {
      snprintf(summary, sizeof(summary), "BURN no confirmation within 3 minutes: nothing was written. Reset the board to try again.");
      Serial.println(summary);
      finish(State::TimedOut);
    } else if (millis() - lastRepeat > REPEAT_EVERY_MS) {
      lastRepeat = millis();
      Serial.printf("BURN waiting for:  BURN %s\n", chipId);
    }
    delay(10);
    return;
  }
#endif
  if (millis() - lastRepeat > REPEAT_EVERY_MS) {
    lastRepeat = millis();
    Serial.println(summary);
    if (state == State::Burned) nextSteps();
  }
  delay(100);
}
