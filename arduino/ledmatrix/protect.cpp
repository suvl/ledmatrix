// Panel protection logic (see protect.h). No Arduino headers: it is unit-tested on the laptop.
#include "protect.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

// ---- state, owned by the render task except the two wake variables ------------------------------
// protectWake() runs in loop(): a time and a sequence number, each a single 32-bit write, no mutex.
static volatile uint32_t gWakeAtMs = 0;
static volatile uint32_t gWakeSeq = 0;

static bool gStarted = false;
static uint32_t gLastMs = 0;        // time of the previous protectStep() call, for the fade
static float gLevel = 100;          // brightness actually applied, percent (float so slow fades do not stall)
static uint32_t gLastActivity = 0;  // last plane frame or wake
static uint32_t gSeenSeq = 0;       // last wake sequence number this task has handled
static bool gWindowOpen = false;    // the 30 s night wake window
static uint32_t gWindowAtMs = 0;

static volatile uint8_t gOutBrightness = 100;
static volatile uint8_t gOutMode = (uint8_t)PanelMode::Normal;
static volatile int8_t gOutDx = 0, gOutDy = 0;

static const uint32_t FADE_MAX_DT_MS = 250;           // a stalled frame must not make the fade jump
static const uint32_t KEEP_STILLNESS_MS = 0x40000000;  // keep the stillness age below 2^31 ms so the unsigned maths survives 49 days

// Milliseconds from `then` to `now` with millis() wrap-around handled by unsigned subtraction.
// A `then` that is a few ms ahead of `now` (a button press read after the frame took its time) counts as 0.
static uint32_t since(uint32_t now, uint32_t then) {
  uint32_t d = now - then;
  return d > 0xFFFFF000u ? 0 : d;
}

static bool inNightWindow(const ProtectConfig &cfg, uint16_t m) {
  uint16_t s = cfg.nightStartMin, e = cfg.nightEndMin;
  if (s == e) return false;  // blanks nothing
  if (s < e) return m >= s && m < e;
  return m >= s || m < e;  // wraps past midnight
}

ProtectOut protectStep(const ProtectConfig &cfg, const ProtectFrame &f) {
  const uint32_t now = f.nowMs;
  const bool firstCall = !gStarted;
  if (firstCall) {
    gStarted = true;
    gLastMs = now;
    gLastActivity = now;
  }

  // A button press from loop(): it wakes a dimmed panel and opens the night window.
  uint32_t seq = gWakeSeq;
  if (seq != gSeenSeq) {
    gSeenSeq = seq;
    uint32_t at = gWakeAtMs;
    if (since(now, at) == 0) at = now;  // never keep a time in the future
    gLastActivity = at;
    gWindowAtMs = at;
    gWindowOpen = true;
  }
  if (gWindowOpen && since(now, gWindowAtMs) >= PROTECT_WAKE_WINDOW_MS) gWindowOpen = false;

  if (f.kind == PictureKind::Plane) gLastActivity = now;
  uint32_t still = since(now, gLastActivity);
  if (still > KEEP_STILLNESS_MS) {  // no plane for 12 days: park the timestamp, the answer stays "still"
    gLastActivity = now - KEEP_STILLNESS_MS;
    still = KEEP_STILLNESS_MS;
  }

  // ---- the brightness this frame is heading for ----
  const float owner = cfg.brightness > 100 ? 100 : cfg.brightness;
  float target = owner;
  PanelMode mode = PanelMode::Normal;
  const bool nightNow = cfg.night && f.timeValid && inNightWindow(cfg, f.localMinute);
  if (f.kind != PictureKind::Other) {
    if (nightNow) {
      if (gWindowOpen) {
        mode = PanelMode::NightAwake;
      } else {
        mode = PanelMode::Night;
        target = 0;
      }
    } else if (cfg.dim && f.kind == PictureKind::Static && still >= (uint32_t)cfg.dimAfterMin * 60000UL) {
      mode = PanelMode::Dim;
      float lvl = cfg.dimLevel > 100 ? 100 : cfg.dimLevel;
      target = lvl < owner ? lvl : owner;
    }
  }

  // ---- fade: 100 percent per second, starting at the target on the first call ----
  uint32_t dt = since(now, gLastMs);
  if (dt > FADE_MAX_DT_MS) dt = FADE_MAX_DT_MS;
  gLastMs = now;
  if (firstCall) {
    gLevel = target;
  } else {
    float step = (float)dt * 0.1f;  // 100 percent per 1000 ms
    if (gLevel < target) {
      gLevel += step;
      if (gLevel > target) gLevel = target;
    } else if (gLevel > target) {
      gLevel -= step;
      if (gLevel < target) gLevel = target;
    }
  }

  ProtectOut out;
  int b = (int)lroundf(gLevel);
  out.brightness = (uint8_t)(b < 0 ? 0 : (b > 100 ? 100 : b));
  out.mode = mode;

  // ---- drift: a 4-step loop of right/down offsets, never negative (column 63 and row 31 stay blank) ----
  if (cfg.drift && (f.kind == PictureKind::Plane || f.kind == PictureKind::Static)) {
    static const int8_t DX[4] = {0, 1, 1, 0};
    static const int8_t DY[4] = {0, 0, 1, 1};
    uint32_t i = (now / PROTECT_DRIFT_STEP_MS) % 4;
    out.dx = DX[i];
    out.dy = DY[i];
  }

  gOutBrightness = out.brightness;
  gOutMode = (uint8_t)out.mode;
  gOutDx = out.dx;
  gOutDy = out.dy;
  return out;
}

void protectWake(uint32_t nowMs) {
  gWakeAtMs = nowMs;
  gWakeSeq = gWakeSeq + 1;  // written last: the render task reads the sequence first, then the time
}

bool protectWeatherTurn(const ProtectConfig &cfg, uint32_t nowMs) {
  if (!cfg.rotate) return true;
  uint32_t period = (uint32_t)(cfg.rotateMin ? cfg.rotateMin : 1) * 60000UL;
  return (nowMs / period) % 2 == 0;
}

ProtectStatus protectStatus() {
  ProtectStatus s;
  s.brightness = gOutBrightness;
  s.mode = (PanelMode)gOutMode;
  s.dx = gOutDx;
  s.dy = gOutDy;
  return s;
}
