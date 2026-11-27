// Firmware updates, the board's side (slice 10, ticket 33; the contract is shared/firmware-update.md, the rules are in otalogic.h).
//
// Three ways in, none of them on the render task:
//   the data task   dataTick(): the daily check, "Check now", a confirmed download from the service
//   the probation task  a small task of its own that decides the probation (see below); the data task never does
//   the net task    the portal's routes (net.cpp) call the api*() functions, and the upload callbacks write the file straight to flash
//   the render task panel() once a frame: which of the two update screens to draw (a few microseconds under a lock that is never
//                   held across HTTPS or a flash write)
//
// Everything that can go wrong leaves the running firmware as it was: the new image is written to the other app slot, checked (size,
// SHA-256 against the manifest for the service's image, the ESP-IDF image validation for both), and only then does the boot slot
// change. A failure is a plain sentence in the problem list (ProblemKind::Update) and in the portal's `error`.
#pragma once
#include <Arduino.h>

#include "otalogic.h"
#include "screens.h"
#include "settings.h"

namespace ota {

// The portal's answer: an HTTP status and the JSON body (always the same JSON as GET /api/update, see shared/firmware-update.md).
struct Reply {
  int http = 200;
  String json;
  bool restart = false;  // the route restarts the board after the answer is sent (a firmware file was accepted)
};

enum class PanelScreen : uint8_t { None, Code, Progress };

// How long the panel shows "Restarting" (lit, at 100 %) before the board restarts, after the last flash write. Long enough to read.
constexpr uint32_t RESTART_SHOW_MS = 3000;

void begin();  // once, from setup(), after settings::begin(): creates the lock and reads the small saved state (namespace "update")
const char *version();  // FIRMWARE_VERSION, for /status (read from the marker in the image that the next update looks for)
const char *signingMode();  // "required" (updates must be signed) or "off" (a development build), for /status
bool flashing();        // an image is being downloaded, received or installed right now (the portal's connection test waits for the turn: it must not)

// The data task, every loop. `timeSynced`: the clock is set (secure connections need it). May block for the length of a check or a
// download; the display and the portal go on, they never wait for it.
void dataTick(uint32_t nowMs, bool timeSynced, const Settings &cfg);

// The render task, every frame: which update screen is wanted, with its data filled in `out` (not touched when None comes back).
// The code is in `out` only while the code screen is wanted; it never goes anywhere else.
PanelScreen panel(uint32_t nowMs, UpdateView &out);

// The render task, every frame, before it draws: flash writes make the panel flicker, so while an image is written the panel is off
// (otalogic.h, QuietGate). Blank: show one black frame and draw nothing until Light; Light: draw again. Never blocks, never locks.
otalogic::QuietGate::Act quietStep(uint32_t nowMs);

// The portal's routes (net.cpp). Bodies are the request's JSON text. No answer ever holds the code.
Reply apiState();                              // GET  /api/update
Reply apiCheck();                              // POST /api/update/check
Reply apiStart(const char *body, size_t len);  // POST /api/update/start   {"source":"service"|"upload"}
Reply apiConfirm(const char *body, size_t len);  // POST /api/update/confirm {"code":"123456"}: 403 with the same JSON when wrong
Reply apiCancel();                             // POST /api/update/cancel

// POST /api/update/upload (multipart: the field "code" FIRST, then the file). WebServer's upload callbacks call these in order; the
// code is checked in uploadBegin, before a byte is written to flash, and a wrong one makes the rest of the body be read and thrown
// away. The route's final handler calls uploadFinish for the answer.
void uploadBegin(const char *code, const char *fieldName, int contentLength);
void uploadWrite(const uint8_t *data, size_t len);
void uploadEnd();
void uploadAbort();
Reply uploadFinish();

// ---- probation (rollback) ---------------------------------------------------------------------------------------------------
// The board's bootloader does not roll back (bench test, shared/firmware-update.md), so the application does, with the OTA state record
// (esp_ota_get_state_partition) and a boot counter in NVS (kept only while an image is on probation, and carrying the image's identity, so a
// power cut cannot defeat it). The pure decisions are in otalogic.h (probationStep, Health).
// Every image also defines verifyRollbackLater() as true (ota.cpp), so the core does not mark it valid before setup().
void probationBoot();               // FIRST THING in setup(): on probation? which boot (counted in NVS)? more than 3 -> rollbackNow.
                                    // Then, on probation, it starts the probation task (priority above the data task, 6 KB of stack):
                                    // the checklist runs there and reaches its verdict at 3 minutes even if nothing else ever runs.
bool probationActive();             // this image is on probation and has not been marked good
bool markGood();                    // the checklist passed: esp_ota_mark_app_valid_cancel_rollback(); false when that failed
// The checklist failed (or the image kept restarting): save the failed version and `why` (one plain sentence) to NVS, then
// esp_ota_mark_app_invalid_rollback_and_reboot(). Does not return when it works. Returns false, with a plain sentence in the problem list,
// when there is no previous image to go back to; the board then stays up.
bool rollbackNow(const char *why);

}  // namespace ota
