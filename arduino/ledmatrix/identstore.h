// Reads this board's identity from the chip and from the read-only `ident` partition (deviceauth.h has the rules and the record
// format). Header only: the firmware (identity.cpp) and the one-off burn sketch (tools/burn_key) read the record with the same code.
// Nothing here prints, and the key never leaves the IdentRecord it is parsed into.
#pragma once
#include <esp_mac.h>
#include <esp_partition.h>

#include "deviceauth.h"

namespace identstore {

// The chip's factory MAC as 12 lowercase hex characters. False (out is "") when it cannot be read.
inline bool readChipId(char out[devauth::DEVICE_ID_LEN + 1]) {
  uint8_t mac[6];
  out[0] = 0;
  if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
  devauth::formatDeviceId(mac, out);
  return true;
}

// The record from the ident partition: two 4 KB sectors, each a full copy. Only the record's own bytes are read. A sector that is
// not there is nullptr (Blank); one that could not be read is given with length 0 (Short, so Invalid). The result is the one of
// devauth::loadIdentity; `rec` holds the record only when it is Ok. The temporary copy of the key is wiped before returning.
inline devauth::Identity load(const char *chipId, bool haveHardwareSigner, devauth::IdentRecord &rec) {
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
  devauth::Identity st = devauth::loadIdentity(p[0], n[0], p[1], n[1], chipId, haveHardwareSigner, rec);
  devauth::secureZero(sector, sizeof(sector));
  return st;
}

}  // namespace identstore
