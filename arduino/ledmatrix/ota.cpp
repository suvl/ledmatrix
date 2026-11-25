#include "ota.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/md.h>

#include "identity.h"
#include "live.h"
#include "net.h"
#include "otalogic.h"
#include "problems.h"
#include "version.h"

extern volatile uint32_t gFrames, gFramesOver1s;  // the render task's counters (ledmatrix.ino)

// The WebServer logs every form field it reads at core debug level 5 (log_v), and the update code arrives in a form field. Level 4 and above
// are refused outright rather than debated: the code is "never in a log".
#if defined(CORE_DEBUG_LEVEL) && CORE_DEBUG_LEVEL >= 4
#warning "CORE_DEBUG_LEVEL is 4 or more: the WebServer then logs form fields, which would put the update code in the serial output"
#endif

// What this image says it is, for the updater of the image that comes after it (otalogic.h, VersionTagScanner). Kept in the image because
// `version()` reads it (so the linker cannot drop it), and findable because nothing else holds this prefix followed by a version.
extern "C" const char otaVersionTag[] __attribute__((used)) = OTA_VERSION_TAG_PREFIX FIRMWARE_VERSION "\x03";

static_assert((int)ESP_CHIP_ID_ESP32S3 == (int)otalogic::CHIP_ID_ESP32S3, "otalogic.h checks the image header for this chip");

namespace ota {
namespace {

using otalogic::Call;
using otalogic::Fail;
using otalogic::Refusal;
using otalogic::Source;
using otalogic::State;

const char *const NVS_NS = "update";   // the version that failed here and why (shared/firmware-update.md, Rollback)
const size_t PIECE = 4096;              // a download is read, hashed and written in pieces of this size
const int UPLOAD_SLACK = 8192;          // what a multipart body adds to the file: boundaries and the code field

// ---- state, guarded by `mu` ---------------------------------------------------------------------------------------------
// The lock is held for a few microseconds at a time and never across HTTPS, flash or NVS. ArduinoJson keeps a const char array by
// pointer, so every JSON answer is serialised before the lock is released.
SemaphoreHandle_t mu = nullptr;
otalogic::Flow flow;
otalogic::CodeAuth auth;
otalogic::CheckSchedule sched;
otalogic::Health health;

struct Notice {
  bool have = false;      // a version newer than this one (and than any that failed here) is on offer
  otalogic::Manifest m;   // valid while `have`
  bool checked = false;   // a check has worked since boot
  int64_t checkedAtS = 0;  // esp_timer seconds of the last successful check (64 bit: it does not wrap)
} notice;

struct RolledBack {
  bool have = false;
  char version[16] = "";
  char why[160] = "";
  otalogic::Version v;
} rolled;

// The rollback mechanism's own state. The boot record lives in RTC no-init memory: it survives a panic, a watchdog reset and a software
// reset (what a crash loop is made of) and not a power-on reset. It has no initialiser on purpose: a constructor would wipe it at every start.
RTC_NOINIT_ATTR otalogic::BootRecord bootRec;
volatile bool onProbation = false;  // this image's slot is NEW or PENDING_VERIFY and it has not been marked good (set once, in probationBoot)
uint32_t probationBoots = 0;   // the number of this boot within the probation
volatile bool started = false;  // begin() has run: the lock, the flow and the problem list exist
volatile Fail stuckFail = Fail::None;  // a rollback or a confirmation that did not work before begin() ran: reported by the data task
char stuckDetail[40] = "";

bool runningValid = false;
otalogic::Version runningV;
bool wantDownload = false;     // the right code was typed for the service's image: the data task fetches it
bool fetchingManifest = false; // the download starts with a fresh look at the manifest (the panel says "Checking")
char panelVersion[24] = "";    // the version being installed, for the panel

struct HwRandom : otalogic::RandomSource {
  uint32_t next32() override { return esp_random(); }  // the hardware generator (the radio is on, so it is true random)
} hwRandom;

void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mu); }

// ---- words ----------------------------------------------------------------------------------------------------------------
// The plain sentence for a raw text (problems.h), into out.
void plain(const char *raw, char *out, size_t cap) {
  char detail[sizeof(ProblemEntry::detail)];
  problemWords(raw, out, cap, detail, sizeof(detail));
}
void plain(Fail f, char *out, size_t cap) { plain(otalogic::failRaw(f), out, cap); }

// What went wrong: the kind of failure and the raw text to word (the macro, then ": " and a detail when there is one).
struct Outcome {
  Fail fail = Fail::None;
  String raw;
  Outcome() {}
  Outcome(Fail f, const String &detail = "") : fail(f), raw(otalogic::failRaw(f)) {
    if (f != Fail::None && detail.length()) { raw += ": "; raw += detail; }
  }
  // a failure of the connection itself: the connection code's own text, worded like every other fetch's
  static Outcome network(const String &rawText) {
    Outcome o(Fail::DownloadFailed);
    o.raw = rawText;
    return o;
  }
  bool ok() const { return fail == Fail::None; }
};

// ---- saved state ----------------------------------------------------------------------------------------------------------
void eraseSaved() {
  Preferences w;
  if (w.begin(NVS_NS, false)) { w.remove("failv"); w.remove("failwhy"); w.end(); }
}

void loadSaved() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) return;  // nothing saved yet
  String v = p.getString("failv", ""), why = p.getString("failwhy", "");
  p.end();
  otalogic::Version fv;
  if (!otalogic::parseVersion(v.c_str(), fv)) return;  // damaged or absent: ignored
  // Once this or a newer image is the one running, the record is history. Not while this image is itself on probation: a successful
  // update clears it, when the new image is marked good (markGood).
  if (!onProbation && runningValid && otalogic::failureRecordObsolete(runningV, fv)) {
    eraseSaved();
    return;
  }
  char norm[16];
  otalogic::formatVersion(fv, norm, sizeof(norm));
  rolled.have = true;
  rolled.v = fv;
  strlcpy(rolled.version, norm, sizeof(rolled.version));
  strlcpy(rolled.why, why.c_str(), sizeof(rolled.why));
}

