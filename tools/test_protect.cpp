// Host test for the panel protection logic (arduino/ledmatrix/protect.cpp). No Arduino headers needed.
//   c++ -std=c++17 -Iarduino/ledmatrix tools/test_protect.cpp arduino/ledmatrix/protect.cpp -o test_protect && ./test_protect
// protect.cpp keeps its state in statics, so every case (and every scenario inside a case) runs in a forked
// process that starts with a fresh state.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <initializer_list>

#include "protect.h"

static int gFails = 0;  // failed checks in the current process

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      printf("    line %d: check failed: %s\n", __LINE__, #cond);  \
      gFails++;                                                    \
    }                                                              \
  } while (0)

static const char *modeName(PanelMode m) {
  switch (m) {
    case PanelMode::Normal: return "Normal";
    case PanelMode::Dim: return "Dim";
    case PanelMode::Night: return "Night";
    case PanelMode::NightAwake: return "NightAwake";
  }
  return "?";
}

#define CHECK_MODE(out, want)                                                                        \
  do {                                                                                               \
    if ((out).mode != (want)) {                                                                      \
      printf("    line %d: mode %s, wanted %s\n", __LINE__, modeName((out).mode), modeName(want));   \
      gFails++;                                                                                      \
    }                                                                                                \
  } while (0)

#define CHECK_BRIGHT(out, want)                                                              \
  do {                                                                                       \
    if ((out).brightness != (want)) {                                                        \
      printf("    line %d: brightness %d, wanted %d\n", __LINE__, (out).brightness, (want)); \
      gFails++;                                                                              \
    }                                                                                        \
  } while (0)

// Runs `f` in a forked child so protect.cpp starts from a clean state; a failure inside counts as one here.
static void fresh(const std::function<void()> &f) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    gFails = 0;
    f();
    fflush(stdout);
    _exit(gFails ? 1 : 0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) gFails++;
}

// A simulated render task: frames every 30 ms, the local clock set explicitly by the test.
struct Sim {
  ProtectConfig cfg;
  uint32_t now = 1000;
  bool timeValid = true;
  uint16_t minute = 12 * 60;  // noon
  ProtectOut last;

  ProtectOut frame(PictureKind k) {  // one frame at the current time
    ProtectFrame f;
    f.nowMs = now;
    f.timeValid = timeValid;
    f.localMinute = minute;
    f.kind = k;
    last = protectStep(cfg, f);
    return last;
  }
  // Steps ms / 30 frames of 30 ms (ms is a multiple of 30 unless the test says otherwise); returns the last answer.
  ProtectOut run(PictureKind k, uint32_t ms) {
    ProtectOut o = last;
    for (uint32_t t = 0; t < ms; t += 30) {
      now += 30;
      o = frame(k);
    }
    return o;
  }
};

static const PictureKind P = PictureKind::Plane, H = PictureKind::Headline, S = PictureKind::Static, O = PictureKind::Other;
static const uint32_t MIN = 60000;

// ---- cases ------------------------------------------------------------------------------------------

static void t_defaults() {
  Sim s;
  s.cfg.drift = false;
  ProtectOut o = s.frame(S);
  CHECK_BRIGHT(o, 100);
  CHECK_MODE(o, PanelMode::Normal);
  o = s.run(S, 100020);  // 100 s of weather: inside the 2 minute default
  CHECK_BRIGHT(o, 100);
  CHECK_MODE(o, PanelMode::Normal);
  s.cfg.brightness = 60;
  o = s.run(S, 2010);
  CHECK_BRIGHT(o, 60);  // the owner's level is the normal level
  ProtectStatus st = protectStatus();
  CHECK(st.brightness == 60 && st.mode == PanelMode::Normal && st.dx == 0 && st.dy == 0);
}

