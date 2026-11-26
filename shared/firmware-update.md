# Firmware updates, contract (slice 10)

Decisions: #21 (firmware update), amended by #37 (images come from the owner's service, never from GitHub; the board talks to one host). Build: #33. This page is the contract between the service (Go), the build workflow, the firmware (C++), the portal and the panel. Every example value is a placeholder.

## Where images come from

The service image carries the firmware. The build workflow compiles the firmware, writes the image and a manifest into `service/firmware/` (git-ignored) and builds the service image with that folder in it as `/firmware`. A new firmware reaches a board by deploying a new service image; no GitHub token, no storage, no extra secret. The folder is read-only data: the service never writes to it.

`tools/make_manifest.py <image.bin> --version <x.y.z> --notes "<text>" --out service/firmware/` writes `manifest.json` and copies the image to `ledmatrix-<version>.bin` in that folder. It reads nothing but its arguments and the image.

## Version

`arduino/ledmatrix/version.h` holds `#define FIRMWARE_VERSION "0.10.0"`: the single source (the workflow reads it, `/status` and the portal show it). Format `MAJOR.MINOR.PATCH`, digits only, each at most 4 digits (no pre-release tags). A build workflow run for a version that is already in the manifest is not a release. The board offers an update only when the manifest's version is strictly newer.

## The manifest and the download (service)

Both routes are public: they answer the same with or without signature headers (a board whose key is refused must still be able to fetch a fix).

```
GET /v1/firmware/manifest
200 {"version":"0.10.1","file":"ledmatrix-0.10.1.bin","size":1412537,"sha256":"<64 lowercase hex>","notes":"first lines of the release notes, at most 200 characters"}
404 {"error":"no firmware is published"}          (no manifest.json, or it is not valid)

GET /v1/firmware/ledmatrix-0.10.1.bin
200 application/octet-stream, Content-Length = size, the exact bytes
404 {"error":"no such firmware"}                   (any other name; the name is matched against the manifest's `file`, never used as a path)
```

The service validates the manifest when it starts and re-reads it at most once a minute (a file that does not match, wrong size or hash, is "no firmware is published", logged once). `size` must be above 100 KB and at most 2,097,152 bytes (one app slot). Caching: `Cache-Control: no-cache` for the manifest. The download is streamed, with range support from `http.ServeContent`. No signature, no token: the integrity check is the board's SHA-256 comparison against the manifest (over HTTPS with the built-in certificate store).

## What the board does

- **Check:** once a day (first check about 5 minutes after the clock is set, then every 24 hours) and when the owner presses "Check now". `GET /v1/firmware/manifest` on the saved service host only, without signature headers, with the same timeouts as the other fetches. The notice shows when the manifest version is strictly newer than `FIRMWARE_VERSION` and is not a version that already failed here (see Rollback). A failed check is a plain sentence in the problem list under `ProblemKind::Update`.
- **Download:** from `https://<service host><path>` where the path is `/v1/firmware/` plus the manifest's `file` (the manifest's text is only validated, never used as a host or a free path: `file` must match `ledmatrix-<version>.bin`). No redirects. SHA-256 computed while streaming and compared to the manifest at the end; the size must equal the manifest's; the image must pass the ESP-IDF image validation for this chip (ESP32-S3 header, one app slot) before the boot slot is switched. Anything wrong: nothing is switched, the running image stays, a plain sentence in the problem list, the panel returns to normal.
- **Never stall the display** (more than 1 s between frames) while checking, downloading or flashing: all of it on the data side, never on the render task; flash writes are chunked and the progress is shown on the panel.
- **Version** shown in `/status` as `"version":"0.10.0"` and on the portal.

## Where the update comes from (known limit)

The board trusts the saved service host: the portal has no password, so anyone on the LAN could point the board at a server of their own (with a valid certificate) and publish a "newer" version. What protects the owner: the panel code (someone must be at the panel), and the portal's notice states the host the update comes from (`available.host`), shown prominently next to the Update button. The real fix is a signed manifest (a public key pinned in the firmware); decision #21 chose "no signature for now" and the flow does not change when it is added (follow-up ticket).

## Authorisation

