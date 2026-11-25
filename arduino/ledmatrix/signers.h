// The two signers behind devauth::Signer (deviceauth.h), for the board: the same HMAC-SHA256 computed in software (mbedtls) with a
// key held in RAM, or by the chip's HMAC unit with the key burned into an eFuse key block, which no code can read. Header only,
// so the firmware (identity.cpp) and the one-off burn sketch (tools/burn_key) use exactly the same code. Nothing here prints.
#pragma once
#include <esp_efuse.h>
#include <esp_hmac.h>
#include <mbedtls/md.h>
#include <string.h>

#include "deviceauth.h"

namespace signers {

// THE one place that says which eFuse key block holds the board's HMAC key. The firmware (identity.cpp), the HardwareSigner and the
// burn sketch (tools/burn_key) all use these two, and the assert keeps them pointing at the same block: the HMAC unit numbers its
// keys 0 to 5 like the key blocks KEY0 to KEY5. To use another block, change both here (and the texts in tools/burn_key/burn_key.ino).
static constexpr esp_efuse_block_t KEY_BLOCK = EFUSE_BLK_KEY5;
static constexpr hmac_key_id_t KEY_ID = HMAC_KEY5;
static_assert((int)KEY_ID == (int)KEY_BLOCK - (int)EFUSE_BLK_KEY0, "the HMAC key id and the eFuse key block must be the same key");

// HMAC-SHA256 under a 32-byte key that the owner of this object keeps alive and wipes. mbedtls_md_hmac wipes its own state.
class SoftwareSigner : public devauth::Signer {
 public:
  explicit SoftwareSigner(const uint8_t *key) : key_(key) {}
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info && key_ && mbedtls_md_hmac(info, key_, devauth::KEY_LEN, msg, len, out) == 0;
  }

 private:
  const uint8_t *key_;
};

// The chip's HMAC unit (esp_hmac_calculate, "upstream" mode) with the key in an eFuse key block whose purpose is HMAC_UP. The unit
// reads the message and writes the result as 32-bit words, so both go through 4-byte aligned buffers here, whatever the caller
// passes. The buffers are on the stack (this is called from the data task and from the portal's task): no shared state.
// Standard HMAC-SHA256, so it gives the same bytes as SoftwareSigner for the same key (the tests prove it on the board).
class HardwareSigner : public devauth::Signer {
 public:
  explicit HardwareSigner(hmac_key_id_t keyId = KEY_ID) : keyId_(keyId) {}
  bool sign(const uint8_t *msg, size_t len, uint8_t out[devauth::SIG_LEN]) override {
    alignas(4) uint8_t in[devauth::CANONICAL_CAP];
    alignas(4) uint8_t res[devauth::SIG_LEN];
    if (len > sizeof(in) || (len && !msg)) return false;
    if (len) memcpy(in, msg, len);
    bool ok = esp_hmac_calculate(keyId_, in, len, res) == ESP_OK;
    if (ok) memcpy(out, res, devauth::SIG_LEN);
    devauth::secureZero(res, sizeof(res));
    return ok;
  }

 private:
  hmac_key_id_t keyId_;
};

}  // namespace signers