const otalogic::Version *failedVersion() { return rolled.have ? &rolled.v : nullptr; }  // lock held

// ---- the installer: one image into the other app slot ----------------------------------------------------------------------
// The same code for the service's download and for an uploaded file. Nothing here touches the boot slot until finish() has checked
// everything; any failure leaves the running image as it was (the half-written slot is just an unused slot).
class Installer {
 public:
  ~Installer() { abort(); }

  Fail begin() {
    abort();
    part_ = esp_ota_get_next_update_partition(nullptr);
    if (!part_) return fail(Fail::FlashFailed, "no update slot");
    // Erased as the writes go (sector by sector), not all at once: no single flash operation is long enough to stall the display.
    esp_err_t e = esp_ota_begin(part_, OTA_WITH_SEQUENTIAL_WRITES, &h_);
    if (e != ESP_OK) return fail(Fail::FlashFailed, esp_err_to_name(e));
    open_ = true;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&md_);
    if (!info || mbedtls_md_setup(&md_, info, 0) != 0) return fail(Fail::FlashFailed, "no SHA-256");  // nothing was allocated
    mdOpen_ = true;
    if (mbedtls_md_starts(&md_) != 0) return fail(Fail::FlashFailed, "no SHA-256");                    // abort() frees it
    written_ = headLen_ = 0;
    headChecked_ = false;
    detail_[0] = 0;
    return Fail::None;
  }

  // Takes the next bytes of the file. The first 24 are held back until the header has been looked at, so nothing reaches flash for a file
  // that is plainly not a program for this chip.
  Fail write(const uint8_t *p, size_t n) {
    if (!open_) return Fail::FlashFailed;
    if (n == 0) return Fail::None;
    if (written_ + n > part_->size) return fail(Fail::TooBig, "");
    mbedtls_md_update(&md_, p, n);
    written_ += n;
    if (!headChecked_) {
      size_t take = otalogic::IMAGE_HEADER_LEN - headLen_;
      if (take > n) take = n;
      memcpy(head_ + headLen_, p, take);
      headLen_ += take;
      p += take;
      n -= take;
      if (headLen_ < otalogic::IMAGE_HEADER_LEN) return Fail::None;  // still collecting the header
      if (!otalogic::imageHeaderOk(head_, headLen_)) return fail(Fail::BadImage, "header");
      headChecked_ = true;
      Fail f = put(head_, headLen_);
      if (f != Fail::None) return f;
    }
    return n ? put(p, n) : Fail::None;
  }

  // The whole file has arrived. For the service's image: `expectSize` and `expectSha` (64 lowercase hex) come from the manifest; for an
  // uploaded file both are 0 / null (there is nothing to compare against). Then the ESP-IDF image validation, and only then the switch.
  Fail finish(size_t expectSize, const char *expectSha, const char *expectVersion) {
    if (!open_) return Fail::FlashFailed;
    if (!headChecked_) return fail(Fail::BadImage, "too short");
    if (expectSize && written_ != expectSize) return fail(Fail::WrongSize, "");
    uint8_t digest[32];
    mbedtls_md_finish(&md_, digest);
    mbedtls_md_free(&md_);
    mdOpen_ = false;
    if (expectSha) {
      char hex[65];
      otalogic::sha256ToHex(digest, hex);
      if (!otalogic::sha256Matches(hex, expectSha)) return fail(Fail::WrongChecksum, "");
    }
    esp_err_t e = esp_ota_end(h_);  // checks the image: header, segments, checksum, the appended SHA-256 (esp_image_verify)
    open_ = false;
    h_ = 0;
    if (e != ESP_OK) return fail(e == ESP_ERR_OTA_VALIDATE_FAILED ? Fail::BadImage : Fail::FlashFailed, esp_err_to_name(e));
    esp_app_desc_t desc;
    e = esp_ota_get_partition_description(part_, &desc);  // an app, not some other kind of image: it must carry the app description
    if (e != ESP_OK) return fail(Fail::BadImage, "no app description");
    if (expectVersion) {  // the service's image must say it is the version the manifest announced (a mislabelled build is not installed)
      Fail f = checkVersion(expectVersion);
      if (f != Fail::None) return f;
    }
    e = esp_ota_set_boot_partition(part_);  // checks the image once more, then writes the boot slot choice
    if (e != ESP_OK) return fail(e == ESP_ERR_OTA_VALIDATE_FAILED ? Fail::BadImage : Fail::FlashFailed, esp_err_to_name(e));
    return Fail::None;
  }

  // Reads the written image back and looks for the version marker the firmware embeds (otalogic.h, VersionTagScanner). The app description
  // is no use here: in this core it holds the core's build id, not our version.
  Fail checkVersion(const char *want) {
    otalogic::Version wantV, gotV;
    if (!otalogic::parseVersion(want, wantV)) return fail(Fail::WrongVersion, "the manifest's version");
    otalogic::VersionTagScanner sc;
    uint8_t buf[1024];
    for (size_t off = 0, n = 0; off < written_ && !sc.found(); off += n) {
      n = written_ - off < sizeof(buf) ? written_ - off : sizeof(buf);
      if (esp_partition_read(part_, off, buf, n) != ESP_OK) return fail(Fail::FlashFailed, "reading the image back");
      sc.feed(buf, n);
      if ((off / sizeof(buf)) % 64 == 63) vTaskDelay(1);  // a breath now and then: this is 1.4 MB of flash reads
    }
    if (!sc.found()) return fail(Fail::WrongVersion, "no version in the image");
    gotV = sc.version();
    if (otalogic::compareVersions(gotV, wantV) != 0) {
      char detail[40];
      snprintf(detail, sizeof(detail), "the image says %s", sc.text());
      return fail(Fail::WrongVersion, detail);
    }
    return Fail::None;
  }

  void abort() {
    if (open_) esp_ota_abort(h_);
    open_ = false;
    h_ = 0;
    if (mdOpen_) mbedtls_md_free(&md_);
    mdOpen_ = false;
  }

  size_t written() const { return written_; }
  const char *detail() const { return detail_; }  // the ESP-IDF error name (or a few words) for the problem list's detail

 private:
  const esp_partition_t *part_ = nullptr;
  esp_ota_handle_t h_ = 0;
  bool open_ = false;
  mbedtls_md_context_t md_;
  bool mdOpen_ = false;
  size_t written_ = 0;
  uint8_t head_[otalogic::IMAGE_HEADER_LEN];
  size_t headLen_ = 0;
  bool headChecked_ = false;
  char detail_[40] = "";

  Fail fail(Fail f, const char *detail) {
    strlcpy(detail_, detail ? detail : "", sizeof(detail_));
    abort();
    return f;
  }
  Fail put(const uint8_t *p, size_t n) {
    esp_err_t e = esp_ota_write(h_, p, n);
    if (e == ESP_OK) return Fail::None;
    return fail(e == ESP_ERR_INVALID_SIZE ? Fail::TooBig : (e == ESP_ERR_OTA_VALIDATE_FAILED ? Fail::BadImage : Fail::FlashFailed), esp_err_to_name(e));
  }
};