static void t_dim_after_exactly() {
  fresh([] {
    Sim s;  // first frame at 1000 ms; stillness starts there
    s.cfg.drift = false;
    s.frame(S);
    ProtectOut o = s.run(S, 119970);
    CHECK(s.now == 1000 + 119970);
    CHECK_MODE(o, PanelMode::Normal);
    CHECK_BRIGHT(o, 100);
    o = s.run(S, 30);  // exactly 120 s after the first frame
    CHECK(s.now == 1000 + 120000);
    CHECK_MODE(o, PanelMode::Dim);
    o = s.run(S, 1020);
    CHECK_BRIGHT(o, 10);
    CHECK_MODE(o, PanelMode::Dim);
  });
  fresh([] {
    Sim t;  // another stillness time
    t.cfg.dimAfterMin = 5;
    t.cfg.drift = false;
    t.frame(S);
    ProtectOut o = t.run(S, 5 * MIN - 30);
    CHECK_MODE(o, PanelMode::Normal);
    o = t.run(S, 30);
    CHECK_MODE(o, PanelMode::Dim);
  });
}

static void t_dim_off() {
  Sim s;
  s.cfg.dim = false;
  s.frame(S);
  ProtectOut o = s.run(S, 10 * MIN);
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
}

static void t_dim_level_capped() {
  struct Row {
    uint8_t owner, dimLevel, want;
  };
  const Row rows[] = {
      {8, 10, 8},    // the owner's level is already below the dim level: dimming never brightens the panel
      {50, 30, 30},  // dim level is a percentage of FULL brightness, the same scale as the slider
      {20, 30, 20},
      {100, 10, 10},
      {100, 5, 5},
  };
  for (const Row &r : rows) {
    fresh([r] {
      Sim s;
      s.cfg.brightness = r.owner;
      s.cfg.dimLevel = r.dimLevel;
      s.cfg.drift = false;
      s.frame(S);
      ProtectOut o = s.run(S, 3 * MIN);
      CHECK_MODE(o, PanelMode::Dim);
      CHECK_BRIGHT(o, r.want);
    });
  }
}

static void t_plane_resets_and_is_never_dimmed() {
  Sim s;
  s.cfg.drift = false;
  s.frame(S);
  ProtectOut o = s.run(S, 3 * MIN);
  CHECK_MODE(o, PanelMode::Dim);
  CHECK_BRIGHT(o, 10);
  o = s.run(P, 1020);  // a plane: back to the owner's brightness within a second
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
  for (int i = 0; i < 6; i++) {  // a plane card on screen for an hour (cycle mode): never dimmed
    o = s.run(P, 10 * MIN);
    CHECK_MODE(o, PanelMode::Normal);
    CHECK_BRIGHT(o, 100);
  }
  // weather returns: stillness counts from the last plane frame
  o = s.run(S, 2 * MIN - 60);
  CHECK_MODE(o, PanelMode::Normal);
  o = s.run(S, 60);
  CHECK_MODE(o, PanelMode::Dim);
}

static void t_headline() {
  Sim s;
  s.cfg.drift = false;
  s.frame(S);
  ProtectOut o = s.run(S, 3 * MIN);
  CHECK_BRIGHT(o, 10);
  o = s.run(H, 1020);  // a headline's turn: the owner's brightness, not Dim
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
  o = s.run(H, 15000);
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
  o = s.frame(S);  // the turn ends; a headline is not activity, so it dims again at once
  CHECK_MODE(o, PanelMode::Dim);
  o = s.run(S, 1020);
  CHECK_BRIGHT(o, 10);
}

static void t_other_never_touched() {
  fresh([] {
    Sim s;
    s.cfg.drift = true;
    s.frame(O);
    ProtectOut o = s.run(O, 10 * MIN);
    CHECK_MODE(o, PanelMode::Normal);
    CHECK_BRIGHT(o, 100);
    CHECK(o.dx == 0 && o.dy == 0);
    s.minute = 2 * 60;  // night
    o = s.run(O, 2010);
    CHECK_MODE(o, PanelMode::Normal);
    CHECK_BRIGHT(o, 100);
  });
  fresh([] {
    Sim t;  // Other is not a Plane frame, so it does not reset stillness either
    t.cfg.drift = false;
    t.frame(S);
    t.run(S, 119010);
    t.run(O, 5010);
    ProtectOut o = t.frame(S);
    CHECK_MODE(o, PanelMode::Dim);
  });
}

