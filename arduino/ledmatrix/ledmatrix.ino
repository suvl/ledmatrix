// LedMatrix firmware.
//   slice 1: every screen and layout on the real panel, with example data
//   slice 2: Wi-Fi setup flow: saved network, setup access point, mDNS, long-press UP to reset
//   slice 3: settings portal
//   slice 4: live data from the owner's service (this file decides what to show; see live.cpp)
//   slice 7: panel protection (protect.cpp): pixel drift, dim after stillness, night blanking, clock/weather rotation;
//            a short press of UP or DOWN wakes the panel
//   slice 8: the problem list (problems.h, live.cpp) and the panel message that points to it after an hour of failure
//
//   core 1: render task   ~30 fps, picks the screen from the network state, the settings and the live data
//   core 0: net task      Wi-Fi, setup access point, DNS, web server and portal API (see net.cpp)
//   core 0: data task     time sync, flights every 30 s, weather every 15 min, headlines every 30 min (live.cpp)
//   loop(): buttons       UP = next demo screen, DOWN = previous; hold UP for 5 s to forget the Wi-Fi network;
//                         any press also wakes the panel from the dim and from night blanking (30 s)
//
// With no data service address saved, the device cycles the demo screens with example data.

#include <Adafruit_Protomatter.h>

#include "display.h"
#include "identity.h"
#include "live.h"
#include "net.h"
#include "ota.h"
#include "plane_view.h"
#include "problems.h"
#include "protect.h"
#include "screens.h"
#include "settings.h"

// ---- panel wiring (Matrix Portal S3), 64x32 uses address lines A-D --------------------------
static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35};
static const uint8_t clockPin = 2;
static const uint8_t latchPin = 47;
static const uint8_t oePin = 14;
Adafruit_Protomatter matrix(64, 6, 1, rgbPins, 4, addrPins, clockPin, latchPin, oePin, true);

static const uint8_t BUTTON_UP = 6;    // per the board's Arduino pin definitions
static const uint8_t BUTTON_DOWN = 7;
static const uint32_t HOLD_TO_RESET_MS = 5000;
static const uint32_t CONNECTED_SCREEN_MS = 5000;

// ---- demo playlist: the enabled screens with their chosen layouts, with example data -----------------------
struct Item {
  Screen screen;
  uint8_t layout;
  const char *name;
};
static int buildPlaylist(const Settings &s, Item out[4]) {
  int n = 0;
  if (s.showFlights) out[n++] = {Screen::Flight, s.flightLayout, "flight"};
  if (s.showWeather) out[n++] = {Screen::Weather, s.weatherLayout, "weather"};
  if (s.showNews) out[n++] = {Screen::News, s.newsLayout, "headlines"};
  if (n == 0) out[n++] = {Screen::Idle, (uint8_t)(s.idleClock ? 0 : 1), "idle"};
  return n;
}

static volatile int gIndex = 0;
static volatile int gPass = 0;       // increments each time the playlist wraps; picks the example flight
static volatile int gHoldPct = 0;    // 0..100, how far the UP long press has progressed
static volatile bool gMatrixOk = false;

// Frame timing while online (the boot is exempt from the 1 s budget); shown in /status for the soak test.
volatile uint32_t gFrames = 0, gFrameMaxMs = 0, gFramesOver1s = 0;

// Protomatter has no brightness control, so the finished frame is scaled before it is sent to the panel.
// The panel runs at 6 bits per colour so that dimmed pictures keep their shades.
static void applyBrightness(uint8_t percent) {
  if (percent >= 100) return;
  if (percent == 0) {  // night blanking
    matrix.fillScreen(0);
    return;
  }
  uint32_t k = (uint32_t)percent * 256 / 100;  // 8.8 fixed point
  uint16_t *p = matrix.getBuffer();
  for (int i = 0; i < PANEL_W * PANEL_H; i++) {
    uint16_t c = p[i];
    if (!c) continue;
    uint32_t r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
    p[i] = (r << 11) | (g << 5) | b;
  }
}