// ---- failing ---------------------------------------------------------------------------------------------------------------
// An update attempt failed: nothing was switched. The problem list, the portal's `error`, one Serial line. Lock free.
void failUpdate(const Outcome &o) {
  char why[160];
  plain(o.raw.c_str(), why, sizeof(why));
  liveNoteProblem(ProblemKind::Update, o.raw.c_str());
  lock();
  flow.fail(why);
  unlock();
  Serial.printf("OTA failed: %s\n", why);
}

// A check could not be done or did not work: a sentence in `error` and, when the problem is real, in the problem list.
void failCheck(const Outcome &o, bool listIt) {
  char why[160];
  plain(o.raw.c_str(), why, sizeof(why));
  if (listIt) liveNoteProblem(ProblemKind::Update, o.raw.c_str());
  lock();
  flow.setError(why);
  if (o.fail == Fail::NoFirmware || o.fail == Fail::ManifestInvalid) notice.have = false;  // the service has nothing valid on offer now
  unlock();
  Serial.printf("OTA check failed: %s\n", why);
}

// ---- the manifest --------------------------------------------------------------------------------------------------------
struct Buf {
  char data[otalogic::MANIFEST_MAX_BYTES + 1];
  size_t n = 0;
  bool full = false;
};
bool bufSink(const uint8_t *p, size_t len, void *ctx) {
  Buf *b = (Buf *)ctx;
  if (b->n + len > otalogic::MANIFEST_MAX_BYTES) { b->full = true; return false; }
  memcpy(b->data + b->n, p, len);
  b->n += len;
  return true;
}

// GET /v1/firmware/manifest on the saved service host, unsigned, and every rule of the contract. No lock held.
Outcome fetchManifest(const Settings &cfg, otalogic::Manifest &m) {
  Buf b;
  LiveStreamReply r;
  bool ok = liveHttpsStream(cfg.serviceHost, "/v1/firmware/manifest", 1024, -1, 5000, bufSink, &b, r);  // the same 5 s as the other fetches
  if (!ok) {
    if (b.full) return Outcome(Fail::ManifestInvalid, otalogic::manifestErrorText(otalogic::ManifestError::TooLong));
    if (r.status == 404) return Outcome(Fail::NoFirmware);
    return Outcome::network(r.error);  // HTTP 5xx, a timeout, a certificate: worded like every other fetch's failure
  }
  otalogic::ManifestError e = otalogic::parseManifest(b.data, b.n, m);
  if (e != otalogic::ManifestError::None) return Outcome(Fail::ManifestInvalid, otalogic::manifestErrorText(e));
  return Outcome();
}

// The check: the daily one, and "Check now". Runs on the data task.
void runCheck(const Settings &cfg, uint32_t now, bool synced) {
  Outcome o;
  otalogic::Manifest m;
  bool hostSaved = cfg.serviceHost[0] != 0;
  if (!hostSaved) o = Outcome(Fail::NoService);
  else if (!synced) o = Outcome(Fail::NoClock);
  else o = fetchManifest(cfg, m);

  if (o.ok()) {
    lock();
    bool offered = runningValid && otalogic::isOffered(runningV, m.v, failedVersion());
    notice.have = offered;
    if (offered) notice.m = m;
    notice.checked = true;
    notice.checkedAtS = esp_timer_get_time() / 1000000;
    flow.endCheck();
    sched.done(now, true);
    unlock();
    liveNoteWorking(ProblemKind::Update);
    Serial.printf("OTA check: service has %s, %s\n", m.version, offered ? "newer: offered" : "not newer: nothing to offer");
  } else {
    // A missing address or clock is not a problem of the update: it only says why the check did nothing. The rest is listed.
    failCheck(o, o.fail != Fail::NoService && o.fail != Fail::NoClock);
    lock();
    flow.endCheck();
    sched.done(now, false);
    unlock();
  }
}

