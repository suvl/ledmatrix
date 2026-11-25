#include "identity.h"

#include <esp_mac.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <mbedtls/md.h>
#include <time.h>

#include "problems.h"

namespace identity {
namespace {

char chipId[devauth::DEVICE_ID_LEN + 1] = "";
devauth::IdentRecord rec;  // holds the record only while status is Ok; wiped otherwise. Set once in begin(), read-only after.
devauth::Identity st = devauth::Identity::Missing;

portMUX_TYPE clockMux = portMUX_INITIALIZER_UNLOCKED;  // guards `offset`: a few instructions at a time, never held across HTTPS
devauth::ClockOffset offset;

// Seconds since boot as a 64-bit count that never wraps (millis() / 1000 wraps after 49 days, and not at 2^32 seconds).
int64_t uptimeS() { return esp_timer_get_time() / 1000000; }

// No hardware signer yet: the eFuse HMAC unit (esp_hmac_calculate, HMAC_KEY5) comes in the burn step and goes behind the same
// devauth::Signer interface. Until then a record whose key is burned (flag bit 0) is refused: the software key is gone from it.
const bool HAVE_HARDWARE_SIGNER = false;

// The software signer: HMAC-SHA256 under the key from the record. mbedtls_md_hmac wipes its own state when it is done.
class SoftSigner : public devauth::Signer {
 public:
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && mbedtls_md_hmac(info, rec.key, devauth::KEY_LEN, msg, len, out) == 0;
  }
} softSigner;

}  // namespace

void begin() {
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) == ESP_OK) devauth::formatDeviceId(mac, chipId);

  // The ident partition: two 4 KB sectors, each a full copy of the record (deviceauth.h). Only the record's own bytes are read.
  // A sector that is not there is nullptr (Blank); one that could not be read is given with length 0 (Short, so Invalid).
  alignas(4) uint8_t sector[2][devauth::IDENT_RECORD_LEN];
  const uint8_t *p[2] = {nullptr, nullptr};
  size_t n[2] = {0, 0};
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "ident");
  if (part) {
    for (int i = 0; i < 2; i++) {
      if (part->size < (size_t)(i + 1) * devauth::SECTOR_BYTES) continue;
      bool read = esp_partition_read(part, (size_t)i * devauth::SECTOR_BYTES, sector[i], sizeof(sector[i])) == ESP_OK;
      p[i] = sector[i];
      n[i] = read ? sizeof(sector[i]) : 0;
    }
  }
  st = devauth::loadIdentity(p[0], n[0], p[1], n[1], chipId, HAVE_HARDWARE_SIGNER, rec);
  devauth::secureZero(sector, sizeof(sector));  // the record's key was in there

  Serial.printf("IDENT device %s record %s\n", chipId[0] ? chipId : "unknown", devauth::identityName(st));
}

const char *deviceId() { return chipId; }
devauth::Identity status() { return st; }
const char *statusName() { return devauth::identityName(st); }

const char *rawProblem() {
  switch (st) {
    case devauth::Identity::Ok: return "";
    case devauth::Identity::Missing: return PROBLEM_RAW_ID_MISSING;
    case devauth::Identity::OtherBoard: return PROBLEM_RAW_ID_OTHER_BOARD;
    case devauth::Identity::HardwareKey: return PROBLEM_RAW_ID_HARDWARE_KEY;
    case devauth::Identity::Invalid: break;
  }
  return PROBLEM_RAW_ID_INVALID;
}

bool signGet(const char *hostLower, const char *target, char *hdr, size_t cap) {
  if (cap) hdr[0] = 0;
  if (st != devauth::Identity::Ok) return false;
  int64_t own = (int64_t)time(nullptr);
  int64_t up = uptimeS();
  portENTER_CRITICAL(&clockMux);
  int64_t t = offset.apply(own, up);
  portEXIT_CRITICAL(&clockMux);
  if (t < 0) t = 0;  // cannot be signed as a negative time; the service will say clock_skew
  size_t len = 0;
  return devauth::signRequest(softSigner, chipId, "GET", hostLower, target, (uint64_t)t, devauth::EMPTY_BODY_SHA256_HEX, hdr, cap, len);
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
