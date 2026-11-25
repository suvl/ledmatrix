// Problems the device ran into (ticket 23, slice 8): the list the portal shows, the words used for each reason, and the
// rule for the panel message that points the owner to the portal. Pure logic with no Arduino headers, so it is tested on
// the laptop (tools/test_problems.cpp). Everything here runs under live.cpp's lock and allocates nothing.
//
// Two separate things are kept, both per kind of data:
//   ProblemLog    the last 10 distinct problems, newest first. The same problem again (same kind, same words) does not add a
//                 line: it moves to the top and its counter goes up, so a failure that repeats every 15 s cannot push an
//                 older, different problem out of the list. Kept in memory; empty after a restart.
//   ProblemWatch  how long each kind has been failing in a row. A kind counts for the panel message only after it has
//                 failed for one hour with no success in between, AND it is still failing now (a failure within the last
//                 10 minutes). Without the second part, one logo that failed an hour ago and was never asked for again
//                 would keep the message up for ever.
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The panel message waits this long. A build for a bench test may shorten it (-DPROBLEM_MESSAGE_AFTER_MS=120000);
// the unit test checks the real value.
#ifndef PROBLEM_MESSAGE_AFTER_MS
#define PROBLEM_MESSAGE_AFTER_MS (60UL * 60 * 1000)
#endif
static const uint32_t PROBLEM_FRESH_MS = 10UL * 60 * 1000;  // a failure this recent means "still failing"
static const uint32_t PROBLEM_MESSAGE_PERIOD_MS = 60UL * 1000;
static const uint32_t PROBLEM_MESSAGE_SHOW_MS = 5UL * 1000;

enum class ProblemKind : uint8_t { Flights, Weather, Headlines, Logo, Clock, Update, Identity, Count };
static const int PROBLEM_KINDS = (int)ProblemKind::Count;

inline const char *problemKindName(ProblemKind k) {
  switch (k) {
    case ProblemKind::Flights: return "flights";
    case ProblemKind::Weather: return "weather";
    case ProblemKind::Headlines: return "headlines";
    case ProblemKind::Logo: return "logo";
    case ProblemKind::Clock: return "clock";
    case ProblemKind::Update: return "update check";
    case ProblemKind::Identity: return "identity";
    default: return "?";
  }
}