// ---- the download from the service --------------------------------------------------------------------------------------
struct Download {
  Installer *inst;
  uint32_t total, done;
  uint32_t lastLogPct;
  Fail fail;
  String detail;
};
bool downloadSink(const uint8_t *p, size_t len, void *ctx) {
  Download *d = (Download *)ctx;
  Fail f = d->inst->write(p, len);
  if (f != Fail::None) { d->fail = f; d->detail = d->inst->detail(); return false; }
  d->done += len;
  lock();
  flow.progress(d->done, d->total);
  unlock();
  uint32_t pct = (uint32_t)otalogic::progressPercent(d->done, d->total);
  if (pct >= d->lastLogPct + 20) { d->lastLogPct = pct; Serial.printf("OTA download %u%%\n", (unsigned)pct); }
  return true;
}

[[noreturn]] void restartSoon() {
  Serial.println("OTA restarting into the new firmware");
  vTaskDelay(pdMS_TO_TICKS(2000));  // the portal polls the state and sees "restarting"
  ESP.restart();
  for (;;) vTaskDelay(1000);
}

// Runs on the data task after the right code was typed for source "service": a fresh look at the manifest (the owner may have
// published since the last check), then the image, then the checks, then the switch.
void runDownload(const Settings &cfg, bool synced) {
  if (!synced) { failUpdate(Outcome(Fail::NoClock)); return; }
  if (WiFi.status() != WL_CONNECTED) { failUpdate(Outcome(Fail::DownloadFailed, "Wi-Fi is down")); return; }
  lock(); fetchingManifest = true; unlock();
  otalogic::Manifest m;
  Outcome o = fetchManifest(cfg, m);
  bool offered = false;
  lock();
  fetchingManifest = false;
  if (o.ok()) {
    offered = runningValid && otalogic::isOffered(runningV, m.v, failedVersion());
    notice.have = offered;
    if (offered) notice.m = m;
    notice.checked = true;
    notice.checkedAtS = esp_timer_get_time() / 1000000;
    strlcpy(panelVersion, m.version, sizeof(panelVersion));
  }
  unlock();
  if (!o.ok()) { failUpdate(o); return; }
  if (!offered) { failUpdate(Outcome(Fail::DownloadFailed, "the service no longer offers a newer version")); return; }

  char path[48];
  if (!otalogic::downloadPath(m, path, sizeof(path))) { failUpdate(Outcome(Fail::ManifestInvalid, otalogic::manifestErrorText(otalogic::ManifestError::File))); return; }

  Installer inst;
  Fail f = inst.begin();
  if (f != Fail::None) { failUpdate(Outcome(f, inst.detail())); return; }
  lock(); flow.progress(0, m.size); unlock();
  Serial.printf("OTA downloading %s (%u bytes)\n", m.version, (unsigned)m.size);

  Download d{&inst, m.size, 0, 0, Fail::None, ""};
  LiveStreamReply r;
  bool ok = liveHttpsStream(cfg.serviceHost, path, PIECE, (int64_t)m.size, 10000, downloadSink, &d, r);
  if (!ok) {
    inst.abort();
    if (d.fail != Fail::None) failUpdate(Outcome(d.fail, d.detail));                                                  // the installer said no
    else if (r.lengthMismatch) failUpdate(Outcome(Fail::WrongSize, "the server announced " + String((long)r.length) + " bytes"));
    else failUpdate(Outcome(Fail::DownloadFailed, r.error));                                                           // HTTP status, timeout, cut off
    return;
  }
  lock(); flow.downloaded(); unlock();
  f = inst.finish(m.size, m.sha256, m.version);  // size, SHA-256, the image validation, the version it says, the switch
  if (f != Fail::None) { failUpdate(Outcome(f, inst.detail())); return; }
  lock(); flow.installed(); unlock();
  liveNoteWorking(ProblemKind::Update);
  Serial.printf("OTA installed %s\n", m.version);
  restartSoon();
}

// ---- the probation checklist ---------------------------------------------------------------------------------------------
// The board's bootloader does not roll back, so this is the application's rollback (shared/firmware-update.md). The verdict is reached
// by a small task of its own (probationTask, at the end of this file), never by the data task: that one waits for Wi-Fi before it does
// anything, so it could not end the probation of an image that cannot join Wi-Fi, and nothing else can if setup() hangs.
void reportStuck(Fail f, const char *detail);

// ---- authorisation, shared by confirm and upload ------------------------------------------------------------------------
enum class AuthOutcome : uint8_t { Accepted, Wrong, UsedUp, Expired, NotAllowed };

// Is this call allowed, and is the code right? Called with the lock held. A right code is spent and the flow moves on; a code used
// up or run out takes the flow back to normal with the reason in `error`. The code is never copied anywhere.
AuthOutcome authorise(Call call, const char *typed, uint32_t now, Refusal &why, char *note, size_t noteCap) {
  note[0] = 0;
  why = flow.allow(call, true, call == Call::Confirm ? Source::Service : Source::Upload, probationActive());
  if (why != Refusal::None) {
    snprintf(note, noteCap, "%s", otalogic::refusalText(why));
    return AuthOutcome::NotAllowed;
  }
  char sentence[160];
  switch (auth.check(typed, now)) {
    case otalogic::CheckResult::Ok:
      flow.authOk();
      return AuthOutcome::Accepted;
    case otalogic::CheckResult::Wrong:
      snprintf(note, noteCap, "%s", otalogic::SENTENCE_CODE_WRONG);
      return AuthOutcome::Wrong;
    case otalogic::CheckResult::UsedUp:
      plain(Fail::WrongCode, sentence, sizeof(sentence));
      flow.authGone(sentence);  // what GET /api/update keeps saying
      otalogic::throttleSentence(auth.throttleSecondsLeft(now), note, noteCap);  // what this answer says: how long to wait
      return AuthOutcome::UsedUp;
    case otalogic::CheckResult::Expired:
      plain(Fail::CodeExpired, sentence, sizeof(sentence));
      flow.authGone(sentence);
      snprintf(note, noteCap, "%s", otalogic::SENTENCE_CODE_EXPIRED);
      return AuthOutcome::Expired;
    case otalogic::CheckResult::NoPending:
      break;
  }
  why = Refusal::NoPending;
  snprintf(note, noteCap, "%s", otalogic::refusalText(why));
  return AuthOutcome::NotAllowed;
}

