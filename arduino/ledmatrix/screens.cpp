#include "screens.h"

#include <Arduino.h>
#include <Fonts/TomThumb.h>
#include <math.h>

#include "art.h"
#include "display.h"
#include "sample_data.h"

namespace {

constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t WHITE = c565(255, 255, 255);
constexpr uint16_t AMBER = c565(255, 176, 0);
constexpr uint16_t BLUE = c565(40, 110, 255);
constexpr uint16_t GREEN = c565(0, 220, 90);
constexpr uint16_t RED = c565(255, 40, 40);
constexpr uint16_t GREY = c565(130, 130, 140);
constexpr uint16_t CYAN = c565(0, 200, 230);
constexpr uint16_t DARKBLUE = c565(0, 40, 90);
constexpr uint16_t ORANGE = c565(255, 120, 20);

const char DEG6 = (char)248;  // degree sign in the default 6x8 font (CP437 table)
// Tom Thumb's extended glyphs (with the degree sign) are switched off in the library header, so in
// 4x6 strings a '^' stands for a degree ring that tt() draws pixel by pixel.

// ---- text helpers: draw a string that must fit maxW pixels, cut it like the panel would, log once ------
String seen[8];
int seenCount = 0;
void warnCut(const String &full, const String &cut) {
  for (int i = 0; i < seenCount; i++)
    if (seen[i] == full) return;
  if (seenCount < 8) seen[seenCount++] = full;
  Serial.printf("TRUNC \"%s\" -> \"%s\"\n", full.c_str(), cut.c_str());
}

void useDefaultFont(int size = 1) {
  matrix.setFont(nullptr);
  matrix.setTextSize(size);
  matrix.setTextWrap(false);
}
void useTinyFont() {
  matrix.setFont(&TomThumb);
  matrix.setTextSize(1);
  matrix.setTextWrap(false);
}
int tinyWidth(const String &s) {
  useTinyFont();
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return x1 + (int)w;
}
int centerX(int w) {
  int x = (PANEL_W - w) / 2;
  return x < 0 ? 0 : x;
}

// 6x8 font. y is the top of the glyphs. Returns the pixel width drawn.
int t6(int x, int y, const String &s, uint16_t color, int maxW, int size = 1) {
  String t = s;
  while (t.length() && (int)t.length() * 6 * size > maxW) t.remove(t.length() - 1);
  if (t != s) warnCut(s, t);
  useDefaultFont(size);
  matrix.setCursor(x, y);
  matrix.setTextColor(color);
  matrix.print(t);
  return (int)t.length() * 6 * size;
}
// Width of a 4x6 string where '^' is a 3 px degree ring followed by a 1 px gap.
int ttWidth(const String &s) {
  int i = s.indexOf('^');
  if (i < 0) return tinyWidth(s);
  String left = s.substring(0, i);
  return tinyWidth(left) + (left.length() ? 1 : 0) + 4 + ttWidth(s.substring(i + 1));
}
// 4x6 font. base is the baseline row (glyphs occupy rows base-5 .. base-1).
void tt(int x, int base, const String &s, uint16_t color, int maxW) {
  String t = s;
  while (t.length() && ttWidth(t) > maxW) t.remove(t.length() - 1);
  if (t != s) warnCut(s, t);
  int cx = x;
  String rest = t;
  for (;;) {
    int i = rest.indexOf('^');
    String part = i < 0 ? rest : rest.substring(0, i);
    useTinyFont();
    matrix.setCursor(cx, base);
    matrix.setTextColor(color);
    matrix.print(part);
    if (i < 0) break;
    cx += tinyWidth(part) + (part.length() ? 1 : 0);
    int y = base - 5;
    matrix.drawPixel(cx + 1, y, color);
    matrix.drawPixel(cx, y + 1, color);
    matrix.drawPixel(cx + 2, y + 1, color);
    matrix.drawPixel(cx + 1, y + 2, color);
    cx += 4;
    rest = rest.substring(i + 1);
  }
  useDefaultFont();
}

String degreeTemp6(const char *num) {
  String s(num);
  s += DEG6;
  s += 'C';
  return s;
}

// ---- flight card ------------------------------------------------------------------------------------------
void flightA(const SampleFlight &f) {
  drawLogo(1, 0, f.logo);
  t6(19, 1, f.airline, WHITE, 44);
  t6(19, 9, f.route, CYAN, 44);
  t6(1, 17, f.type, AMBER, 62);
  tt(1, 30, String(f.alt) + " " + f.spdVal + f.spdUnit, GREY, 62);
}
void flightB(const SampleFlight &f) {
  drawLogo(1, 0, f.logo);
  t6(19, 1, f.airline, WHITE, 44);
  t6(19, 9, f.route, CYAN, 44);
  tt(1, 23, f.type, AMBER, 62);
  tt(1, 30, String("to ") + f.destName, GREY, 62);
}
void flightC(const SampleFlight &f) {
  t6(2, 1, f.airline, WHITE, 60);
  t6(2, 9, f.route, CYAN, 60);
  tt(2, 21, f.type, AMBER, 60);
  tt(2, 27, String("alt ") + f.alt + " spd " + f.spdVal, GREY, 60);
  tt(2, 32, String("trk ") + f.trackDeg + '^' + " vr " + f.vr, GREY, 60);
}

// ---- weather ------------------------------------------------------------------------------------------------
void weatherA() {
  drawWxIcon(1, 2, WxKind::PartCloud, 16);
  t6(21, 3, "18", WHITE, 26, 2);
  t6(45, 3, String(DEG6), WHITE, 6);
  t6(51, 3, "C", WHITE, 6);
  tt(2, 25, "Partly cloudy", AMBER, 60);
  tt(2, 31, "H21 L12 rain 10%", GREY, 60);
}
void weatherB() {
  tt(2, 7, "HOME", AMBER, 60);
  String t = degreeTemp6("18");
  t6(centerX((int)t.length() * 12), 10, t, WHITE, 62, 2);
  String c = "Partly cloudy";
  tt(centerX(tinyWidth(c)), 31, c, GREY, 62);
}
void weatherC() {
  struct Slot {
    const char *label;
    WxKind kind;
    const char *temp;
  };
  static const Slot slots[3] = {{"NOW", WxKind::PartCloud, "18"}, {"+3H", WxKind::Sun, "21"}, {"+6H", WxKind::Rain, "15"}};
  for (int i = 0; i < 3; i++) {
    int x0 = 2 + i * 21;
    tt(x0 + 2, 7, slots[i].label, AMBER, 20);
    drawWxIcon(x0 + 3, 10, slots[i].kind, 12);
    String t = String(slots[i].temp) + '^';
    tt(x0 + (19 - ttWidth(t)) / 2, 29, t, WHITE, 20);
  }
}

// ---- headlines ----------------------------------------------------------------------------------------------
int wrapLines(const String &text, int maxChars, String out[], int maxLines) {
  int n = 0, start = 0;
  String cur = "";
  while (start <= (int)text.length()) {
    int sp = text.indexOf(' ', start);
    String w = sp < 0 ? text.substring(start) : text.substring(start, sp);
    String cand = cur.length() ? cur + " " + w : w;
    if ((int)cand.length() > maxChars && cur.length()) {
      if (n < maxLines) out[n++] = cur;
      cur = w;
    } else {
      cur = cand;
    }
    if (sp < 0) break;
    start = sp + 1;
  }
  if (cur.length() && n < maxLines) out[n++] = cur;
  return n;
}
void newsA(float t) {
  String h = SAMPLE_HEADLINES[(int)(t / 14) % SAMPLE_HEADLINE_COUNT];
  matrix.fillRect(0, 0, PANEL_W, 8, DARKBLUE);
  tt(2, 6, "NEWS", WHITE, 60);
  float wpx = (float)h.length() * 6;
  int x = (int)(PANEL_W - (fmodf(t, 14.0f) / 14.0f) * (PANEL_W + wpx));
  useDefaultFont();
  matrix.setCursor(x, 16);
  matrix.setTextColor(AMBER);
  matrix.print(h);
}
void newsB(float t) {
  String h = SAMPLE_HEADLINES[(int)(t / 4) % SAMPLE_HEADLINE_COUNT];
  tt(2, 6, "NEWS", BLUE, 60);
  String lines[4];
  int n = wrapLines(h, 15, lines, 4);
  for (int i = 0; i < n; i++) tt(2, 13 + i * 6, lines[i], WHITE, 62);
}

// ---- idle ---------------------------------------------------------------------------------------------------
void idleClock(float t, uint32_t ms) {
  // Demo clock: starts at 21:47 and advances with uptime. Real time (SNTP, timezone) comes in slice 4.
  uint32_t minutes = 21 * 60 + 47 + ms / 60000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u%c%02u", (unsigned)((minutes / 60) % 24), ((int)(t * 2) % 2 == 0) ? ':' : ' ', (unsigned)(minutes % 60));
  t6(2, 4, buf, WHITE, 64, 2);
  String date = "SAT 21 NOV";
  tt(centerX(tinyWidth(date)), 29, date, GREY, 62);
}

// ---- setup mode, layout B -------------------------------------------------------------------------------------
void setupB(float t) {
  int p = (int)(t / 3) % 3;
  switch (p) {
    case 0: t6(2, 4, "JOIN WIFI", AMBER, 62); t6(2, 16, "ledmatrix", WHITE, 62); break;
    case 1: t6(2, 4, "PASSWORD", AMBER, 62); t6(2, 16, "7K3PQ9XM", WHITE, 62); break;
    default: t6(2, 4, "THEN OPEN", AMBER, 62); tt(2, 22, "192.168.4.1", CYAN, 62); break;
  }
  for (int i = 0; i < 3; i++) matrix.fillRect(24 + i * 6, 29, 4, 2, i == p ? AMBER : c565(60, 60, 60));
}

// ---- status screens ---------------------------------------------------------------------------------------------
void bootScreen(float t) {
  String name = "ledmatrix";
  t6(centerX((int)name.length() * 6), 7, name, WHITE, 62);
  String dots = String("connecting") + String("...").substring(0, 1 + (int)(t * 2) % 3);
  tt(14, 24, dots, GREY, 50);
}
void connectedScreen() {
  tt(2, 8, "WIFI OK", GREEN, 60);
  tt(2, 16, "192.168.1.100", WHITE, 62);
  tt(2, 24, "ledmatrix.local", CYAN, 62);
}
void staleScreen(float t) {
  drawWxIcon(4, 8, WxKind::PartCloud, 16);
  t6(26, 10, "18", WHITE, 26, 2);
  t6(50, 10, String(DEG6), WHITE, 6);
  if ((int)(t * 1.5f) % 2 == 0) matrix.fillRect(60, 1, 3, 3, ORANGE);
  tt(2, 30, "stale data dot", GREY, 62);
}
void noWifiScreen() {
  for (int i = 0; i < 12; i++) {
    matrix.drawPixel(3 + i, 9 + i, RED);
    matrix.drawPixel(14 - i, 9 + i, RED);
    matrix.drawPixel(4 + i, 9 + i, RED);
    matrix.drawPixel(13 - i, 9 + i, RED);
  }
  tt(20, 10, "NO WIFI", RED, 44);
  tt(20, 17, "hold UP 5s", WHITE, 44);
  tt(20, 24, "to set up", GREY, 44);
}

}  // namespace

void drawScreen(Screen s, uint8_t layout, uint32_t ms, int flightIdx) {
  float t = ms / 1000.0f;
  const SampleFlight &f = SAMPLE_FLIGHTS[((flightIdx % SAMPLE_FLIGHT_COUNT) + SAMPLE_FLIGHT_COUNT) % SAMPLE_FLIGHT_COUNT];
  switch (s) {
    case Screen::Flight:
      if (layout == 0) flightA(f);
      else if (layout == 1) flightB(f);
      else flightC(f);
      break;
    case Screen::Weather:
      if (layout == 0) weatherA();
      else if (layout == 1) weatherB();
      else weatherC();
      break;
    case Screen::News:
      if (layout == 0) newsA(t);
      else newsB(t);
      break;
    case Screen::Idle:
      if (layout == 0) idleClock(t, ms);  // layout 1 is the clean black screen: draw nothing
      break;
    case Screen::Setup: setupB(t); break;
    case Screen::Boot: bootScreen(t); break;
    case Screen::Connected: connectedScreen(); break;
    case Screen::Stale: staleScreen(t); break;
    case Screen::NoWifi: noWifiScreen(); break;
  }
}