// ---- the reason in plain words ---------------------------------------------------------------------------------------
// The firmware's own raw texts. live.cpp reports exactly these and problemWords recognises exactly these, so the two cannot
// drift apart (the unit test checks what each one becomes).
#define PROBLEM_RAW_NO_ANSWER "no answer: the connection closed or timed out"  // no HTTP status line arrived
#define PROBLEM_RAW_UNEXPECTED "unexpected answer"                               // the body could not be read
#define PROBLEM_RAW_BUSY "busy with another request"                            // the one-request-at-a-time lock was not freed in time
#define PROBLEM_RAW_NO_ADDRESS "no address found for the host name"             // the name lookup returned nothing
// Device authentication (tickets 37 and 39, shared/device-auth.md). The service's 401 reasons, as live.cpp reports them:
#define PROBLEM_RAW_AUTH_MISSING "refused: signature missing (HTTP 401)"
#define PROBLEM_RAW_AUTH_MALFORMED "refused: signature malformed (HTTP 401)"
#define PROBLEM_RAW_AUTH_BAD_SIGNATURE "refused: bad signature (HTTP 401)"
#define PROBLEM_RAW_AUTH_REVOKED "refused: device revoked (HTTP 401)"
#define PROBLEM_RAW_AUTH_CLOCK_SKEW "refused: clock skew (HTTP 401)"
// ...and the board's own identity record (identity.cpp), which is checked before anything is sent:
#define PROBLEM_RAW_ID_MISSING "no identity record"
#define PROBLEM_RAW_ID_INVALID "identity record is not valid"
#define PROBLEM_RAW_ID_OTHER_BOARD "identity record is for another board"
#define PROBLEM_RAW_ID_HARDWARE_KEY "record says the key is burned into the chip but the chip has no key"
#define PROBLEM_RAW_ID_HARDWARE_UNPROTECTED "the burned key is not fully protected"
#define PROBLEM_RAW_SIGNER_MISMATCH "the chip's key and the saved key give different answers"  // signing works with the saved key
#define PROBLEM_RAW_SIGNER_UNPROTECTED "the key in the chip is not fully protected"              // signing works with the saved key
#define PROBLEM_RAW_SIGN_FAILED "could not sign the request"  // the request itself was not valid, or the signer failed
// Firmware updates (ticket 33, shared/firmware-update.md, otalogic.h's Fail). A reason may follow a macro after ": " (a detail for
// the list: the HTTP status, the manifest rule that failed, the ESP-IDF error name); the sentence is chosen by the macro's start.
#define PROBLEM_RAW_UPDATE_NO_FIRMWARE "no firmware is published (HTTP 404)"                 // the manifest route answered 404
#define PROBLEM_RAW_UPDATE_MANIFEST_INVALID "firmware manifest is not valid"                 // the manifest broke a rule of the contract
#define PROBLEM_RAW_UPDATE_NO_SERVICE "no service address saved for the update check"        // a check was asked for with no address
#define PROBLEM_RAW_UPDATE_NO_CLOCK "the clock is not set yet, needed for the update check"  // secure connections need the time
#define PROBLEM_RAW_UPDATE_DOWNLOAD_FAILED "firmware download failed"                        // the transfer broke off, or the server said no
#define PROBLEM_RAW_UPDATE_UPLOAD_INCOMPLETE "firmware upload did not finish"                // the browser stopped sending the file
#define PROBLEM_RAW_UPDATE_WRONG_SIZE "firmware size does not match the manifest"            // not the number of bytes the manifest said
#define PROBLEM_RAW_UPDATE_WRONG_CHECKSUM "firmware checksum does not match the manifest"    // the SHA-256 differs
#define PROBLEM_RAW_UPDATE_WRONG_VERSION "firmware version does not match the manifest"      // the image says it is another version
#define PROBLEM_RAW_UPDATE_BAD_IMAGE "firmware image is not valid for this chip"             // the ESP-IDF image validation said no
#define PROBLEM_RAW_UPDATE_TOO_BIG "firmware image does not fit the update slot"             // more than the 2 MB slot
#define PROBLEM_RAW_UPDATE_FLASH_FAILED "flash write failed while installing the firmware"   // erase, write or switching the boot slot failed
#define PROBLEM_RAW_UPDATE_WRONG_CODE "update refused: wrong code"                           // the code was used up by 3 wrong tries
#define PROBLEM_RAW_UPDATE_CODE_EXPIRED "update refused: code expired"                       // the 3 minutes ran out
#define PROBLEM_RAW_UPDATE_ROLLED_BACK "update rolled back"                                  // the new image failed its probation
#define PROBLEM_RAW_UPDATE_NO_ROLLBACK "no previous firmware to go back to"                  // it failed its probation and there is nothing to return to
#define PROBLEM_RAW_UPDATE_NOT_CONFIRMED "could not record that the new firmware is good"    // the otadata write that ends the probation failed

inline const char *problemFind(const char *hay, const char *needle) {  // case-insensitive substring; where it starts, or nullptr
  size_t n = strlen(needle);
  if (!n) return hay;
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return hay;
  }
  return nullptr;
}
inline bool problemHas(const char *hay, const char *needle) { return problemFind(hay, needle) != nullptr; }
// Like problemHas, but the needle must stand alone as a word: "eof" is not found in "geofence", "closed" not in "enclosed".
inline bool problemHasWord(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  if (!n) return true;
  for (const char *p = hay; *p; p++) {
    if (p > hay && isalpha((unsigned char)p[-1])) continue;
    size_t i = 0;
    while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n && !isalpha((unsigned char)p[n])) return true;
  }
  return false;
}