// Pixel drift: moves the finished picture right and down by dx, dy (each 0 or 1; the screens keep column 63 and
// row 31 blank, so nothing visible falls off). Walks from the bottom right so it never reads a pixel it already wrote.
static void shiftPicture(int dx, int dy) {
  if (dx <= 0 && dy <= 0) return;
  uint16_t *p = matrix.getBuffer();
  for (int y = PANEL_H - 1; y >= 0; y--) {
    for (int x = PANEL_W - 1; x >= 0; x--) {
      int sx = x - dx, sy = y - dy;
      p[y * PANEL_W + x] = (sx >= 0 && sy >= 0) ? p[sy * PANEL_W + sx] : 0;
    }
  }
}

static ProtectConfig protectConfigOf(const Settings &s) {
  ProtectConfig p;
  p.brightness = s.brightness;
  p.drift = s.drift;
  p.dim = s.dimOn;
  p.dimAfterMin = s.dimAfterMin;
  p.dimLevel = s.dimLevel;
  p.night = s.nightOn;
  p.nightStartMin = s.nightStartMin;
  p.nightEndMin = s.nightEndMin;
  p.rotate = s.rotateOn;
  p.rotateMin = s.rotateMin;
  return p;
}

// What protection treats each screen as: planes are activity and never dimmed, headlines get the owner's brightness
// for their turn, weather and clock dim, everything else (setup, boot, no Wi-Fi ...) is left alone.
static PictureKind pictureKindOf(Screen s) {
  switch (s) {
    case Screen::Flight: return PictureKind::Plane;
    case Screen::News: return PictureKind::Headline;
    case Screen::Weather:
    case Screen::Idle: return PictureKind::Static;
    default: return PictureKind::Other;
  }
}

static const char *panelModeName(PanelMode m) {
  switch (m) {
    case PanelMode::Normal: return "Normal";
    case PanelMode::Dim: return "Dim";
    case PanelMode::Night: return "Night";
    case PanelMode::NightAwake: return "NightAwake";
  }
  return "?";
}

// One Serial line when the mode or the drift offset changes, printed once the fade has settled (about a second later)
// so the brightness shown is the level it is heading for. A change that reverts before then prints nothing.
static void logProtect(const ProtectOut &out, uint32_t now) {
  static PanelMode shownMode = PanelMode::Normal;
  static int shownDx = 0, shownDy = 0;
  static bool pending = false;
  static uint32_t changedAt = 0;
  bool differs = out.mode != shownMode || out.dx != shownDx || out.dy != shownDy;
  if (!differs) {
    pending = false;
  } else if (!pending) {
    pending = true;
    changedAt = now;
  } else if (now - changedAt >= 1200) {
    Serial.printf("PROTECT mode=%s brightness=%d drift=%d,%d\n", panelModeName(out.mode), out.brightness, out.dx, out.dy);
    shownMode = out.mode;
    shownDx = out.dx;
    shownDy = out.dy;
    pending = false;
  }
}

// One Serial line when the picture changes (another screen, or another plane in a cycle), for soak tests and bench checks.
static void logShown(Screen s, const char *callsign) {
  static Screen lastScreen = Screen::Boot;
  static char lastCs[10] = "";
  static bool first = true;
  if (!first && s == lastScreen && strcmp(callsign, lastCs) == 0) return;
  first = false;
  lastScreen = s;
  strlcpy(lastCs, callsign, sizeof(lastCs));
  static const char *const names[] = {"flight", "weather", "news", "idle", "setup", "boot", "connected", "stale", "nowifi", "problem", "update code", "update progress"};
  Serial.printf("SHOW %s %s\n", names[(int)s], callsign);
}