Both paths (the service's image and an uploaded `.bin`) need a code shown only on the panel:
- `POST /api/update/start` with `{"source":"service"}` or `{"source":"upload"}` makes the panel show `Screen::UpdateCode` with a random 6-digit code (from the hardware random number generator, digits only, leading zeros allowed). The code is never in a response, a log, `/status` or the serial output.
- The code is valid for 3 minutes and for 3 wrong tries; then it is gone and the owner must start again. Only one authorisation at a time; a new `start` replaces the old code, but the wrong tries already used carry over a replace or a cancel (so replacing cannot reset the 3 tries or the throttle); they reset on a correct code, a used-up code or expiry. Comparison in constant time. While no code is pending, `confirm` and `upload` are refused.
- Guessing is throttled: when a code is used up by 3 wrong tries, a new `start` is refused for 60 seconds, then 120, doubling up to 1 hour; a correct code resets it. (The portal has no password; a page in a browser on the LAN could post to it, but only someone who can see the panel can read the code.)
- `POST /api/update/confirm` with `{"code":"123456"}` starts the download for `source: service`.
- `POST /api/update/upload` (the forced `.bin`): multipart form with the file and the code, sent by the portal after `start`; the code is checked before the first byte is written to flash, and a wrong code stops the upload. The same SHA-256-less checks apply (valid app image for this chip, fits the slot); there is no manifest to compare against.
- After success, failure or expiry the panel returns to its normal screen (`Screen::UpdateProgress` while installing).

## The board's API for the portal (all JSON, LAN only, no password: the code is the authorisation)

```
GET  /api/update      -> {"version":"0.10.0",
                          "available":{"version":"0.10.1","notes":"...","size":1412537,"host":"<the saved service host>"} | null,
                          "checkedAgo":3600 | null,            seconds since the last successful check
                          "state":"idle|checking|waiting_code|downloading|installing|restarting|failed|probation",
                          "source":"service|upload" | null,
                          "progress":{"done":123456,"total":1412537} | null,
                          "codeSecondsLeft":150 | null, "triesLeft":3 | null,
                          "error":"one plain sentence" | null,
                          "rolledBack":{"version":"0.10.1","why":"one plain sentence"} | null}
POST /api/update/check   -> same JSON (starts a check, answers at once)
POST /api/update/start   {"source":"service|upload"}   -> same JSON (the panel now shows the code)
POST /api/update/confirm {"code":"123456"}              -> same JSON; wrong code: HTTP 403 with the same JSON, triesLeft smaller
POST /api/update/upload  multipart: file + code          -> same JSON when the image is accepted; the board restarts a moment later
POST /api/update/cancel  -> same JSON (drops a pending code; refused while flashing)
```

JSON is built with ArduinoJson under the lock that guards the state. No endpoint ever returns the code.

## The panel

Two screens in `screens.h` already (scaffold): `Screen::UpdateCode` and `Screen::UpdateProgress`, with their data in `UpdateView`. They are drawn instead of the normal screens (not dimmed, not drifting, columns 63 and rows 31 may be used for these two, but keep them legible), and the board returns to the normal cycle afterwards. The renderer in `shared/renderer.js`, the preview page and the margin check mirror them pixel for pixel like every other screen.

## Rollback (probation)

**Bench result (25 Nov 2026, on the real board):** the board's prebuilt TinyUF2 bootloader does NOT roll back: after an unvalidated restart it booted the same new image again, and the image state stayed `NEW` (never `PENDING_VERIFY`). So the rollback is done by the application, with the OTA state record the firmware itself maintains. Tested on the board: (T2) `esp_ota_mark_app_invalid_rollback_and_reboot()` from the new slot returns to the previous slot, marks the new one `INVALID`, and `esp_ota_get_last_invalid_partition()` reports it; (T3) a crash loop with a counter in `RTC_NOINIT_ATTR` memory (it survives panic resets) rolled back on the 4th boot; (T4) `esp_ota_mark_app_valid_cancel_rollback()` makes the state `VALID`. (Slice 15, ticket 47: the counter moved from RTC memory, which a power-on reset wipes, to NVS, so power cuts count too; see the first bullet below.)

The mechanism:
- Every image defines `bool verifyRollbackLater() { return true; }`, so the core does not mark the image valid before `setup()`.
- **On probation** = the running slot's state (`esp_ota_get_state_partition(esp_ota_get_running_partition(), &st)`) is `ESP_OTA_IMG_NEW` (or `PENDING_VERIFY`). `esp_ota_set_boot_partition()` sets `NEW` on the slot the updater switches to; an image flashed over USB has state `UNDEFINED` and is never on probation. The state is in `otadata`, so it survives power cuts.
- First thing in `setup()` (before Wi-Fi and the tasks): if on probation, increment the boot counter. More than 3 boots of the same probation (3 crashes or power cuts) -> `rollbackNow("The new version kept restarting.")`. The counter is **in NVS** (namespace `update`, key `pcount`, one 64-bit item: the image's identity in the high half, the count in the low half), so a power cut cannot defeat it (RTC memory is wiped by a power-on reset). Rules:
  - It exists only while an image is on probation. A start that is not on probation erases it when it finds one; `markGood()` erases it (after the image state is `VALID`); the installer erases it once the boot slot is switched (only writes when one is there).
  - It carries the identity of the image it counts for: a 32-bit hash of the first 8 bytes of the running app's ELF SHA-256 (`esp_app_desc_t.app_elf_sha256`) and the address of the slot it runs from. A count of another image is ignored and replaced: the count starts at 1.
  - A count this firmware cannot have written (0, or above 3) is ignored. If NVS cannot be read at all, the start counts as boot 1 and never rolls back by itself (the 3-minute checklist still does).
  - Flash writes: boots 1 to 3 write the count (one write each), the 4th boot rolls back without writing, and the image that comes back (or `markGood()`) erases it: at most 4 writes for one update, never more than one per start.
  - Nothing here changes the checklist: 3 minutes, a 60-second minimum soak, the verdict task.
- The image marks itself good (`esp_ota_mark_app_valid_cancel_rollback()`, counter reset) only when ALL of these have happened within 3 minutes of its boot: Wi-Fi joined; the portal server answering; the render task drawn at least 100 frames with none over 1 s; and the service reached: one complete HTTP answer (any status, 200, 401, 404, 5xx) from the saved service host to any request the board made (a data fetch or the update check), because a board whose key is refused must still be able to take a fix; waived when no service address is saved, when the identity is not `ok` (nothing could be sent), or when nothing would be fetched. The verdict is reached by its OWN small task (started first thing by the probation step, higher priority than the data task, enough stack for NVS and String work), never by the data task, so a stuck Wi-Fi join or a hung `setup()` still ends in a rollback at 3 minutes. Otherwise, at 3 minutes: `rollbackNow("<which condition was missing>")`. A hang that the task watchdog catches is a restart and counts like a crash.
- `rollbackNow(why)`: first write to NVS (the `update` namespace) the version that failed and the plain `why`, then `esp_ota_mark_app_invalid_rollback_and_reboot()`. If it returns (no valid previous image: check `esp_ota_check_rollback_is_possible()` first), stay up and put a plain sentence in the problem list. The image that runs after a rollback reads that record: `rolledBack` in `/api/update`, and that version (and anything not newer) is not offered again until a newer one exists. The record is cleared by a successful later update (`markGood()` of an image that is not older than the failed version). It also puts one plain entry in the portal's problem list (`ProblemKind::Update`, once per boot, from the data task, when the image is not itself on probation): the sentence "The new firmware (version x.y.z) did not pass its start-up check, so the board went back to the previous firmware.", with the reason as its detail. It is an event, not a failing data source: it goes to the list only and never feeds the one-hour rule behind the panel's PROBLEM message.
- Known gap: a crash before `setup()` starts (core initialisation, static constructors) is not caught. The image has passed ESP-IDF image validation and the SHA-256 check before the switch, and comes from the same build as the running one.

## Constraints for everyone

- This repository is public: placeholders only (example.com, version numbers, the vector-style hashes). No real host, address, name or secret anywhere.
- The display never stalls more than 1 s. No HTTPS or flash work on the render task, no lock held across HTTPS.
- Everything that can fail says why in plain words (problem list, `error`, `rolledBack.why`).
- Update paths are soft: a failed check or download changes nothing on the board.