// Turns what the connection code reports (an mbedTLS text, "HTTP 502", the PROBLEM_RAW_* texts, the firmware's own
// sentences) into a sentence an owner can act on. `why` gets the sentence. `detail` gets the technical text
// it came from (cut to fit), or "" when the sentence already says everything. A text it does not know is passed through
// with a capital letter, so a new kind of failure is shown, never hidden.
inline void problemWords(const char *raw, char *why, size_t whyCap, char *detail, size_t detailCap) {
  if (!why) whyCap = 0;  // no room for an output: nothing is written there
  if (!detail) detailCap = 0;
  if (!raw) raw = "";
  while (*raw == ' ') raw++;
  int http = 0;
  const char *h = problemFind(raw, "HTTP ");  // any case: "http 502" is a status too
  if (h) http = atoi(h + 5);
  const char *s = nullptr;  // a sentence without a number in it; the HTTP ones are worded below because they carry the status

  // Firmware updates first of all: their raw texts may end in a reason that holds an HTTP status, "timed out" or "refused", which the
  // branches below would take. Each sentence also holds a phrase of its own, so a sentence fed back in is recognised as itself.
  if (problemHas(raw, PROBLEM_RAW_UPDATE_NO_FIRMWARE) || problemHas(raw, "has no firmware published")) {
    s = "The service answered, but it has no firmware published to offer.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_MANIFEST_INVALID) || problemHas(raw, "firmware details from the service are not valid")) {
    s = "The firmware details from the service are not valid, so no update is offered.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_NO_SERVICE) || problemHas(raw, "no service address is saved")) {
    s = "No service address is saved, so there is nothing to check for updates.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_NO_CLOCK) || problemHas(raw, "clock is not set yet")) {
    s = "The clock is not set yet, and secure connections need the right time. Try again in a minute.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_DOWNLOAD_FAILED) || problemHas(raw, "firmware download did not finish")) {
    s = "The firmware download did not finish, so nothing was changed. Try again later.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_UPLOAD_INCOMPLETE) || problemHas(raw, "firmware file did not finish arriving")) {
    s = "The firmware file did not finish arriving, so nothing was changed. Try the upload again.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_WRONG_SIZE) || problemHas(raw, "not the size the service announced")) {
    s = "The downloaded firmware is not the size the service announced, so it was thrown away. Nothing was changed.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_WRONG_CHECKSUM) || problemHas(raw, "does not match the checksum the service announced")) {
    s = "The downloaded firmware does not match the checksum the service announced, so it was thrown away. Nothing was changed.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_WRONG_VERSION) || problemHas(raw, "different version from the one the service announced")) {
    s = "The downloaded firmware is a different version from the one the service announced, so it was not installed. Nothing was changed.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_BAD_IMAGE) || problemHas(raw, "not a valid program for this board")) {
    s = "The firmware file is not a valid program for this board's chip, so it was not installed. Nothing was changed.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_TOO_BIG) || problemHas(raw, "too big for the space kept for updates")) {
    s = "The firmware is too big for the space kept for updates, so it was not installed. Nothing was changed.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_FLASH_FAILED) || problemHas(raw, "writing the new firmware to the board's memory failed")) {
    s = "Writing the new firmware to the board's memory failed. The board keeps running the current firmware.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_WRONG_CODE) || problemHas(raw, "typed wrong three times")) {
    s = "The update was refused: the code was typed wrong three times. Starting again is blocked for a while.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_CODE_EXPIRED) || problemHas(raw, "code on the panel ran out of time")) {
    s = "The update was refused: the code on the panel ran out of time. Start again if you still want to update.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_NO_ROLLBACK) || problemHas(raw, "no previous firmware to go back to")) {
    s = "The new firmware failed its start-up check, but there is no previous firmware to go back to, so it keeps running.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_NOT_CONFIRMED) || problemHas(raw, "board could not record that")) {
    s = "The new firmware passed its start-up check, but the board could not record that, so it may go back after a few restarts.";
  } else if (problemHas(raw, PROBLEM_RAW_UPDATE_ROLLED_BACK) || problemHas(raw, "went back to the previous firmware")) {
    s = "The new firmware did not pass its start-up check, so the board went back to the previous firmware.";
  // Device authentication next: the raw texts of a refusal carry "HTTP 401" and "refused", which the branches below would take.
  } else if (problemHas(raw, PROBLEM_RAW_ID_MISSING)) {
    s = "This board has no identity record. It must be provisioned over USB.";
  } else if (problemHas(raw, PROBLEM_RAW_ID_INVALID)) {
    s = "This board's identity record is not valid. It must be provisioned again over USB.";
  } else if (problemHas(raw, PROBLEM_RAW_ID_OTHER_BOARD)) {
    s = "This board's identity record is for another board. It must be provisioned again over USB.";
  } else if (problemHas(raw, PROBLEM_RAW_ID_HARDWARE_KEY)) {
    s = "This board's record says the key is burned into the chip but the chip has no key. It must be provisioned again over USB.";
  } else if (problemHas(raw, PROBLEM_RAW_ID_HARDWARE_UNPROTECTED)) {
    s = "This board's record says the key is burned into the chip but the burned key is not fully protected, so it is not used. It must be provisioned again over USB.";
  } else if (problemHas(raw, PROBLEM_RAW_SIGNER_UNPROTECTED)) {
    s = "The key in the chip is not fully protected (was a burn interrupted?), so the board does not use it and keeps signing with the saved key.";
  } else if (problemHas(raw, PROBLEM_RAW_SIGNER_MISMATCH)) {
    s = "The chip's key and the saved key give different answers, so the board keeps signing with the saved key. The burn may have used the wrong key.";
  } else if (problemHas(raw, PROBLEM_RAW_SIGN_FAILED) || problemHas(raw, "could not sign")) {
    s = "The board could not sign the request. Restart it; if this keeps happening the firmware needs an update.";
  } else if (problemHas(raw, PROBLEM_RAW_AUTH_MISSING) || problemHas(raw, "did not receive a signature")) {
    s = "The service did not receive a signature on the request. Something between the board and the service may be removing it.";
  } else if (problemHas(raw, PROBLEM_RAW_AUTH_MALFORMED) || problemHas(raw, "could not read the signature")) {
    s = "The service could not read the signature on the request. The board and the service may run versions that do not match.";
  } else if (problemHas(raw, PROBLEM_RAW_AUTH_BAD_SIGNATURE) || problemHas(raw, "does not accept this board's key")) {
    s = "The service does not accept this board's key. Either the service is set up for other boards, or this board must be "
        "provisioned again over USB.";
  } else if (problemHas(raw, PROBLEM_RAW_AUTH_REVOKED) || problemHas(raw, "has revoked this board")) {
    s = "The service has revoked this board. The owner of the service has to allow it again.";
  } else if (problemHas(raw, PROBLEM_RAW_AUTH_CLOCK_SKEW) || problemHas(raw, "clock is too far off")) {
    s = "The service says this board's clock is too far off from its own. Check that both clocks are right.";
  } else if (http == 401 || http == 403) {
    // a refusal this device cannot read: not one of the reasons above (an older service, or something in front of it)
    snprintf(why, whyCap, "The service refused the request (HTTP %d) without a reason this device knows. Check the service address.", http);
  } else if (http == 404) {
    snprintf(why, whyCap, "The address answered, but it is not the data service (HTTP 404). Check the service address.");
  } else if (http == 429) {
    snprintf(why, whyCap, "The service is limiting requests (HTTP 429). It should recover by itself.");
  } else if (http >= 500 && http < 600) {
    snprintf(why, whyCap, "The service, or the server in front of it, is down or had an error (HTTP %d).", http);
  } else if (http > 0) {
    snprintf(why, whyCap, "The service answered with an unexpected status (HTTP %d).", http);
  } else if (problemHas(raw, PROBLEM_RAW_NO_ANSWER) || (h && h[5] == '0')) {
    // No status line arrived. live.cpp reports PROBLEM_RAW_NO_ANSWER; "HTTP 0" (an unparsable status line) is the same thing.
    // Before the timeout and closed branches below: the raw text contains both of those words.
    s = "The server hung up or did not answer in time.";
  } else if (problemHas(raw, PROBLEM_RAW_NO_ADDRESS) || problemHas(raw, "resolve")) {
    s = "The server name could not be looked up (DNS). Check the service address and the router's DNS.";
  } else if (problemHas(raw, "x509") || problemHas(raw, "certificate")) {
    // The core only ever reports "certificate verification failed": an expired or not-yet-valid certificate, an unknown
    // certificate authority and the wrong server cannot be told apart, so the sentence names all three.
    s = "The server's certificate could not be checked. The board's clock may be wrong, the certificate authority may be "
        "unknown, or this is not the real server.";
  } else if (problemHas(raw, "handshake") && (problemHas(raw, "timeout") || problemHas(raw, "timed out"))) {
    s = "The secure handshake timed out: the server did not answer in time.";
  } else if (problemHas(raw, "timeout") || problemHas(raw, "timed out")) {
    s = "The server did not answer in time.";
  } else if (problemHas(raw, "refused")) {
    s = "The server refused the connection.";
  } else if (problemHasWord(raw, "eof") || problemHasWord(raw, "reset") || problemHasWord(raw, "closed")) {
    s = "The server closed the connection before it finished answering.";
  } else if (problemHas(raw, "connect failed")) {
    s = "Could not connect to the server (nothing answered on port 443).";
  } else if (problemHas(raw, PROBLEM_RAW_UNEXPECTED)) {
    s = "The service answered with something this device cannot read.";
  } else if (problemHas(raw, PROBLEM_RAW_BUSY)) {
    s = "The device was busy with another request and gave up waiting.";
  } else {
    // unknown: shown as it came, with a capital letter, so a new kind of failure is visible rather than hidden
    snprintf(why, whyCap, "%s", raw[0] ? raw : "Unknown problem.");
    if (whyCap && why[0]) why[0] = (char)toupper((unsigned char)why[0]);
    if (detailCap) detail[0] = 0;
    return;
  }
  if (s) snprintf(why, whyCap, "%s", s);
  // the detail is the technical text the sentence came from, unless that is only "HTTP nnn", which the sentence has
  bool bareStatus = strncmp(raw, "HTTP ", 5) == 0 && strlen(raw) < 12;
  if (raw[0] && !bareStatus) snprintf(detail, detailCap, "%s", raw);
  else if (detailCap) detail[0] = 0;
}

