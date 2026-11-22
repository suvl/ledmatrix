// LedMatrix firmware.
//   slice 1: every screen and layout on the real panel, with example data
//   slice 2: Wi-Fi setup flow: saved network, setup access point, mDNS, long-press UP to reset
//   slice 3: settings portal
//   slice 4: live data from the owner's service (this file decides what to show; see live.cpp)
//
//   core 1: render task   ~30 fps, picks the screen from the network state, the settings and the live data
//   core 0: net task      Wi-Fi, setup access point, DNS, web server and portal API (see net.cpp)
//   core 0: data task     time sync, flights every 30 s, weather every 15 min, headlines every 30 min (live.cpp)
//   loop(): buttons       UP = next demo screen, DOWN = previous; hold UP for 5 s to forget the Wi-Fi network
//
// With no data service address saved, the device cycles the demo screens with example data.

#include <Adafruit_Protomatter.h>

#include "display.h"
#include "live.h"
#include "net.h"
#include "plane_view.h"
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
  uint32_t k = (uint32_t)percent * 256 / 100;  // 8.8 fixed point
  uint16_t *p = matrix.getBuffer();
  for (int i = 0; i < PANEL_W * PANEL_H; i++) {
    uint16_t c = p[i];
    if (!c) continue;
    uint32_t r = ((c >> 11) & 31) * k >> 8, g = ((c >> 5) & 63) * k >> 8, b = (c & 31) * k >> 8;
    p[i] = (r << 11) | (g << 5) | b;
  }
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
          planeToView(lv.plane, pv);
          ui.flight = &pv.f;
          screen = Screen::Flight;
          layout = cfg.flightLayout;
        } else if (lv.showNews) {
          ui.headline = lv.news.title;
          ui.headlineSource = lv.news.source;
          ui.headlineAgeMs = lv.newsAgeMs;
          screen = Screen::News;
          layout = cfg.newsLayout;
        } else if (cfg.showWeather && lv.wx.valid) {
          ui.wx = &lv.wx;
          screen = Screen::Weather;
          layout = cfg.weatherLayout;
        } else {
          screen = Screen::Idle;
          layout = cfg.idleClock ? 0 : 1;
        }
        if (lv.timeValid) ui.clock = &lv.now;
      }

      matrix.fillScreen(0);
      drawScreen(screen, layout, now, flightIdx, ui);
      if (live && lv.stale && (now / 700) % 2 == 0) matrix.fillRect(60, 1, 3, 3, matrix.color565(255, 120, 20));  // stale-data dot
      int hold = gHoldPct;
      if (hold > 8) matrix.fillRect(0, 31, PANEL_W * hold / 100, 1, matrix.color565(255, 176, 0));  // long-press progress
      applyBrightness(cfg.brightness);
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
  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  delay(1500);
  Serial.println("\nledmatrix: slice 4, live data");
  settings::begin();  // loads the saved settings; must run before the tasks read them
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
    goTo(gIndex + 1);
    lastChange = millis();
    upSince = millis();
  }
  if (down && !downWas) {
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
