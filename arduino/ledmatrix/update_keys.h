// arduino/ledmatrix/update_keys.h: the public keys that updates may be signed with. Public by nature; edit only to rotate (docs).
// UPDATE_KEY_0 is the current key: its private half is the CI secret UPDATE_SIGNING_KEY (key id eb6a67ad).
// UPDATE_KEY_1 is the spare ("next"): its private half is offline and never in CI (key id dfcb5742).
// Own builds: replace both with your own public keys and sign with your own private key (shared/manifest-signing.md).
#pragma once
#ifndef UPDATE_REQUIRE_SIGNATURE   // 1 (the default): an update is installed only when its manifest carries a valid signature by a key below
#define UPDATE_REQUIRE_SIGNATURE 1 // 0: development builds only, no signature is looked at (the portal says so)
#endif
#define UPDATE_KEY_COUNT 2
#define UPDATE_KEY_0 "04051ce48f76a8a6afebe0aa2498cca4a0d56b09817fdd76a9d443a870bb1df4546717dd1557ffb214c959a8c926040956bc1b0cf17e97082aef268922cbc5d2fd"
#define UPDATE_KEY_1 "0480907918febed7c22cf3dcbfa7cbe21501e3c2cba2ee524da7a6991f564375f55f6b44f3ca256ec6605415ac2a7b1e523b7707551fd0c2cc2f71c76591be157b"
