// This board's identity (tickets 37 and 39): the device ID (the chip's factory MAC), the identity record in the read-only `ident`
// partition, and the signer that proves the ID to the owner's service: the saved software key, or, once the key is burned into the
// chip (tools/burn_key), the chip's HMAC unit. The rules and the formats are in deviceauth.h (pure logic,
// tested on the laptop) and shared/device-auth.md. This file is the part that touches the chip.
//
// begin() reads the record once. After that everything here is read-only except the clock offset, which has its own tiny lock,
// so any task may call any of it. Nothing here prints, returns or logs the key.
#pragma once
#include <Arduino.h>

#include "deviceauth.h"

namespace identity {

void begin();  // once, from setup(), before the net and data tasks start

const char *deviceId();                 // this chip's factory MAC, 12 lowercase hex characters ("" if it could not be read)
devauth::Identity status();             // Ok, or why the board cannot sign
const char *statusName();               // for /status: "ok", "missing", "invalid", "other_board", "hardware_key"
const char *rawProblem();               // the PROBLEM_RAW_* text (problems.h) for a status that is not Ok, "" when it is Ok
const char *signerName();               // for /status: "software", "hardware", or "none" (the board cannot sign)
const char *signerCheckName();          // for /status: "not_run", "match" or "mismatch" (the chip's key against the saved one)
const char *notice();                   // a PROBLEM_RAW_* text for a condition the owner should know about although signing works
                                        // (today: the chip's key and the saved key disagree), "" when there is none

// Writes the three signature header lines (CRLF-terminated, see devauth::buildHeaders) for a GET of `target` on `hostLower`
// (devauth::lowerHost) into hdr. The timestamp is the board's clock plus the stored clock offset. False, with hdr empty, when
// the board cannot sign (status() is not Ok) or the request is not valid. Takes no lock across anything slow.
bool signGet(const char *hostLower, const char *target, char *hdr, size_t cap);

// The service refused a timestamp (clock_skew) and said what time it is: remember the difference to this board's clock, for the
// next signatures only. The system clock is not touched.
void noteServerTime(int64_t serverTime);
void clearClockOffset();  // NTP set the system clock: the stored difference is stale

}  // namespace identity
