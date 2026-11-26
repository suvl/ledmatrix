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

// A real logo, or (demo data only) the placeholder art, or nothing: a plane with no logo gets the whole width for text.
static bool flightHasLogo(const SampleFlight &f) { return f.logoPx || !f.code; }
static void drawFlightLogo(const SampleFlight &f) {
  if (f.logoPx) drawLogoPixels(1, 0, f.logoPx);
  else if (!f.code) drawLogo(1, 0, f.logo);
}

// ---- flight card ------------------------------------------------------------------------------------------
// Pixel drift shifts the finished picture one pixel right and down, so column 63 and row 31 must stay black. Text that
// starts at column tx gets 63 - tx pixels (it ends at column 62 at the latest). In the 4x6 font the descenders g j p q y
// reach the baseline row itself, so a line that can hold them has its baseline at row 30 at most.
void flightA(const SampleFlight &f) {
  drawFlightLogo(f);
  int x = flightHasLogo(f) ? 19 : 2, w = flightHasLogo(f) ? 44 : 60, tx = flightHasLogo(f) ? 1 : 2;
  t6(x, 1, f.airline, WHITE, w);
  t6(x, 9, f.route, CYAN, w);
  t6(tx, 17, f.type, AMBER, 62);
  tt(tx, 30, String(f.alt) + " " + f.spdVal + f.spdUnit, GREY, 63 - tx);
}
void flightB(const SampleFlight &f) {
  drawFlightLogo(f);
  int x = flightHasLogo(f) ? 19 : 2, w = flightHasLogo(f) ? 44 : 60, tx = flightHasLogo(f) ? 1 : 2;
  t6(x, 1, f.airline, WHITE, w);
  t6(x, 9, f.route, CYAN, w);
  tt(tx, 23, f.type, AMBER, 63 - tx);
  tt(tx, 30, String("to ") + f.destName, GREY, 63 - tx);
}
void flightC(const SampleFlight &f) {
  t6(2, 0, f.airline, WHITE, 60);
  t6(2, 8, f.route, CYAN, 60);
  tt(2, 20, f.type, AMBER, 60);
  tt(2, 26, String(f.alt) + " " + f.spdVal + f.spdUnit, GREY, 60);
  tt(2, 31, String(f.trackDeg) + '^' + " " + f.vr, GREY, 60);  // digits and f, t, s, /: nothing descends below baseline 31
}

// ---- weather ------------------------------------------------------------------------------------------------
WxKind iconKind(const char *icon) {
  if (strcmp(icon, "sun") == 0) return WxKind::Sun;
  if (strcmp(icon, "part") == 0) return WxKind::PartCloud;
  if (strcmp(icon, "rain") == 0) return WxKind::Rain;
  return WxKind::Cloud;
}
const char *iconText(const char *icon) {
  if (strcmp(icon, "sun") == 0) return "Clear";
  if (strcmp(icon, "part") == 0) return "Partly cloudy";
  if (strcmp(icon, "rain") == 0) return "Rain";
  return "Cloudy";
}
// The example values the screens show when no service is configured.
LiveWeather demoWeather() {
  LiveWeather w;
  w.valid = true;
  strlcpy(w.loc, "HOME", sizeof(w.loc));
  w.hasT = w.hasHi = w.hasLo = w.hasPop = w.hasH3 = w.hasH6 = true;
  w.t = 18; w.hi = 21; w.lo = 12; w.pop = 10; w.h3t = 21; w.h6t = 15;
  strlcpy(w.icon, "part", sizeof(w.icon));
  strlcpy(w.h3i, "sun", sizeof(w.h3i));
  strlcpy(w.h6i, "rain", sizeof(w.h6i));
  return w;
}
String tempText(bool has, int v) { return has ? String(v) : String("--"); }