// ---- JSON ----------------------------------------------------------------------------------------------------------------
// The one answer every route gives (shared/firmware-update.md). `note` replaces `error` for this answer only (a refused call). Lock held.
void buildJson(JsonDocument &doc, const char *note, const char *host, int http) {
  uint32_t now = millis();  // read under the lock: nothing recorded before it can be later than this
  int64_t nowS = esp_timer_get_time() / 1000000;
  doc["version"] = FIRMWARE_VERSION;
  if (notice.have) {
    JsonObject a = doc["available"].to<JsonObject>();
    a["version"] = notice.m.version;
    a["notes"] = notice.m.notes;
    a["size"] = notice.m.size;
    a["host"] = host;  // where the update comes from: the saved service host (the portal shows it next to the Update button)
  } else {
    doc["available"] = nullptr;
  }
  if (notice.checked) doc["checkedAgo"] = otalogic::ageSeconds(nowS, notice.checkedAtS);
  else doc["checkedAgo"] = nullptr;
  doc["state"] = otalogic::stateName(flow.shown());
  const char *src = otalogic::sourceName(flow.source);
  if (src && flow.base != State::Idle) doc["source"] = src;
  else doc["source"] = nullptr;
  if (flow.base == State::Downloading || flow.base == State::Installing || flow.base == State::Restarting) {
    JsonObject p = doc["progress"].to<JsonObject>();
    p["done"] = flow.done;
    p["total"] = flow.total;
  } else {
    doc["progress"] = nullptr;
  }
  if (auth.pending()) {
    doc["codeSecondsLeft"] = auth.secondsLeft(now);
    doc["triesLeft"] = auth.triesLeft();
  } else {
    doc["codeSecondsLeft"] = nullptr;
    doc["triesLeft"] = nullptr;
  }
  const char *err = note && note[0] ? note : flow.error;
  if (!err[0] && http >= 400) err = otalogic::refusalFallback(http);  // a refusal never goes out without a sentence
  if (err[0]) doc["error"] = err;
  else doc["error"] = nullptr;
  if (rolled.have) {
    JsonObject rb = doc["rolledBack"].to<JsonObject>();
    rb["version"] = rolled.version;
    rb["why"] = rolled.why;
  } else {
    doc["rolledBack"] = nullptr;
  }
}

Reply reply(int http, const char *note = nullptr, bool restart = false) {
  Reply r;
  r.http = http;
  r.restart = restart;
  JsonDocument doc;
  Settings cfg = settings::get();  // before the lock: the settings have a lock of their own
  lock();
  buildJson(doc, note, cfg.serviceHost, http);
  serializeJson(doc, r.json);  // before the unlock: the document points into the state
  unlock();
  return r;
}

}  // namespace

// ---- public: start-up and the data task -----------------------------------------------------------------------------------
void begin() {
  if (!mu) mu = xSemaphoreCreateMutex();  // probationBoot() may have made it already
  runningValid = otalogic::parseVersion(FIRMWARE_VERSION, runningV);
  if (!runningValid) Serial.println("OTA FIRMWARE_VERSION is not x.y.z: no update will be offered");
  loadSaved();
  if (probationActive()) flow.bootProbation();  // (the probation task, started by probationBoot, may already have marked it good: then not)
  started = true;
  Serial.printf("OTA version %s%s\n", FIRMWARE_VERSION, rolled.have ? " (an update to a newer version failed here and was rolled back)" : "");
}

// FIRMWARE_VERSION as plain text (for /status). The read of the marker is what keeps the marker in the linked image; the marker
// itself must never be returned: its text ends with a control character (0x03) that is not valid in JSON.
const char *version() {
  (void)*(volatile const char *)otaVersionTag;
  return FIRMWARE_VERSION;
}

bool flashing() {
  lock();
  bool f = flow.flashing();
  unlock();
  return f;
}

void dataTick(uint32_t now, bool synced, const Settings &cfg) {
  bool hostSaved = cfg.serviceHost[0] != 0;
  bool expired = false, doDownload = false, doCheck = false;
  if (stuckFail != Fail::None) {  // a rollback that could not be done, found out in setup() before the problem list existed
    Fail f = stuckFail;
    stuckFail = Fail::None;
    reportStuck(f, stuckDetail);
  }
  lock();
  sched.tick(now, synced, hostSaved);
  if (auth.tick(now)) {  // the code ran out of time: the panel goes back to normal, the portal says why
    char sentence[160];
    plain(Fail::CodeExpired, sentence, sizeof(sentence));
    flow.authGone(sentence);
    expired = true;
  }
  if (wantDownload) {
    wantDownload = false;
    doDownload = flow.base == State::Downloading;
  }
  if (!doDownload && !flow.flashing() && sched.due(now, synced, hostSaved)) {
    if (!flow.checking) flow.beginCheck();
    doCheck = true;
  }
  unlock();
  if (expired) liveNoteProblem(ProblemKind::Update, otalogic::failRaw(Fail::CodeExpired));
  if (doDownload) runDownload(cfg, synced);
  else if (doCheck) runCheck(cfg, now, synced);
}

