// Every screen the device can show, ported from the preview page (preview/index.template.html).
#pragma once
#include <stdint.h>
#include <time.h>

#include "live.h"
#include "sample_data.h"

// UpdateProgress is last: ledmatrix.ino indexes a name table with these values.
enum class Screen : uint8_t { Flight, Weather, News, Idle, Setup, Boot, Connected, Stale, NoWifi, Problem, UpdateCode, UpdateProgress };

// What the two firmware-update screens show (shared/firmware-update.md). The update module fills it, the screens only draw it.
struct UpdateView {
  char code[8] = "";          // UpdateCode: the 6-digit authorisation code (NUL-terminated); never logged
  int secondsLeft = 0;        // UpdateCode: seconds until the code expires
  char version[24] = "";      // UpdateProgress: the version being installed; empty for an uploaded file of unknown version
  int percent = 0;            // UpdateProgress: 0..100
  char line[24] = "";         // UpdateProgress: a short status word, e.g. "Downloading", "Checking", "Installing", "Restarting"
};

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
  // Only for Screen::UpdateCode and Screen::UpdateProgress; null otherwise.
  const UpdateView *update = nullptr;
  // The display name (devicename.h, up to 24 characters): the Connected screen and the problem message show "<name>.local".
  // Null or empty means the default, "ledmatrix". (The setup screen shows apSsid, which is the same name.)
  const char *deviceName = nullptr;
};

// Draws one frame of a screen into the Protomatter back buffer (the caller clears and calls show()).
//   layout:   0-based layout index within the screen (Flight 0..2, Weather 0..2, News 0..1, Idle 0=clock 1=blank)
//   ms:       milliseconds since boot, drives animation
//   flightIdx: which example flight to show (Flight screen only)
void drawScreen(Screen s, uint8_t layout, uint32_t ms, int flightIdx, const UiState &ui);
