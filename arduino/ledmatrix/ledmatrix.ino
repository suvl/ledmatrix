// LedMatrix firmware, slice 1: every screen and layout on the real panel, with example data.
//
//   core 1: render task  ~30 fps, draws the current screen
//   loop(): buttons      UP = next screen, DOWN = previous screen; the demo also advances by itself
//
// Later slices add settings, the Wi-Fi setup flow, the portal and live data.

#include <Adafruit_Protomatter.h>

#include "display.h"
#include "screens.h"

// ---- panel wiring (Matrix Portal S3), 64x32 uses address lines A-D --------------------------
static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35};
static const uint8_t clockPin = 2;
static const uint8_t latchPin = 47;
static const uint8_t oePin = 14;
Adafruit_Protomatter matrix(64, 4, 1, rgbPins, 4, addrPins, clockPin, latchPin, oePin, true);

static const uint8_t BUTTON_UP = 6;    // per the board's Arduino pin definitions
static const uint8_t BUTTON_DOWN = 7;

// ---- demo playlist: one entry per screen/layout ------------------------------------------------
struct Item {
  Screen screen;
  uint8_t layout;
  const char *name;
};
static const Item PLAYLIST[] = {
  {Screen::Flight, 0, "flight A"}, {Screen::Flight, 1, "flight B"}, {Screen::Flight, 2, "flight C"},
  {Screen::Weather, 0, "weather A"}, {Screen::Weather, 1, "weather B"}, {Screen::Weather, 2, "weather C"},
  {Screen::News, 0, "headlines A"}, {Screen::News, 1, "headlines B"},
  {Screen::Idle, 0, "idle clock"}, {Screen::Idle, 1, "idle black"},
  {Screen::Setup, 0, "setup B"},
  {Screen::Boot, 0, "status boot"}, {Screen::Connected, 0, "status connected"},
  {Screen::Stale, 0, "status stale"}, {Screen::NoWifi, 0, "status no wifi"},
};
static const int PLAYLIST_LEN = sizeof(PLAYLIST) / sizeof(PLAYLIST[0]);
static const uint32_t AUTO_ADVANCE_MS = 8000;

static volatile int gIndex = 0;
static volatile int gPass = 0;  // increments each time the playlist wraps; picks the example flight
static volatile bool gMatrixOk = false;

static void renderTask(void *) {
  ProtomatterStatus st = matrix.begin();
  Serial.printf("matrix.begin() = %d\n", (int)st);
  gMatrixOk = (st == PROTOMATTER_OK);
  matrix.cp437(true);  // raw CP437 codes so the degree sign (248) draws as itself
  for (;;) {
    if (gMatrixOk) {
      const Item &it = PLAYLIST[gIndex];
      matrix.fillScreen(0);
      drawScreen(it.screen, it.layout, millis(), gPass + (it.screen == Screen::Flight ? it.layout : 0));
      matrix.show();
    }
    vTaskDelay(pdMS_TO_TICKS(33));
  }
}

static void goTo(int index) {
  int n = PLAYLIST_LEN;
  int next = ((index % n) + n) % n;
  if (next == 0 && index > gIndex) gPass = gPass + 1;
  gIndex = next;
  Serial.printf("SCREEN %d/%d %s\n", next + 1, n, PLAYLIST[next].name);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  delay(1500);
  Serial.println("\nledmatrix slice 1: screens on the real panel");
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, nullptr, 1);
}

void loop() {
  static bool upWas = false, downWas = false;
  static uint32_t lastChange = millis();
  bool up = digitalRead(BUTTON_UP) == LOW;
  bool down = digitalRead(BUTTON_DOWN) == LOW;
  if (up && !upWas) {
    goTo(gIndex + 1);
    lastChange = millis();
  }
  if (down && !downWas) {
    goTo(gIndex - 1);
    lastChange = millis();
  }
  upWas = up;
  downWas = down;
  if (millis() - lastChange >= AUTO_ADVANCE_MS) {
    goTo(gIndex + 1);
    lastChange = millis();
  }
  delay(20);
}