// ---- public: the panel ---------------------------------------------------------------------------------------------------
PanelScreen panel(uint32_t now, UpdateView &out) {
  PanelScreen s = PanelScreen::None;
  lock();
  if (flow.base == State::WaitingCode && auth.pending() && auth.secondsLeft(now) > 0) {
    s = PanelScreen::Code;
    auth.codeText(out.code);
    out.secondsLeft = (int)auth.secondsLeft(now);
  } else if (flow.flashing()) {
    s = PanelScreen::Progress;
    strlcpy(out.version, flow.source == Source::Service ? panelVersion : "", sizeof(out.version));
    out.percent = flow.base == State::Downloading ? otalogic::progressPercent(flow.done, flow.total) : 100;
    if (out.percent > 99 && flow.base == State::Downloading) out.percent = 99;  // 100 is for when everything has arrived
    strlcpy(out.line, otalogic::progressWord(flow.base, flow.source, fetchingManifest), sizeof(out.line));
  }
  unlock();
  return s;
}

// ---- public: the portal's routes -----------------------------------------------------------------------------------------
Reply apiState() { return reply(200); }

Reply apiCheck() {
  Settings cfg = settings::get();
  uint32_t now = millis();
  lock();
  Refusal rf = flow.allow(Call::Check, notice.have);
  if (rf == Refusal::None) {
    flow.beginCheck();
    sched.checkNow();  // the data task does the work; this answers at once
  }
  unlock();
  if (rf != Refusal::None) return reply(409, otalogic::refusalText(rf));
  if (!cfg.serviceHost[0]) {  // nothing to ask: say so now (the data task would only say the same a moment later)
    char sentence[160];
    plain(Fail::NoService, sentence, sizeof(sentence));
    lock();
    flow.setError(sentence);
    flow.endCheck();
    sched.done(now, false);
    unlock();
  }
  return reply(200);
}

static bool parseBody(const char *body, size_t len, JsonDocument &doc) {
  if (!body || len == 0 || len > 256) return false;
  return !deserializeJson(doc, body, len) && doc.is<JsonObject>();
}

Reply apiStart(const char *body, size_t len) {
  JsonDocument doc;
  if (!parseBody(body, len, doc) || !doc["source"].is<const char *>()) return reply(400, "Send {\"source\":\"service\"} or {\"source\":\"upload\"}.");
  const char *s = doc["source"].as<const char *>();
  Source src = strcmp(s, "service") == 0 ? Source::Service : strcmp(s, "upload") == 0 ? Source::Upload : Source::None;
  if (src == Source::None) return reply(400, "The source must be \"service\" or \"upload\".");
  uint32_t now = millis();
  char note[96] = "";
  int http = 200;
  lock();
  Refusal rf = flow.allow(Call::Start, notice.have, src, probationActive());
  if (rf != Refusal::None) {
    snprintf(note, sizeof(note), "%s", otalogic::refusalText(rf));
    http = 409;
  } else if (auth.start(src, now, hwRandom) == otalogic::StartResult::Throttled) {
    otalogic::throttleSentence(auth.throttleSecondsLeft(now), note, sizeof(note));
    http = 429;
  } else {
    flow.startAuth(src);
  }
  unlock();
  if (http == 200) Serial.printf("OTA a code is on the panel (%s)\n", otalogic::sourceName(src));  // never the code itself
  return reply(http, note);
}

Reply apiConfirm(const char *body, size_t len) {
  JsonDocument doc;
  if (!parseBody(body, len, doc) || !doc["code"].is<const char *>()) return reply(400, "Send {\"code\":\"123456\"}: the six digits shown on the panel.");
  uint32_t now = millis();
  Refusal why = Refusal::None;
  char note[96];
  lock();
  AuthOutcome out = authorise(Call::Confirm, doc["code"].as<const char *>(), now, why, note, sizeof(note));
  if (out == AuthOutcome::Accepted) {
    wantDownload = true;
    panelVersion[0] = 0;  // the data task fills it in when it has looked at the manifest
  }
  unlock();
  switch (out) {  // every refusal carries its own sentence in `error`
    case AuthOutcome::Accepted: return reply(200);
    case AuthOutcome::Wrong: return reply(403, note);
    case AuthOutcome::UsedUp: liveNoteProblem(ProblemKind::Update, otalogic::failRaw(Fail::WrongCode)); return reply(403, note);
    case AuthOutcome::Expired: liveNoteProblem(ProblemKind::Update, otalogic::failRaw(Fail::CodeExpired)); return reply(403, note);
    case AuthOutcome::NotAllowed: break;
  }
  return reply(409, note);
}

Reply apiCancel() {
  lock();
  Refusal rf = flow.allow(Call::Cancel, notice.have);
  if (rf == Refusal::None && auth.cancel()) flow.authGone(nullptr);
  unlock();
  return rf == Refusal::None ? reply(200) : reply(409, otalogic::refusalText(rf));
}

// ---- public: the upload --------------------------------------------------------------------------------------------------
namespace {
struct Upload {
  bool sawFile = false;   // a file part started
  bool refused = false;   // answered already (wrong code, not allowed, a bad file): the rest of the body is read and thrown away
  int http = 200;
  char note[160] = "";    // for the answer
  bool installed = false; // the file passed everything and the boot slot is switched
  uint32_t total = 0, done = 0;
  uint32_t lastMs = 0;    // the last callback, so a request that never finished cannot block the next one
  Installer inst;

  void reset() {
    inst.abort();
    sawFile = refused = installed = false;
    http = 200;
    note[0] = 0;
    total = done = 0;
  }
} up;

void uploadRefuse(int http, const char *note) {
  up.refused = true;
  up.http = http;
  snprintf(up.note, sizeof(up.note), "%s", note ? note : "");
  up.inst.abort();
}
}  // namespace

