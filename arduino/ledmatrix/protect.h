// Panel protection (tickets 24 and 25): pixel drift, dim after stillness, night blanking, clock/weather rotation.
// Pure logic with no Arduino headers, so it is unit-tested on the laptop (tools/test_protect.cpp).
//
// The render task calls protectStep() once per frame and applies the answer; loop() calls protectWake() on a button press.
// protectStep() keeps its state in this file and must only be called from one task.
#pragma once
#include <stdint.h>

// The slice of Settings that protection needs (the sketch copies it from Settings each frame).
struct ProtectConfig {
  uint8_t brightness = 100;  // the owner's level, percent
  bool drift = true;
  bool dim = true;
  uint16_t dimAfterMin = 2;
  uint8_t dimLevel = 10;     // percent of full brightness; the effective dim level is min(dimLevel, brightness)
  bool night = true;
  uint16_t nightStartMin = 0, nightEndMin = 360;  // minutes after local midnight
  bool rotate = false;
  uint16_t rotateMin = 15;
};

// What the picture on this frame is.
enum class PictureKind : uint8_t {
  Plane,     // a flight card: never dimmed, drifts, counts as activity (resets the stillness timer)
  Headline,  // a headline: shown at the owner's brightness for its turn, never drifts (it scrolls or pages)
  Static,    // weather, clock, idle: dims after stillness, drifts
  Other      // boot, setup, connected, no Wi-Fi: never dimmed, blanked or drifted
};

struct ProtectFrame {
  uint32_t nowMs = 0;
  bool timeValid = false;      // the clock is synced, so localMinute is meaningful; without it nothing is blanked
  uint16_t localMinute = 0;    // minutes after local midnight, 0..1439
  PictureKind kind = PictureKind::Other;
};

enum class PanelMode : uint8_t { Normal, Dim, Night, NightAwake };  // NightAwake: inside the 30 s a button press opens at night

struct ProtectOut {
  uint8_t brightness = 100;  // percent 0..100 to apply this frame, fades included; 0 = dark
  int8_t dx = 0, dy = 0;     // drift offset, each 0 or 1: shift the finished picture right and down by this many pixels
  PanelMode mode = PanelMode::Normal;
};

// One frame of protection. Fades take about one second (100 percent per second); the first call starts at its target.
ProtectOut protectStep(const ProtectConfig &cfg, const ProtectFrame &f);

// A button press: restores the owner's brightness (wakes a dimmed panel) and opens a 30 s window in a blanked one.
// Safe to call from another task than protectStep().
void protectWake(uint32_t nowMs);

// Rotation: true when it is weather's turn, false when it is the clock's. Changes every cfg.rotateMin minutes of uptime;
// always true when rotation is off.
bool protectWeatherTurn(const ProtectConfig &cfg, uint32_t nowMs);

// The last frame's answer, for the status page (/api/live "panel").
struct ProtectStatus {
  uint8_t brightness = 100;
  PanelMode mode = PanelMode::Normal;
  int8_t dx = 0, dy = 0;
};
ProtectStatus protectStatus();

// Drift step (3 minutes) and the night wake window (30 s), exposed for the tests.
static const uint32_t PROTECT_DRIFT_STEP_MS = 3UL * 60 * 1000;
static const uint32_t PROTECT_WAKE_WINDOW_MS = 30UL * 1000;