static void t_fade_one_second() {
  Sim s;
  s.cfg.drift = false;
  s.frame(S);
  s.run(S, 2 * MIN - 30);  // the frame before the dim starts
  ProtectOut o = s.run(S, 30);
  CHECK_MODE(o, PanelMode::Dim);
  CHECK(o.brightness >= 95);  // a ramp, not a jump
  int at500 = s.run(S, 510).brightness;
  CHECK(at500 >= 40 && at500 <= 60);  // 100 -> 10 at 100 percent per second: about 50 half a second in
  int prev = at500, rising = 0;
  for (int i = 0; i < 40; i++) {
    int b = s.run(S, 30).brightness;
    if (b > prev) rising++;
    prev = b;
  }
  CHECK(rising == 0);
  CHECK(prev == 10);

  // and back up: 10 -> 100 takes 0.9 s
  o = s.frame(P);
  CHECK(o.brightness <= 20);
  int up500 = s.run(P, 510).brightness;
  CHECK(up500 >= 50 && up500 <= 70);
  o = s.run(P, 510);
  CHECK_BRIGHT(o, 100);
}

static void t_fade_on_portal_change() {
  Sim s;
  s.cfg.drift = false;
  s.cfg.dim = false;
  s.frame(S);
  s.cfg.brightness = 50;  // the owner moves the slider
  ProtectOut o = s.run(S, 30);
  CHECK(o.brightness > 90);
  o = s.run(S, 300);
  CHECK(o.brightness > 55 && o.brightness < 80);
  o = s.run(S, 1020);
  CHECK_BRIGHT(o, 50);
}

static void t_fade_clamps_a_stall() {
  Sim s;
  s.cfg.drift = false;
  s.cfg.dim = false;
  s.frame(S);
  s.minute = 2 * 60;  // the night starts ...
  s.now += 5000;      // ... and the render task stalled for 5 s
  ProtectOut o = s.frame(S);
  CHECK(o.brightness >= 70);  // one frame moves at most 250 ms worth, 25 percent
}

static void t_night_window() {
  struct Row {
    uint16_t start, end, minute;
    bool blank;
  };
  const Row rows[] = {
      // default 00:00 -> 06:00
      {0, 360, 0, true},
      {0, 360, 359, true},
      {0, 360, 360, false},
      {0, 360, 720, false},
      {0, 360, 1439, false},
      // wraps past midnight: 22:00 -> 06:00
      {1320, 360, 1319, false},
      {1320, 360, 1320, true},
      {1320, 360, 1439, true},
      {1320, 360, 0, true},
      {1320, 360, 359, true},
      {1320, 360, 360, false},
      {1320, 360, 720, false},
      // an evening window that does not wrap: 20:00 -> 23:00
      {1200, 1380, 1199, false},
      {1200, 1380, 1200, true},
      {1200, 1380, 1379, true},
      {1200, 1380, 1380, false},
      // start == end blanks nothing
      {360, 360, 360, false},
      {0, 0, 0, false},
      {0, 0, 700, false},
  };
  for (const Row &r : rows) {
    for (PictureKind k : {P, H, S}) {  // planes and headlines do not wake a blanked panel
      fresh([r, k] {
        Sim s;
        s.cfg.dim = false;
        s.cfg.drift = false;
        s.cfg.nightStartMin = r.start;
        s.cfg.nightEndMin = r.end;
        s.minute = r.minute;
        s.frame(k);
        ProtectOut o = s.run(k, 1200);
        bool dark = o.brightness == 0 && o.mode == PanelMode::Night;
        bool lit = o.brightness == 100 && o.mode == PanelMode::Normal;
        if (r.blank ? !dark : !lit) {
          printf("    window %u-%u at %u kind %d: brightness %d mode %s, wanted %s\n", r.start, r.end, r.minute, (int)k,
                 o.brightness, modeName(o.mode), r.blank ? "Night/0" : "Normal/100");
          gFails++;
        }
      });
    }
  }
}

static void t_night_off_invalid_time_other() {
  Sim s;
  s.cfg.dim = false;
  s.cfg.drift = false;
  s.minute = 2 * 60;
  s.timeValid = false;  // the clock is not synced yet: never blank on a guess
  s.frame(S);
  ProtectOut o = s.run(S, 2010);
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
  s.timeValid = true;
  s.cfg.night = false;  // switched off in the portal
  o = s.run(S, 2010);
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
  s.cfg.night = true;
  o = s.run(O, 2010);  // setup, boot and no-Wi-Fi screens stay visible at night
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
  o = s.run(S, 2010);
  CHECK_MODE(o, PanelMode::Night);
  CHECK_BRIGHT(o, 0);
}