void uploadBegin(const char *code, const char *fieldName, int contentLength) {
  uint32_t now = millis();
  if (up.sawFile && now - up.lastMs > 10000) up.reset();  // left over from a request that never reached its end
  up.lastMs = now;
  if (up.sawFile) {  // a second file in the same form: only one image is taken
    uploadRefuse(400, "Send one file only.");
    return;
  }
  up.sawFile = true;
  if (!fieldName || strcmp(fieldName, "file") != 0) { uploadRefuse(400, "The firmware file must be sent in the form field \"file\"."); return; }

  // Is this upload allowed at all, and does the file fit? Both are asked BEFORE the code is spent: a file that cannot be installed must not
  // use up the code (and one of its three tries is not for a size). `contentLength` is the whole body, a little above the file.
  Refusal why = Refusal::None;
  lock();
  why = flow.allow(Call::Upload, false, Source::Upload, probationActive());
  unlock();
  if (why != Refusal::None) { uploadRefuse(409, otalogic::refusalText(why)); return; }
  if (!code || !code[0]) { uploadRefuse(400, "The code must be sent before the file."); return; }  // not a try: nothing was guessed
  if (contentLength > (int)otalogic::IMAGE_MAX_BYTES + UPLOAD_SLACK) {  // when the browser said how big the body is; otherwise the write check catches it
    char sentence[160];
    plain(Fail::TooBig, sentence, sizeof(sentence));
    liveNoteProblem(ProblemKind::Update, (String(otalogic::failRaw(Fail::TooBig)) + ": the upload is " + String(contentLength) + " bytes").c_str());
    uploadRefuse(413, sentence);
    return;
  }

  // The code: not a byte goes to flash before it is right.
  char note[96];
  lock();
  AuthOutcome out = authorise(Call::Upload, code, now, why, note, sizeof(note));
  unlock();
  switch (out) {
    case AuthOutcome::Accepted: break;
    case AuthOutcome::Wrong: uploadRefuse(403, note); return;
    case AuthOutcome::UsedUp: liveNoteProblem(ProblemKind::Update, otalogic::failRaw(Fail::WrongCode)); uploadRefuse(403, note); return;
    case AuthOutcome::Expired: liveNoteProblem(ProblemKind::Update, otalogic::failRaw(Fail::CodeExpired)); uploadRefuse(403, note); return;
    case AuthOutcome::NotAllowed: uploadRefuse(409, note); return;
  }

  // Accepted: the state is "downloading" now (the panel shows the progress screen).
  Fail f = up.inst.begin();
  if (f != Fail::None) {
    failUpdate(Outcome(f, up.inst.detail()));
    uploadRefuse(500, "");
    return;
  }
  up.total = contentLength > 0 ? (uint32_t)contentLength : 0;
  lock(); flow.progress(0, up.total); panelVersion[0] = 0; unlock();
  Serial.println("OTA receiving an uploaded firmware file");
}

void uploadWrite(const uint8_t *data, size_t len) {
  up.lastMs = millis();
  if (!up.sawFile || up.refused || len == 0) return;  // thrown away
  Fail f = up.inst.write(data, len);
  if (f != Fail::None) {
    failUpdate(Outcome(f, up.inst.detail()));
    uploadRefuse(f == Fail::FlashFailed ? 500 : 422, "");
    return;
  }
  up.done += len;
  lock(); flow.progress(up.done, up.total > up.done ? up.total : up.done); unlock();
}

void uploadEnd() {
  up.lastMs = millis();
  if (!up.sawFile || up.refused) return;
  lock(); flow.downloaded(); unlock();  // all bytes are in: the validation and the switch
  Fail f = up.inst.finish(0, nullptr, nullptr);
  if (f != Fail::None) {
    failUpdate(Outcome(f, up.inst.detail()));
    uploadRefuse(f == Fail::FlashFailed ? 500 : 422, "");
    return;
  }
  lock(); flow.installed(); unlock();
  liveNoteWorking(ProblemKind::Update);
  up.installed = true;
  Serial.printf("OTA uploaded firmware accepted (%u bytes)\n", (unsigned)up.done);
}

void uploadAbort() {  // the browser went away, or the request broke off
  if (!up.sawFile) return;
  bool wasInstalled = up.installed;
  bool midFile = !up.refused && !up.installed;
  up.inst.abort();
  if (midFile) failUpdate(Outcome(Fail::UploadIncomplete));
  up.reset();
  if (wasInstalled) restartSoon();  // the file was all there and the boot slot is switched; only the end of the request broke: restart anyway
}

Reply uploadFinish() {
  bool saw = up.sawFile, installed = up.installed;
  int http = up.http;
  char note[sizeof(up.note)];
  strlcpy(note, up.note, sizeof(note));
  up.reset();
  if (!saw) return reply(400, "No file was sent.");
  if (installed) return reply(200, nullptr, true);  // the route sends this, then restarts the board
  if (http < 400) http = 500;                       // a file started and nothing says why it did not finish: not a success
  return reply(http, note);                         // the refusal, with the state's own `error` unless the note replaces it
}

// ---- public: probation (rollback) --------------------------------------------------------------------------------------------
// The mechanism, as specified in shared/firmware-update.md (bench-tested on the board; its bootloader does not roll back):
//  - every image says verifyRollbackLater() is true, so the core does not mark it valid before setup() (below);
//  - probationBoot(), first in setup(): is this image on probation, which boot of it is this, has it crashed too often; then it starts
//    the probation task;
//  - probationTask(): the checklist on a task of its own, markGood() when it passes, rollbackNow() when it fails (3 minutes at the latest);
//  - rollbackNow(): saves the failed version and why, then esp_ota_mark_app_invalid_rollback_and_reboot().
extern "C" bool verifyRollbackLater() { return true; }