// ---- the list ---------------------------------------------------------------------------------------------------------
struct ProblemEntry {
  ProblemKind kind = ProblemKind::Flights;
  uint32_t firstUnix = 0, lastUnix = 0;  // unix time of the first and the latest time; 0 = the clock was not set yet
  uint32_t firstUpS = 0, lastUpS = 0;    // seconds since the board started, always known
  uint16_t count = 0;                    // times it happened (stops at 65535)
  char why[160] = "";                    // plain words
  char detail[80] = "";                  // the technical text, or ""
};

struct ProblemLog {
  static const int MAX = 10;
  ProblemEntry e[MAX];  // newest first
  int n = 0;

  // Notes one problem. The same kind with the same words moves to the top and counts up.
  void record(ProblemKind kind, const char *whyText, const char *detailText, uint32_t unixNow, uint32_t upS) {
    if (!whyText) whyText = "";
    int at = -1;
    // compared over the stored length: a text that was cut to fit must still match itself when it comes again
    for (int i = 0; i < n; i++)
      if (e[i].kind == kind && strncmp(e[i].why, whyText, sizeof(e[i].why) - 1) == 0) { at = i; break; }
    ProblemEntry cur;
    if (at >= 0) {
      cur = e[at];
      if (cur.count < 65535) cur.count++;
      cur.lastUnix = unixNow;
      cur.lastUpS = upS;
      if (detailText && detailText[0]) snprintf(cur.detail, sizeof(cur.detail), "%s", detailText);  // the newest technical text
      for (int i = at; i > 0; i--) e[i] = e[i - 1];
    } else {
      cur.kind = kind;
      cur.firstUnix = cur.lastUnix = unixNow;
      cur.firstUpS = cur.lastUpS = upS;
      cur.count = 1;
      snprintf(cur.why, sizeof(cur.why), "%s", whyText);
      snprintf(cur.detail, sizeof(cur.detail), "%s", detailText ? detailText : "");
      int last = n < MAX ? n : MAX - 1;  // the oldest falls off the end when the list is full
      for (int i = last; i > 0; i--) e[i] = e[i - 1];
      if (n < MAX) n++;
    }
    e[0] = cur;
  }
};