void weatherA(const LiveWeather &w) {
  drawWxIcon(1, 2, iconKind(w.icon), 16);
  String t = tempText(w.hasT, w.t);
  int x = t.length() == 1 ? 27 : 21;
  t6(x, 3, t, WHITE, 26, 2);
  t6(45, 3, String(DEG6), WHITE, 6);
  t6(51, 3, "C", WHITE, 6);
  tt(2, 25, iconText(w.icon), AMBER, 60);
  String line = String("H") + (w.hasHi ? String(w.hi) : String("-")) + " L" + (w.hasLo ? String(w.lo) : String("-"));
  if (w.hasPop) line += " " + String(w.pop) + "%";
  tt(2, 31, line, GREY, 60);
}
void weatherB(const LiveWeather &w) {
  String loc = w.loc[0] ? String(w.loc) : String("WEATHER");
  loc.toUpperCase();
  tt(2, 7, loc, AMBER, 60);
  String t = tempText(w.hasT, w.t);
  t += DEG6;
  t += 'C';
  t6(centerX((int)t.length() * 12), 10, t, WHITE, 62, 2);
  String c = iconText(w.icon);
  tt(centerX(tinyWidth(c)), 30, c, GREY, 62);  // baseline 30: the y of "Cloudy" descends to the baseline row, row 31 stays black
}
// The three columns are labelled with the clock hour (24 h, board local time) each one refers to: now, +3 h, +6 h.
// Capital N, M and W cannot be told apart in the 3 px font, digits and "h" can. No clock yet: "--h".
void weatherC(const LiveWeather &w, const struct tm *clock) {
  struct Slot {
    const char *icon;
    bool has;
    int temp;
  };
  Slot slots[3] = {{w.icon, w.hasT, w.t}, {w.h3i, w.hasH3, w.h3t}, {w.h6i, w.hasH6, w.h6t}};
  for (int i = 0; i < 3; i++) {
    int x0 = 2 + i * 21;
    char label[4] = "--h";
    if (clock) snprintf(label, sizeof(label), "%02dh", (clock->tm_hour + 3 * i) % 24);
    tt(x0 + (19 - ttWidth(label)) / 2, 7, label, AMBER, 19);  // columns are 19 px wide: the last one ends at column 62
    drawWxIcon(x0 + 3, 10, iconKind(slots[i].icon), 12);
    String t = (slots[i].has ? String(slots[i].temp) : String("--")) + '^';
    tt(x0 + (19 - ttWidth(t)) / 2, 29, t, WHITE, 19);
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
void newsA(float t, const UiState &ui) {
  String h = ui.headline ? String(ui.headline) : String(SAMPLE_HEADLINES[(int)(t / 14) % SAMPLE_HEADLINE_COUNT]);
  String label = (ui.headlineSource && ui.headlineSource[0]) ? String(ui.headlineSource) : String("NEWS");
  matrix.fillRect(0, 0, PANEL_W, 8, DARKBLUE);
  tt(2, 6, label, WHITE, 60);
  float wpx = (float)h.length() * 6;
  int x = ui.headline ? (int)(PANEL_W - (ui.headlineAgeMs / 1000.0f) * 24.0f)  // live: starts at the right edge, 24 px per second
                      : (int)(PANEL_W - (fmodf(t, 14.0f) / 14.0f) * (PANEL_W + wpx));
  useDefaultFont();
  matrix.setCursor(x, 16);
  matrix.setTextColor(AMBER);
  matrix.print(h);
}
void newsB(float t, const UiState &ui) {
  String h = ui.headline ? String(ui.headline) : String(SAMPLE_HEADLINES[(int)(t / 4) % SAMPLE_HEADLINE_COUNT]);
  String label = (ui.headlineSource && ui.headlineSource[0]) ? String(ui.headlineSource) : String("NEWS");
  tt(2, 6, label, BLUE, 60);
  String lines[12];
  int n = wrapLines(h, 15, lines, 12);
  int pages = (n + 3) / 4;
  int page = pages > 0 ? (int)(((ui.headline ? ui.headlineAgeMs / 1000.0f : t) / 4)) % pages : 0;
  for (int i = 0; i < 4 && page * 4 + i < n; i++) tt(2, 13 + i * 6, lines[page * 4 + i], WHITE, 62);
}

// ---- idle ---------------------------------------------------------------------------------------------------
void idleClock(float t, uint32_t ms, bool h24, const struct tm *tmv) {
  unsigned hour, minute;
  String date;
  if (tmv) {
    hour = tmv->tm_hour;
    minute = tmv->tm_min;
    char buf[20];
    strftime(buf, sizeof(buf), "%a %d %b", tmv);
    date = buf;
    date.toUpperCase();
  } else {
    // Demo clock: starts at 21:47 and advances with uptime.
    uint32_t minutes = 21 * 60 + 47 + ms / 60000;
    hour = (minutes / 60) % 24;
    minute = minutes % 60;
    date = "SAT 21 NOV";
  }
  bool pm = hour >= 12;
  if (!h24) hour = hour % 12 == 0 ? 12 : hour % 12;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02u%c%02u", hour, ((int)(t * 2) % 2 == 0) ? ':' : ' ', minute);
  t6(2, 4, buf, WHITE, 64, 2);
  if (!h24) date += pm ? " PM" : " AM";
  tt(centerX(tinyWidth(date)), 29, date, GREY, 62);
}

// ---- setup mode, layout B -------------------------------------------------------------------------------------
// Static on purpose: the owner has to read the password and type it on a phone, so nothing cycles.
void setupScreen(const UiState &ui) {
  t6(2, 0, ui.apSsid, AMBER, 62);
  tt(2, 14, "WIFI PASSWORD", GREY, 62);
  t6(2, 16, ui.apPass, WHITE, 62);
  tt(2, 30, ui.apIp, CYAN, 62);
}

// ---- status screens ---------------------------------------------------------------------------------------------
void bootScreen(float t) {
  String name = "ledmatrix";
  t6(centerX((int)name.length() * 6), 7, name, WHITE, 62);
  String dots = String("connecting") + String("...").substring(0, 1 + (int)(t * 2) % 3);
  tt(14, 24, dots, GREY, 50);
}
void connectedScreen(const UiState &ui) {
  tt(2, 8, "WIFI OK", GREEN, 60);
  tt(2, 16, ui.staIp, WHITE, 62);
  tt(2, 24, "ledmatrix.local", CYAN, 62);
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

// The message that points the owner to the portal, after something has failed for an hour (ticket 23). Drawn instead of
// weather or the clock for 5 s once a minute, so like them it must keep column 63 and row 31 black for the pixel drift.
void problemScreen() {
  t6(2, 1, "PROBLEM", AMBER, 62);
  tt(2, 15, "Data is failing", WHITE, 60);
  tt(2, 22, "See the portal:", GREY, 60);
  tt(2, 29, "ledmatrix.local", CYAN, 60);
}

// ---- firmware update screens (ticket 33, shared/firmware-update.md) ---------------------------------------------------
// Drawn instead of the normal screens, never dimmed and never drifting, so these two may light column 63 and row 31
// (the countdown bar below does use row 31). They are static apart from the data in UpdateView.
constexpr int UPDATE_CODE_SECONDS = 180;  // how long a code is valid (the contract: 3 minutes); the countdown bar's full length
constexpr uint16_t TRACK = c565(40, 40, 50);

int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Ink width of a string in the 4x6 font at an integer scale (the library scales every pixel).
int ttWidthN(const String &s, int size) {
  useTinyFont();
  matrix.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  useDefaultFont();
  return x1 + (int)w;
}
// The 4x6 font at an integer scale; base is the baseline row (the glyphs reach up to base - 5 * size).
void ttBig(int x, int base, const String &s, uint16_t color, int size) {
  useTinyFont();
  matrix.setTextSize(size);
  matrix.setCursor(x, base);
  matrix.setTextColor(color);
  matrix.print(s);
  useDefaultFont();
}
// Text centred on the panel, in the 4x6 font.
void ttCentered(int base, const String &s, uint16_t color) { tt(centerX(ttWidth(s)), base, s, color, 62); }

// The authorisation code. Six digits do not fit the 6x8 font doubled (72 px), so they are the 4x6 font doubled: 6x10 px
// each, in two groups of three. The code is only ever drawn here, never logged.
void updateCodeScreen(const UpdateView &u) {
  t6(2, 0, "UPDATE", AMBER, 36);
  int left = clampInt(u.secondsLeft, 0, 5999);
  char clockText[8];
  snprintf(clockText, sizeof(clockText), "%d:%02d", left / 60, left % 60);
  tt(62 - ttWidth(clockText), 6, clockText, left <= 30 ? RED : GREY, 30);  // m:ss, its last pixel in column 61

  String code = u.code;
  String a = code.substring(0, 3), b = code.length() > 3 ? code.substring(3) : String("");
  const int gap = b.length() ? 6 : 0;  // blank columns between the two groups
  int wa = ttWidthN(a, 2), wb = b.length() ? ttWidthN(b, 2) : 0;
  int x0 = centerX(wa + gap + wb);
  ttBig(x0, 19, a, WHITE, 2);
  if (b.length()) ttBig(x0 + wa + gap, 19, b, WHITE, 2);

  ttCentered(27, "Type it in portal", GREY);

  // Countdown bar on the last row: one pixel per three seconds, red for the last half minute.
  matrix.fillRect(2, PANEL_H - 1, 60, 1, TRACK);
  int fill = clampInt(left, 0, UPDATE_CODE_SECONDS) * 60 / UPDATE_CODE_SECONDS;
  if (fill > 0) matrix.fillRect(2, PANEL_H - 1, fill, 1, left <= 30 ? RED : AMBER);
}

// Download or install progress. With a version: its number in the 6x8 font, the bar, the status word. Without one (an uploaded
// file of unknown version) the status word takes the place of the number.
void updateProgressScreen(const UpdateView &u) {
  t6(2, 0, "UPDATE", AMBER, 36);
  int pct = clampInt(u.percent, 0, 100);
  String p = String(pct) + "%";
  t6(63 - (int)p.length() * 6, 0, p, WHITE, 26);  // right-aligned: its last pixel in column 61

  bool hasVersion = u.version[0] != 0;
  int barY;
  if (hasVersion) {
    t6(2, 9, String("v") + u.version, WHITE, 60);
    tt(2, 30, u.line, CYAN, 60);
    barY = 18;
  } else {
    t6(2, 10, u.line, WHITE, 60);
    barY = 20;
  }

  matrix.drawRect(2, barY, 60, 6, GREY);
  int fill = pct * 58 / 100;
  if (fill > 0) matrix.fillRect(3, barY + 1, fill, 4, GREEN);
}

}  // namespace

void drawScreen(Screen s, uint8_t layout, uint32_t ms, int flightIdx, const UiState &ui) {
  float t = ms / 1000.0f;
  const SampleFlight &sample = SAMPLE_FLIGHTS[((flightIdx % SAMPLE_FLIGHT_COUNT) + SAMPLE_FLIGHT_COUNT) % SAMPLE_FLIGHT_COUNT];
  const SampleFlight &f = ui.flight ? *ui.flight : sample;
  LiveWeather demo = demoWeather();
  const LiveWeather &w = ui.wx ? *ui.wx : demo;
  // Demo weather shows the same example hours as the preview (21h, 00h, 03h, like the 21:47 demo clock); live weather uses the board clock.
  struct tm demoClock = {};
  demoClock.tm_hour = 21;
  const struct tm *wxClock = ui.wx ? ui.clock : &demoClock;
  switch (s) {
    case Screen::Flight:
      if (layout == 0) flightA(f);
      else if (layout == 1) flightB(f);
      else flightC(f);
      break;
    case Screen::Weather:
      if (layout == 0) weatherA(w);
      else if (layout == 1) weatherB(w);
      else weatherC(w, wxClock);
      break;
    case Screen::News:
      if (layout == 0) newsA(t, ui);
      else newsB(t, ui);
      break;
    case Screen::Idle:
      if (layout == 0) idleClock(t, ms, ui.clock24h, ui.clock);  // layout 1 is the clean black screen: draw nothing
      break;
    case Screen::Setup: setupScreen(ui); break;
    case Screen::Boot: bootScreen(t); break;
    case Screen::Connected: connectedScreen(ui); break;
    case Screen::NoWifi: noWifiScreen(); break;
    case Screen::Problem: problemScreen(); break;
    case Screen::UpdateCode: if (ui.update) updateCodeScreen(*ui.update); break;
    case Screen::UpdateProgress: if (ui.update) updateProgressScreen(*ui.update); break;
  }
}