namespace {
void reportStuck(Fail f, const char *detail) {
  if (!started || !liveStarted()) {  // setup() has not got that far: the problem list does not exist yet, the data task says it later
    stuckFail = f;
    strlcpy(stuckDetail, detail ? detail : "", sizeof(stuckDetail));
    return;
  }
  failUpdate(Outcome(f, detail ? detail : ""));
}

// The checklist, on a task of its own: started first thing in setup() (probationBoot), above the data task and the net task, and it reads
// only what cannot block: counters and flags written by the tasks that own them, so a stuck Wi-Fi join, a data task that never starts or
// a setup() that hangs still ends in a verdict at 3 minutes. 6 KB of stack: the rollback path does NVS and String work.
void probationTask(void *) {
  int markTries = 0;
  for (;;) {
    otalogic::HealthInputs in;
    in.wifiJoined = netWifiJoined();
    in.portalAnswering = netPortalAnswering();
    in.framesDrawn = gFrames;
    in.framesOver1s = gFramesOver1s;
    in.serviceReached = liveServiceAnswered();
    in.serviceExpected = true;  // until the settings and the identity exist (before ota::begin) nothing is waived
    if (started) {
      Settings c = settings::get();
      bool topics = false;
      for (int i = 0; i < 5; i++) topics = topics || c.queries[i][0];
      in.serviceExpected = otalogic::serviceCheckApplies(c.serviceHost[0] != 0, identity::status() == devauth::Identity::Ok,
                                                         otalogic::anythingToFetch(c.showFlights, c.showWeather, c.showNews, c.hasLocation, topics));
    }
    otalogic::HealthVerdict v = health.evaluate(millis(), in);
    if (v == otalogic::HealthVerdict::Good) {
      if (markGood() || ++markTries >= 5) break;  // a failed otadata write is tried a few more times, a quarter second apart
    } else if (v == otalogic::HealthVerdict::Failed) {
      rollbackNow(health.why());  // does not return when it works
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  Serial.printf("OTA probation task done (stack free %u bytes)\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  vTaskDelete(nullptr);
}

otalogic::ImageState imageStateOf(esp_ota_img_states_t st) {
  switch (st) {
    case ESP_OTA_IMG_NEW: return otalogic::ImageState::New;
    case ESP_OTA_IMG_PENDING_VERIFY: return otalogic::ImageState::PendingVerify;
    case ESP_OTA_IMG_VALID: return otalogic::ImageState::Valid;
    case ESP_OTA_IMG_INVALID: return otalogic::ImageState::Invalid;
    case ESP_OTA_IMG_ABORTED: return otalogic::ImageState::Aborted;
    default: return otalogic::ImageState::Undefined;
  }
}
}  // namespace

void probationBoot() {
  if (!mu) mu = xSemaphoreCreateMutex();  // the probation task may need the lock before begin() runs
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  const esp_partition_t *run = esp_ota_get_running_partition();
  // no record for this slot (a USB flash, or the factory slot): not on probation
  bool known = run && esp_ota_get_state_partition(run, &st) == ESP_OK;
  otalogic::ProbationStep step = otalogic::probationStep(otalogic::isProbation(known ? imageStateOf(st) : otalogic::ImageState::Undefined), bootRec);
  onProbation = step.onProbation;
  probationBoots = step.boots;
  if (!step.onProbation) return;
  Serial.printf("OTA this image is on probation: boot %u of at most %u\n", (unsigned)step.boots, (unsigned)otalogic::PROBATION_MAX_BOOTS);
  if (step.rollback) rollbackNow(otalogic::WHY_RESTART_LOOP);  // does not return when it works; if it does, the board carries on
  health.begin(0);                                               // "within 3 minutes of its boot": millis() counts from the boot
  if (xTaskCreate(probationTask, "probation", 6144, nullptr, 3, nullptr) != pdPASS)
    Serial.println("OTA could not start the probation task: this image stays on probation until it restarts too often");
}

bool probationActive() { return onProbation; }

bool markGood() {
  esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
  if (e != ESP_OK) {
    Serial.printf("OTA could not mark the image good: %s\n", esp_err_to_name(e));
    reportStuck(Fail::CannotConfirm, esp_err_to_name(e));
    return false;
  }
  otalogic::probationOver(bootRec);
  onProbation = false;
  lock();
  flow.markedGood();
  bool clear = rolled.have && runningValid && otalogic::failureRecordObsolete(runningV, rolled.v);
  if (clear) rolled.have = false;
  unlock();
  if (clear) eraseSaved();  // a successful later update clears the record of the one that failed
  if (liveStarted()) liveNoteWorking(ProblemKind::Update);
  Serial.println("OTA the new firmware passed its start-up check: kept");
  return true;
}

bool rollbackNow(const char *why) {
  if (!why || !why[0]) why = "The new firmware failed its start-up check.";
  Serial.printf("OTA start-up check failed: %s\n", why);
  if (!esp_ota_check_rollback_is_possible()) {  // no valid previous image: stay up and say so
    reportStuck(Fail::CannotRollBack, "");
    return false;
  }
  // The record first: the image that runs after the rollback reads it (rolledBack in /api/update, and this version is not offered again).
  {
    Preferences p;
    if (p.begin(NVS_NS, false)) {
      p.putString("failv", FIRMWARE_VERSION);
      p.putString("failwhy", why);
      p.end();
    }
  }
  Serial.println("OTA going back to the previous firmware");
  esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return when it works
  eraseSaved();                                                  // it did not: nothing was rolled back, so the record would be a lie
  reportStuck(Fail::CannotRollBack, esp_err_to_name(e));
  return false;
}

}  // namespace ota