// ---- the one-hour rule ------------------------------------------------------------------------------------------------
struct ProblemWatch {
  bool failing[PROBLEM_KINDS] = {};
  uint32_t since[PROBLEM_KINDS] = {};  // millis of the first failure of the current run
  uint32_t last[PROBLEM_KINDS] = {};   // millis of the latest failure

  void failed(ProblemKind k, uint32_t nowMs) {
    int i = (int)k;
    if (i < 0 || i >= PROBLEM_KINDS) return;
    // A new run starts when the kind was not failing, and also when its previous failure is more than PROBLEM_FRESH_MS
    // old: nothing was recorded for that long (Wi-Fi down, never retried), so the failure was not continuous. A gap of
    // exactly PROBLEM_FRESH_MS still counts as continuous, the same "within" as kindIsLong.
    if (!failing[i] || (uint32_t)(nowMs - last[i]) > PROBLEM_FRESH_MS) { failing[i] = true; since[i] = nowMs; }
    last[i] = nowMs;
  }
  void ok(ProblemKind k) {
    int i = (int)k;
    if (i >= 0 && i < PROBLEM_KINDS) failing[i] = false;
  }
  void clear() {  // the owner changed the settings: a fresh try gets a fresh hour
    for (int i = 0; i < PROBLEM_KINDS; i++) failing[i] = false;
  }

