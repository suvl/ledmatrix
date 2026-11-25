#include "identity.h"

#include <esp_efuse.h>
#include <esp_timer.h>
#include <time.h>

#include "identstore.h"
#include "problems.h"
#include "signers.h"

namespace identity {
namespace {

char chipId[devauth::DEVICE_ID_LEN + 1] = "";
devauth::IdentRecord rec;  // the record while the status is Ok (its key only while the software signer is the one in use). Set once in begin().
devauth::Identity st = devauth::Identity::Missing;
devauth::SignerPlan plan;       // which signer, and what the check between the two said
devauth::Signer *active = nullptr;  // the signer in use: set once in begin(), read-only after

portMUX_TYPE clockMux = portMUX_INITIALIZER_UNLOCKED;  // guards `offset`: a few instructions at a time, never held across HTTPS
devauth::ClockOffset offset;

// Seconds since boot as a 64-bit count that never wraps (millis() / 1000 wraps after 49 days, and not at 2^32 seconds).
int64_t uptimeS() { return esp_timer_get_time() / 1000000; }

// This firmware has both signers: a record whose key is burned (flag bit 0) is signed for by the chip's HMAC unit.
const bool HAVE_HARDWARE_SIGNER = true;

signers::SoftwareSigner softSigner(rec.key);  // the key sits in the static record above
signers::HardwareSigner hardSigner(signers::KEY_ID);

// Is there an HMAC key in the key block? Its purpose is HMAC_UP and the block is not an unused one (all zero, unprotected, purpose USER).
bool hardwareKeyPresent() {
  return esp_efuse_get_key_purpose(signers::KEY_BLOCK) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP && !esp_efuse_key_block_unused(signers::KEY_BLOCK);
}

// Is that key fully protected: read protection (no software can read it back), write protection, and write protection of the
// purpose field? A burn that was interrupted can leave a key without all three; such a key is never used.
bool hardwareKeyProtected() {
  return esp_efuse_get_key_dis_read(signers::KEY_BLOCK) && esp_efuse_get_key_dis_write(signers::KEY_BLOCK) &&
         esp_efuse_get_keypurpose_dis_write(signers::KEY_BLOCK);
}

}  // namespace

void begin() {
  identstore::readChipId(chipId);
  st = identstore::load(chipId, HAVE_HARDWARE_SIGNER, rec);

  if (st == devauth::Identity::Ok) {
    bool hwKey = hardwareKeyPresent();
    bool hwProtected = hwKey && hardwareKeyProtected();
    bool agree = false;
    // The burn is done but the record still holds the software key: prove that the chip's key is the same key before using it.
    if (devauth::needsSignerCheck(rec.burned(), hwKey, hwProtected)) agree = devauth::signersAgree(softSigner, hardSigner);
    plan = devauth::chooseSigner(rec.burned(), hwKey, hwProtected, agree);
    switch (plan.kind) {
      case devauth::SignerKind::Hardware:
        active = &hardSigner;
        devauth::secureZero(rec.key, sizeof(rec.key));  // the chip signs now: the software key leaves RAM
        break;
      case devauth::SignerKind::Software:
        active = &softSigner;
        break;
      case devauth::SignerKind::None:
        st = plan.problem;  // the record says the key is in the chip, and the chip has none (or only an unprotected one)
        rec.wipe();
        break;
    }
  }

  Serial.printf("IDENT device %s record %s signer %s check %s\n", chipId[0] ? chipId : "unknown", devauth::identityName(st),
                devauth::signerKindName(plan.kind), devauth::signerCheckName(plan.check));
  if (plan.check == devauth::SignerCheck::Mismatch)
    Serial.println("IDENT the chip's key and the saved key give different answers: signing with the saved key only");
  if (plan.check == devauth::SignerCheck::Unprotected)
    Serial.println("IDENT the key in the chip is not fully protected: not using it");
}

const char *deviceId() { return chipId; }
devauth::Identity status() { return st; }
const char *statusName() { return devauth::identityName(st); }
const char *signerName() { return devauth::signerKindName(st == devauth::Identity::Ok ? plan.kind : devauth::SignerKind::None); }
const char *signerCheckName() { return devauth::signerCheckName(plan.check); }

const char *rawProblem() {
  switch (st) {
    case devauth::Identity::Ok: return "";
    case devauth::Identity::Missing: return PROBLEM_RAW_ID_MISSING;
    case devauth::Identity::OtherBoard: return PROBLEM_RAW_ID_OTHER_BOARD;
    case devauth::Identity::HardwareKey: return PROBLEM_RAW_ID_HARDWARE_KEY;
    case devauth::Identity::HardwareKeyUnprotected: return PROBLEM_RAW_ID_HARDWARE_UNPROTECTED;
    case devauth::Identity::Invalid: break;
  }
  return PROBLEM_RAW_ID_INVALID;
}

const char *notice() {
  switch (plan.check) {
    case devauth::SignerCheck::Mismatch: return PROBLEM_RAW_SIGNER_MISMATCH;
    case devauth::SignerCheck::Unprotected: return st == devauth::Identity::Ok ? PROBLEM_RAW_SIGNER_UNPROTECTED : "";  // otherwise signing fails and says why
    case devauth::SignerCheck::Match:
    case devauth::SignerCheck::NotRun: break;
  }
  return "";
}

bool signGet(const char *hostLower, const char *target, char *hdr, size_t cap) {
  if (cap) hdr[0] = 0;
  if (st != devauth::Identity::Ok || !active) return false;
  int64_t own = (int64_t)time(nullptr);
  int64_t up = uptimeS();
  portENTER_CRITICAL(&clockMux);
  int64_t t = offset.apply(own, up);
  portEXIT_CRITICAL(&clockMux);
  if (t < 0) t = 0;  // cannot be signed as a negative time; the service will say clock_skew
  size_t len = 0;
  return devauth::signRequest(*active, chipId, "GET", hostLower, target, (uint64_t)t, devauth::EMPTY_BODY_SHA256_HEX, hdr, cap, len);
}

void noteServerTime(int64_t serverTime) {
  int64_t own = (int64_t)time(nullptr);
  int64_t up = uptimeS();
  portENTER_CRITICAL(&clockMux);
  offset.store(serverTime, own, up);
  portEXIT_CRITICAL(&clockMux);
}

void clearClockOffset() {
  portENTER_CRITICAL(&clockMux);
  offset.clear();
  portEXIT_CRITICAL(&clockMux);
}

}  // namespace identity