static void renderTask(void *) {
  ProtomatterStatus st = matrix.begin();
  Serial.printf("matrix.begin() = %d\n", (int)st);
  gMatrixOk = (st == PROTOMATTER_OK);
  matrix.cp437(true);  // raw CP437 codes so the degree sign (248) draws as itself
  uint32_t onlineSince = 0;
  NetState lastState = NetState::Connecting;
  for (;;) {
    if (gMatrixOk) {
      uint32_t now = millis();
      NetInfo ni = netInfo();
      Settings cfg = settings::get();
      LiveView lv = liveView(now, cfg);
      if (ni.state != lastState) {
        lastState = ni.state;
        Serial.printf("NET state %d\n", (int)ni.state);
      }
      UiState ui = {ni.apSsid[0] ? ni.apSsid : "ledmatrix", ni.apPass[0] ? ni.apPass : "7K3PQ9XM",
                    ni.apIp[0] ? ni.apIp : "192.168.4.1", ni.ip[0] ? ni.ip : "192.168.1.100", cfg.clock24h};

      const ProtectConfig pcfg = protectConfigOf(cfg);
      Screen screen;
      uint8_t layout = 0;
      int flightIdx = 0;
      PlaneView pv;
      bool live = false;
      if (ni.state == NetState::Online) {
        if (onlineSince == 0) onlineSince = now;
      } else {
        onlineSince = 0;
      }
      if (ni.state == NetState::Setup) {
        screen = Screen::Setup;
      } else if (ni.state == NetState::Connecting) {
        screen = Screen::Boot;
      } else if (ni.state == NetState::Offline) {
        screen = Screen::NoWifi;
      } else if (now - onlineSince < CONNECTED_SCREEN_MS) {
        screen = Screen::Connected;
      } else if (!lv.configured) {
        Item list[4];  // no data service saved: example data
        int n = buildPlaylist(cfg, list);
        const Item &it = list[gIndex % n];
        screen = it.screen;
        layout = it.layout;
        flightIdx = gPass;
      } else {
        live = true;
        if (lv.showPlane) {
          planeToView(lv.plane, pv, now);
          ui.flight = &pv.f;
          screen = Screen::Flight;
          layout = cfg.flightLayout;
        } else if (lv.showNews) {
          ui.headline = lv.news.title;
          ui.headlineSource = lv.news.source;
          ui.headlineAgeMs = lv.newsAgeMs;
          screen = Screen::News;
          layout = cfg.newsLayout;
        } else if (cfg.showWeather && lv.wx.valid && (!cfg.rotateOn || protectWeatherTurn(pcfg, now))) {
          // with rotation on, weather only has the even slots; the clock (or black) idle screen has the others
          ui.wx = &lv.wx;
          screen = Screen::Weather;
          layout = cfg.weatherLayout;
        } else {
          screen = Screen::Idle;
          layout = cfg.idleClock ? 0 : 1;
        }
        if (lv.timeValid) ui.clock = &lv.now;
      }

      // A firmware update takes over the panel (ota.h): the code screen while a code is pending, the progress screen while the
      // image comes in or is installed. Decided before panel protection, so these pictures count as "other" (never dimmed,
      // blanked at night or drifted: the owner is standing in front of the panel). Only while online: setup and the Wi-Fi
      // screens have no update to show. The stale-data dot and the problem message give way too (live is dropped).
      UpdateView uv;
      if (ni.state == NetState::Online) {
        ota::PanelScreen os = ota::panel(now, uv);
        if (os != ota::PanelScreen::None) {
          screen = os == ota::PanelScreen::Code ? Screen::UpdateCode : Screen::UpdateProgress;
          ui.update = &uv;
          live = false;
        }
      }

      ProtectFrame pf;
      pf.nowMs = now;
      pf.timeValid = lv.timeValid;
      pf.localMinute = lv.timeValid ? (uint16_t)(lv.now.tm_hour * 60 + lv.now.tm_min) : 0;
      pf.kind = pictureKindOf(screen);
      ProtectOut out = protectStep(pcfg, pf);
      logProtect(out, now);

      // The problem message (ticket 23) takes the place of weather or the clock for 5 s once a minute, while some kind of data
      // has been failing for an hour and still is. Decided after protectStep, which was given the weather or clock as the
      // picture: the message counts as that static picture (dims after stillness, drifts, does not reset the stillness timer).
      // A plane card or a headline is never replaced, and it stays dark during night blanking (a button press, which opens the
      // 30 s NightAwake window, may show it).
      // The decision itself is problemMessageWanted() (problems.h), which the laptop tests cover.
      if (problemMessageWanted(live, lv.problemMessage, now, screen == Screen::Weather || screen == Screen::Idle,
                               out.mode == PanelMode::Night))
        screen = Screen::Problem;
      logShown(screen, live && screen == Screen::Flight ? lv.plane.cs : "");

      matrix.fillScreen(0);
      drawScreen(screen, layout, now, flightIdx, ui);
      shiftPicture(out.dx, out.dy);  // drift moves the picture only; the overlays below stay at their fixed positions
      if (live && lv.stale && (now / 700) % 2 == 0) matrix.fillRect(60, 1, 3, 3, matrix.color565(255, 120, 20));  // stale-data dot
      int hold = gHoldPct;
      if (hold > 8) matrix.fillRect(0, 31, PANEL_W * hold / 100, 1, matrix.color565(255, 176, 0));  // long-press progress
      applyBrightness(out.brightness);
      matrix.show();
      static uint32_t lastShow = 0;
      uint32_t t = millis();
      if (ni.state == NetState::Online && lastShow) {
        uint32_t gap = t - lastShow;
        if (gap > gFrameMaxMs) gFrameMaxMs = gap;
        if (gap > 1000) gFramesOver1s = gFramesOver1s + 1;
      }
      lastShow = ni.state == NetState::Online ? t : 0;
      gFrames = gFrames + 1;
    }
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}

static void goTo(int index) {
  Item list[4];
  int n = buildPlaylist(settings::get(), list);
  int next = ((index % n) + n) % n;
  if (next == 0 && index > gIndex) gPass = gPass + 1;
  gIndex = next;
  Serial.printf("SCREEN %d/%d %s\n", next + 1, n, list[next].name);
}

void setup() {
  Serial.begin(115200);
  ota::probationBoot();  // FIRST: an image on probation counts this boot (more than 3 crashes: back to the previous image), see ota.h
  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  delay(1500);
  Serial.println("\nledmatrix: slice 4, live data");
  settings::begin();  // loads the saved settings; must run before the tasks read them
  identity::begin();  // reads this board's identity record once; must run before the net and data tasks use it
  ota::begin();       // the update module's lock and saved state; must run before the net and data tasks use it
  netBegin();         // must run before the render task asks for the network state
  liveBegin();        // must run before the render task asks for the live view
  xTaskCreatePinnedToCore(renderTask, "render", 10240, nullptr, 2, nullptr, 1);
}

void loop() {
  static bool upWas = false, downWas = false;
  static uint32_t lastChange = millis(), upSince = 0;
  bool up = digitalRead(BUTTON_UP) == LOW;
  bool down = digitalRead(BUTTON_DOWN) == LOW;
  if (up && !upWas) {
    protectWake(millis());  // a short press wakes the panel
    goTo(gIndex + 1);
    lastChange = millis();
    upSince = millis();
  }
  if (down && !downWas) {
    protectWake(millis());
    goTo(gIndex - 1);
    lastChange = millis();
  }
  if (up) {
    uint32_t held = millis() - upSince;
    gHoldPct = (int)min<uint32_t>(100, held * 100 / HOLD_TO_RESET_MS);
    if (held >= HOLD_TO_RESET_MS) {
      Serial.println("UP held: forgetting Wi-Fi and restarting into setup");
      netWipeAndRestart();
    }
  } else {
    gHoldPct = 0;
  }
  upWas = up;
  downWas = down;
  if (millis() - lastChange >= (uint32_t)settings::get().dwellSec * 1000) {
    goTo(gIndex + 1);
    lastChange = millis();
  }
  delay(20);
}
