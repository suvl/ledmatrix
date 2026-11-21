// LedMatrix firmware.
//   slice 1: every screen and layout on the real panel, with example data
//   slice 2: Wi-Fi setup flow: saved network, setup access point, mDNS, long-press UP to reset
//   slice 3: settings portal (this file picks screens from the saved settings; see net.cpp and portal/)
//
//   core 1: render task   ~30 fps, picks the screen from the network state and the saved settings
//   core 0: net task      Wi-Fi, setup access point, DNS, web server and portal API (see net.cpp)
//   loop(): buttons       UP = next screen, DOWN = previous; hold UP for 5 s to forget the Wi-Fi network

#include <Adafruit_Protomatter.h>

#include "display.h"
#include "net.h"
#include "screens.h"
#include "settings.h"

// ---- panel wiring (Matrix Portal S3), 64x32 uses address lines A-D --------------------------
static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35};
static const uint8_t clockPin = 2;
static const uint8_t latchPin = 47;
static const uint8_t oePin = 14;
Adafruit_Protomatter matrix(64, 4, 1, rgbPins, 4, addrPins, clockPin, latchPin, oePin, true);

static const uint8_t BUTTON_UP = 6;    // per the board's Arduino pin definitions
static const uint8_t BUTTON_DOWN = 7;
static const uint32_t HOLD_TO_RESET_MS = 5000;
static const uint32_t CONNECTED_SCREEN_MS = 5000;

// ---- demo playlist: the enabled screens with their chosen layouts, until live data arrives (slice 4) ----
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
      if (ni.state != lastState) {
        lastState = ni.state;
        Serial.printf("NET state %d\n", (int)ni.state);
      }
      UiState ui = {ni.apSsid[0] ? ni.apSsid : "ledmatrix", ni.apPass[0] ? ni.apPass : "7K3PQ9XM",
                    ni.apIp[0] ? ni.apIp : "192.168.4.1", ni.ip[0] ? ni.ip : "192.168.1.100", cfg.clock24h};

      Screen screen;
      uint8_t layout = 0;
      int flightIdx = 0;
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
      } else {
        Item list[4];
        int n = buildPlaylist(cfg, list);
        const Item &it = list[gIndex % n];
        screen = it.screen;
        layout = it.layout;
        flightIdx = gPass;
      }

      matrix.fillScreen(0);
      drawScreen(screen, layout, now, flightIdx, ui);
      int hold = gHoldPct;
      if (hold > 8) matrix.fillRect(0, 31, PANEL_W * hold / 100, 1, matrix.color565(255, 176, 0));  // long-press progress
      matrix.show();
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
  Serial.println("\nledmatrix: slice 3, settings portal");
  settings::begin();  // loads the saved settings; must run before the tasks read them
  netBegin();         // must run before the render task asks for the network state
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, nullptr, 1);
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