static void t_wake_window_at_night() {
  Sim s;
  s.cfg.dim = false;
  s.cfg.drift = false;
  s.minute = 2 * 60;
  s.frame(S);  // the first frame starts blanked
  ProtectOut o = s.run(S, 2010);
  CHECK_MODE(o, PanelMode::Night);
  CHECK_BRIGHT(o, 0);
  o = s.run(P, 2010);  // a plane does not wake it
  CHECK_MODE(o, PanelMode::Night);
  CHECK_BRIGHT(o, 0);
  o = s.run(H, 2010);  // nor does a headline
  CHECK_MODE(o, PanelMode::Night);
  CHECK_BRIGHT(o, 0);

  protectWake(s.now);
  const uint32_t wakeAt = s.now;
  o = s.run(S, 30);
  CHECK_MODE(o, PanelMode::NightAwake);
  CHECK(o.brightness > 0 && o.brightness < 10);  // fading up, not a jump
  o = s.run(S, 1020);
  CHECK_BRIGHT(o, 100);
  o = s.run(S, 28920);
  CHECK(s.now - wakeAt == 29970);
  CHECK_MODE(o, PanelMode::NightAwake);
  CHECK_BRIGHT(o, 100);
  o = s.run(S, 30);  // exactly 30 s after the press: the window has ended
  CHECK(s.now - wakeAt == PROTECT_WAKE_WINDOW_MS);
  CHECK_MODE(o, PanelMode::Night);
  CHECK(o.brightness > 0 && o.brightness < 100);  // fading down
  o = s.run(S, 1050);
  CHECK_BRIGHT(o, 0);
  o = s.run(S, 60000);  // it ended by itself and does not come back
  CHECK_MODE(o, PanelMode::Night);
  CHECK_BRIGHT(o, 0);

  // a second press opens a new window, and a plane card shows meanwhile
  protectWake(s.now);
  o = s.run(P, 1200);
  CHECK_MODE(o, PanelMode::NightAwake);
  CHECK_BRIGHT(o, 100);
}

static void t_wake_stamped_slightly_ahead() {
  Sim s;  // loop() reads millis() after the render task did: the press time can be a few ms ahead of the frame time
  s.cfg.dim = false;
  s.cfg.drift = false;
  s.minute = 2 * 60;
  s.frame(S);
  s.run(S, 2010);
  protectWake(s.now + 7);
  ProtectOut o = s.frame(S);
  CHECK_MODE(o, PanelMode::NightAwake);
  o = s.run(S, 1500);
  CHECK_MODE(o, PanelMode::NightAwake);
  CHECK_BRIGHT(o, 100);
}

static void t_wake_resets_dim() {
  Sim s;
  s.cfg.drift = false;
  s.frame(S);
  ProtectOut o = s.run(S, 3 * MIN);
  CHECK_MODE(o, PanelMode::Dim);
  CHECK_BRIGHT(o, 10);
  protectWake(s.now);
  const uint32_t wakeAt = s.now;
  o = s.run(S, 30);
  CHECK_MODE(o, PanelMode::Normal);
  o = s.run(S, 1020);
  CHECK_BRIGHT(o, 100);
  o = s.run(S, 118920);  // 119.97 s after the press
  CHECK(s.now - wakeAt == 119970);
  CHECK_MODE(o, PanelMode::Normal);  // stillness restarted at the press
  o = s.run(S, 30);
  CHECK_MODE(o, PanelMode::Dim);
}

static void t_wake_in_daytime_is_harmless() {
  Sim s;
  s.cfg.drift = false;
  s.frame(S);
  protectWake(s.now);
  ProtectOut o = s.run(S, 1020);
  CHECK_MODE(o, PanelMode::Normal);
  CHECK_BRIGHT(o, 100);
}