  // How long this kind has been failing in a row; 0 when it is not failing.
  uint32_t failingForMs(ProblemKind k, uint32_t nowMs) const {
    int i = (int)k;
    return (i >= 0 && i < PROBLEM_KINDS && failing[i]) ? (uint32_t)(nowMs - since[i]) : 0;
  }
  // Is this kind failing now (it failed recently) and has it been for long enough to interrupt the owner?
  bool kindIsLong(ProblemKind k, uint32_t nowMs) const {
    int i = (int)k;
    if (i < 0 || i >= PROBLEM_KINDS || !failing[i]) return false;
    return (uint32_t)(nowMs - since[i]) >= PROBLEM_MESSAGE_AFTER_MS && (uint32_t)(nowMs - last[i]) <= PROBLEM_FRESH_MS;
  }
  bool longFailure(uint32_t nowMs) const {
    for (int i = 0; i < PROBLEM_KINDS; i++)
      if (kindIsLong((ProblemKind)i, nowMs)) return true;
    return false;
  }
};

// The panel message shows for 5 seconds at the start of every minute while a long failure lasts. (Free-running on
// uptime; the glitch when millis() wraps after 49 days is one shorter or longer window, nothing more.)
inline bool problemMessagePhase(uint32_t nowMs) { return nowMs % PROBLEM_MESSAGE_PERIOD_MS < PROBLEM_MESSAGE_SHOW_MS; }

// Does the panel message replace the picture right now? The one place that decides it (the sketch calls this).
//   live            the device is showing live data (not the example screens)
//   longFailure     ProblemWatch::longFailure(now): some kind of data has failed for an hour and still is failing
//   staticPicture   the picture is weather or the clock/idle screen; a plane card or a headline passes false and is never replaced
//   night           night blanking is on (PanelMode::Night); the 30 s NightAwake window passes false and may show it
inline bool problemMessageWanted(bool live, bool longFailure, uint32_t nowMs, bool staticPicture, bool night) {
  return live && longFailure && problemMessagePhase(nowMs) && staticPicture && !night;
}
