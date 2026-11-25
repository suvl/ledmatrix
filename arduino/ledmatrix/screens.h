// Every screen the device can show, ported from the preview page (preview/index.template.html).
#pragma once
#include <stdint.h>
#include <time.h>

#include "live.h"
#include "sample_data.h"

// Problem is last: ledmatrix.ino indexes a name table with these values.
enum class Screen : uint8_t { Flight, Weather, News, Idle, Setup, Boot, Connected, Stale, NoWifi, Problem };

// Live strings some screens show (setup network details, the station address).
struct UiState {
  const char *apSsid;
  const char *apPass;
  const char *apIp;
  const char *staIp;
  bool clock24h;
  // Live data. Null means "no service configured": the screens show their example values instead.
  const SampleFlight *flight = nullptr;
  const LiveWeather *wx = nullptr;
  const char *headline = nullptr;
  const char *headlineSource = nullptr;
  uint32_t headlineAgeMs = 0;
  const struct tm *clock = nullptr;
};

// Draws one frame of a screen into the Protomatter back buffer (the caller clears and calls show()).
//   layout:   0-based layout index within the screen (Flight 0..2, Weather 0..2, News 0..1, Idle 0=clock 1=blank)
//   ms:       milliseconds since boot, drives animation
//   flightIdx: which example flight to show (Flight screen only)
void drawScreen(Screen s, uint8_t layout, uint32_t ms, int flightIdx, const UiState &ui);