static void t_drift() {
  const int8_t want[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (PictureKind k : {S, P}) {  // plane cards drift too
    fresh([&want, k] {
      Sim s;
      s.cfg.dim = false;
      s.cfg.night = false;
      for (uint32_t step = 0; step < 12; step++) {
        s.now = step * PROTECT_DRIFT_STEP_MS + 1000;
        ProtectOut o = s.frame(k);
        if (o.dx != want[step % 4][0] || o.dy != want[step % 4][1]) {
          printf("    kind %d step %u: drift %d,%d wanted %d,%d\n", (int)k, (unsigned)step, o.dx, o.dy,
                 want[step % 4][0], want[step % 4][1]);
          gFails++;
        }
        CHECK(o.dx >= 0 && o.dx <= 1 && o.dy >= 0 && o.dy <= 1);
      }
    });
  }
  fresh([] {  // the step is exactly 3 minutes: the last ms of a step keeps the old offset, the next ms moves
    Sim s;
    s.cfg.dim = false;
    s.now = PROTECT_DRIFT_STEP_MS - 1;
    ProtectOut o = s.frame(S);
    CHECK(o.dx == 0 && o.dy == 0);
    s.now = PROTECT_DRIFT_STEP_MS;
    o = s.frame(S);
    CHECK(o.dx == 1 && o.dy == 0);
    ProtectStatus st = protectStatus();
    CHECK(st.dx == 1 && st.dy == 0);
  });
}

static void t_drift_not_for_headline_other_or_when_off() {
  for (PictureKind k : {H, O}) {
    fresh([k] {
      Sim s;
      s.cfg.dim = false;
      s.cfg.night = false;
      for (uint32_t step = 0; step < 8; step++) {
        s.now = step * PROTECT_DRIFT_STEP_MS + 1000;
        ProtectOut o = s.frame(k);
        CHECK(o.dx == 0 && o.dy == 0);
      }
    });
  }
  fresh([] {
    Sim s;
    s.cfg.drift = false;
    s.cfg.dim = false;
    for (uint32_t step = 0; step < 8; step++) {
      s.now = step * PROTECT_DRIFT_STEP_MS + 1000;
      CHECK(s.frame(S).dx == 0 && s.last.dy == 0);
      CHECK(s.frame(P).dx == 0 && s.last.dy == 0);
    }
  });
}

static void t_rotation() {
  ProtectConfig c;
  c.rotate = false;
  CHECK(protectWeatherTurn(c, 0));
  CHECK(protectWeatherTurn(c, 15 * MIN));
  CHECK(protectWeatherTurn(c, 99 * MIN));
  c.rotate = true;
  c.rotateMin = 15;
  CHECK(protectWeatherTurn(c, 0));
  CHECK(protectWeatherTurn(c, 15 * MIN - 1));
  CHECK(!protectWeatherTurn(c, 15 * MIN));
  CHECK(!protectWeatherTurn(c, 30 * MIN - 1));
  CHECK(protectWeatherTurn(c, 30 * MIN));
  CHECK(!protectWeatherTurn(c, 45 * MIN));
  c.rotateMin = 1;
  CHECK(protectWeatherTurn(c, 30000));
  CHECK(!protectWeatherTurn(c, 60000));
  CHECK(protectWeatherTurn(c, 120000));
  c.rotateMin = 0;  // never divide by zero
  CHECK(protectWeatherTurn(c, 5000));
  c.rotateMin = 240;
  CHECK(protectWeatherTurn(c, 239 * MIN));
  CHECK(!protectWeatherTurn(c, 240 * MIN));
}

static void t_millis_wrap_dim() {
  fresh([] {
    Sim s;
    s.cfg.drift = false;
    s.now = 0u - 60000;  // a minute before millis() wraps
    s.frame(S);
    ProtectOut o = s.run(S, 60000);  // reaches the wrap
    CHECK(s.now == 0);
    CHECK_MODE(o, PanelMode::Normal);
    CHECK_BRIGHT(o, 100);
    o = s.run(S, 59970);
    CHECK_MODE(o, PanelMode::Normal);
    o = s.run(S, 30);  // 120 s after the first frame, past the wrap
    CHECK_MODE(o, PanelMode::Dim);
  });
  fresh([] {  // a plane just before the wrap resets stillness across it
    Sim t;
    t.cfg.drift = false;
    t.now = 0u - 30000;
    t.frame(S);
    t.run(P, 1020);
    ProtectOut o = t.run(S, 100020);
    CHECK(t.now < 100000);  // really wrapped
    CHECK_MODE(o, PanelMode::Normal);
    o = t.run(S, 20010);
    CHECK_MODE(o, PanelMode::Dim);
  });
}

static void t_millis_wrap_wake() {
  Sim s;
  s.cfg.dim = false;
  s.cfg.drift = false;
  s.minute = 2 * 60;
  s.now = 0u - 20000;
  s.frame(S);
  s.run(S, 2010);
  CHECK_BRIGHT(s.last, 0);
  protectWake(s.now);
  const uint32_t wakeAt = s.now;
  ProtectOut o = s.run(S, 15000);  // still before the wrap
  CHECK(s.now > wakeAt);
  CHECK_MODE(o, PanelMode::NightAwake);
  o = s.run(S, 10020);  // 25 s after the press, past the wrap
  CHECK(s.now < 100000);
  CHECK_MODE(o, PanelMode::NightAwake);
  CHECK_BRIGHT(o, 100);
  o = s.run(S, 5100);  // past 30 s
  CHECK_MODE(o, PanelMode::Night);
  o = s.run(S, 1500);
  CHECK_BRIGHT(o, 0);
}

static void t_wake_dim_across_wrap() {
  Sim s;
  s.cfg.drift = false;
  s.now = 0u - 200000;
  s.frame(S);
  s.run(S, 3 * MIN);  // dimmed, 20 s before the wrap
  CHECK_MODE(s.last, PanelMode::Dim);
  protectWake(s.now);
  s.run(S, 1500);
  CHECK_MODE(s.last, PanelMode::Normal);
  ProtectOut o = s.run(S, 60000);  // wrapped meanwhile
  CHECK(s.now < 100000);
  CHECK_MODE(o, PanelMode::Normal);
  o = s.run(S, 60000);
  CHECK_MODE(o, PanelMode::Dim);
}

static void t_very_long_stillness() {
  // no plane for weeks: the answer stays "dim" over the millis() wrap (the stillness age is kept below 2^31)
  Sim s;
  s.cfg.drift = false;
  s.now = 0;
  s.frame(S);
  const uint32_t jumps[] = {0x40000001u, 0x80000000u, 0xC0000000u, 0x00000005u, 0x40000000u};
  for (uint32_t t : jumps) {
    s.now = t;
    ProtectOut o = s.frame(S);
    if (o.mode != PanelMode::Dim) {
      printf("    at %08x: mode %s, wanted Dim\n", (unsigned)t, modeName(o.mode));
      gFails++;
    }
  }
}

// ---- runner -------------------------------------------------------------------------------------------

struct Case {
  const char *name;
  void (*fn)();
};

#define CASE(n) {#n, n}

int main() {
  const Case cases[] = {
      CASE(t_defaults),
      CASE(t_dim_after_exactly),
      CASE(t_dim_off),
      CASE(t_dim_level_capped),
      CASE(t_plane_resets_and_is_never_dimmed),
      CASE(t_headline),
      CASE(t_other_never_touched),
      CASE(t_fade_one_second),
      CASE(t_fade_on_portal_change),
      CASE(t_fade_clamps_a_stall),
      CASE(t_night_window),
      CASE(t_night_off_invalid_time_other),
      CASE(t_wake_window_at_night),
      CASE(t_wake_stamped_slightly_ahead),
      CASE(t_wake_resets_dim),
      CASE(t_wake_in_daytime_is_harmless),
      CASE(t_drift),
      CASE(t_drift_not_for_headline_other_or_when_off),
      CASE(t_rotation),
      CASE(t_millis_wrap_dim),
      CASE(t_millis_wrap_wake),
      CASE(t_wake_dim_across_wrap),
      CASE(t_very_long_stillness),
  };
  int failed = 0, total = 0;
  for (const Case &c : cases) {
    total++;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {  // child: a fresh copy of protect.cpp's state
      gFails = 0;
      c.fn();
      fflush(stdout);
      _exit(gFails ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", c.name);
    if (!ok) failed++;
  }
  printf("\n%d of %d cases passed\n", total - failed, total);
  return failed ? 1 : 0;
}
