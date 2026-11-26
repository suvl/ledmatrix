// Runs the board's real screens.cpp (and the real Adafruit GFX) on the laptop against a fake panel, with a lot of
// made-up data, and checks that column 63 and row 31 stay black on every screen that pixel drift moves.
// Built and run by tools/check_margins.py. With -d it prints the demo screens as pixels instead (for the
// renderer.js comparison).
#include <Adafruit_Protomatter.h>
#include <Arduino.h>

#include <iostream>
#include <map>
#include <random>
#include <string>

#include "screens.h"

#undef min
#undef max
SerialStandIn Serial;
Adafruit_Protomatter matrix;

struct Stat {
  long n = 0, bad = 0;
  int maxx = -1, maxy = -1;
  std::string example;
};
static std::map<std::string, Stat> stats;

static void run(const std::string &group, const std::string &what, Screen s, int layout, const UiState &ui, int flightIdx = 0, uint32_t ms = 0) {
  matrix.clear();
  drawScreen(s, (uint8_t)layout, ms, flightIdx, ui);
  Stat &st = stats[group];
  st.n++;
  bool bad = false;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 64; x++)
      if (matrix.fb[y][x]) {
        st.maxx = std::max(st.maxx, x);
        st.maxy = std::max(st.maxy, y);
        if (x == 63 || y == 31) bad = true;
      }
  if (bad && st.bad++ == 0) st.example = what;
}

static std::mt19937 rng(12345);
static int rnd(int a, int b) { return a + (int)(rng() % (unsigned)(b - a + 1)); }
static std::string rstr(int minLen, int maxLen, const char *charset) {
  int n = rnd(minLen, maxLen), k = (int)strlen(charset);
  std::string r;
  for (int i = 0; i < n; i++) r += charset[rnd(0, k - 1)];
  return r;
}
static const char *ANY = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -.'/&,()";
static const char *WIDE = "MWHNQ1gjpqy ";  // wide letters, a narrow digit (odd widths) and every descender

// Strings right at the width limits: a run of wide letters of every length, with a narrow character at either end.
static std::vector<std::string> edgeStrings() {
  std::vector<std::string> v;
  for (int n = 1; n <= 25; n++) {
    std::string w(n, 'W');
    v.push_back(w);
    v.push_back(w.substr(1) + "1");
    v.push_back("1" + w.substr(1));
    v.push_back(w.substr(1) + "y");
    v.push_back(w.substr(1) + ".");
  }
  return v;
}

static const char *LAYOUT[] = {"A", "B", "C"};

static void flights(const UiState &ui) {
  static uint16_t logoPx[256];
  for (auto &p : logoPx) p = 0xFFFF;  // a real logo can light all 16x16 pixels
  static const char *units[] = {"kt", "mph", "kts"};
  std::vector<std::string> edge = edgeStrings();
  for (int it = 0; it < 30000 + (int)edge.size(); it++) {
    std::string air, route, type, dest, alt, spd, vr;
    if (it < (int)edge.size()) {
      air = route = type = dest = edge[it];
    } else {
      const char *cs = it % 3 == 0 ? WIDE : ANY;
      air = rstr(1, 25, cs); route = rstr(0, 8, it % 3 == 0 ? WIDE : "ABCDEFGHIJKLMNOPQRSTUVWXYZ->"); type = rstr(0, 19, cs); dest = rstr(1, 21, cs);
    }
    char b[32];
    snprintf(b, sizeof b, "%dft", it % 5 == 0 ? rnd(0, 999999) : rnd(0, 45000)); alt = b;
    snprintf(b, sizeof b, "%d", rnd(0, it % 7 == 0 ? 9999 : 700)); spd = b;
    if (it < (int)edge.size()) { alt = edge[it]; spd = ""; }  // the speed line right at its width limit too
    snprintf(b, sizeof b, "%+dft/s", it % 11 == 0 ? rnd(-9999, 9999) : rnd(-150, 150)); vr = b;
    SampleFlight f{air.c_str(), route.c_str(), type.c_str(), "", dest.c_str(), alt.c_str(), spd.c_str(), units[it % 3], rnd(0, 359), vr.c_str(), LogoKind::Globe};
    f.code = it % 4 == 0 ? nullptr : "XX";                   // a live plane, or an example plane
    f.logoPx = (it % 2 == 0 && f.code) ? logoPx : nullptr;  // with or without a real logo
    UiState u = ui;
    u.flight = &f;
    std::string what = "air=" + air + " route=" + route + " type=" + type + " dest=" + dest + " alt=" + alt + " spd=" + spd + f.spdUnit + " trk=" + std::to_string(f.trackDeg) + " vr=" + vr + (f.code ? (f.logoPx ? " logo" : " no logo") : " example");
    for (int l = 0; l < 3; l++) run(std::string("Flight ") + LAYOUT[l], what, Screen::Flight, l, u);
  }
}

static void weather(const UiState &ui) {
  static const char *icons[] = {"sun", "part", "rain", "cloud"};
  std::vector<std::string> edge = edgeStrings();
  for (int it = 0; it < 20000 + (int)edge.size(); it++) {
    LiveWeather w;
    w.valid = true;
    std::string loc = it < (int)edge.size() ? edge[it] : rstr(0, 23, it % 3 == 0 ? WIDE : ANY);
    strlcpy(w.loc, loc.c_str(), sizeof w.loc);
    w.hasT = rnd(0, 9) > 0; w.hasHi = rnd(0, 9) > 0; w.hasLo = rnd(0, 9) > 0; w.hasPop = rnd(0, 9) > 0; w.hasH3 = rnd(0, 9) > 0; w.hasH6 = rnd(0, 9) > 0;
    int lim = it % 5 == 0 ? 120 : 55;  // 3-digit values are not realistic but must not break the margin either
    w.t = rnd(-lim, lim); w.hi = rnd(-lim, lim); w.lo = rnd(-lim, lim); w.pop = rnd(0, 100); w.h3t = rnd(-lim, lim); w.h6t = rnd(-lim, lim);
    strlcpy(w.icon, icons[rnd(0, 3)], sizeof w.icon); strlcpy(w.h3i, icons[rnd(0, 3)], sizeof w.h3i); strlcpy(w.h6i, icons[rnd(0, 3)], sizeof w.h6i);
    struct tm tmv = {};
    tmv.tm_hour = rnd(0, 23);
    UiState u = ui;
    u.wx = &w;
    u.clock = it % 8 == 0 ? nullptr : &tmv;
    std::string what = "loc=" + loc + " t=" + (w.hasT ? std::to_string(w.t) : "--") + " hi=" + std::to_string(w.hi) + " lo=" + std::to_string(w.lo) + " pop=" + std::to_string(w.pop) + " h3=" + std::to_string(w.h3t) + " h6=" + std::to_string(w.h6t) + " icon=" + w.icon + " hour=" + (u.clock ? std::to_string(tmv.tm_hour) : "none");
    for (int l = 0; l < 3; l++) run(std::string("Weather ") + LAYOUT[l], what, Screen::Weather, l, u);
  }
}

// Every day of a leap year (all weekday and month names), every hour, both clock modes, both colon phases.
static void idle(const UiState &ui) {
  for (int day = 0; day < 366; day++)
    for (int h24 = 0; h24 < 2; h24++)
      for (int hh = 0; hh < 24; hh++)
        for (int mm = 0; mm < 60; mm += 29) {
          struct tm tmv = {};
          tmv.tm_year = 124; tmv.tm_mday = 1 + day; tmv.tm_hour = hh; tmv.tm_min = mm;
          mktime(&tmv);
          UiState u = ui;
          u.clock = &tmv;
          u.clock24h = h24;
          for (int phase = 0; phase < 2; phase++)
            run("Idle clock", "day " + std::to_string(day) + (h24 ? " 24h " : " 12h ") + std::to_string(hh) + ":" + std::to_string(mm) + (phase ? " colon off" : " colon on"), Screen::Idle, 0, u, 0, phase ? 500 : 0);
        }
}

// Display names (devicename.h: a-z, 0-9 and hyphens, 1 to 24 characters, no hyphen at either end): every length of the widest letters,
// with a narrow digit or a descender at either end and hyphens inside, then random ones.
static std::vector<std::string> deviceNames() {
  std::vector<std::string> v = {"ledmatrix"};
  for (int n = 1; n <= 24; n++)
    for (char c : {'w', 'm', '0', 'q'}) {
      std::string w(n, c);
      v.push_back(w);
      v.push_back(w.substr(1) + "1");
      v.push_back("1" + w.substr(1));
      v.push_back(w.substr(1) + "y");
      if (n >= 3) v.push_back(std::string(1, c) + std::string(n - 2, '-') + std::string(1, c));
    }
  for (int i = 0; i < 3000; i++) {
    std::string s = rstr(1, 24, "abcdefghijklmnopqrstuvwxyz0123456789-");
    if (s.front() == '-') s.front() = 'a';
    if (s.back() == '-') s.back() = 'z';
    v.push_back(s);
  }
  return v;
}

// The problem message is fixed text that replaces weather or the clock, so it drifts like them. Twice, for two animation times.
// It shows the display name as "<name>.local", so every kind of name is tried: a long one is cut and must not reach column 63.
static void problem(const UiState &ui) {
  for (int phase = 0; phase < 2; phase++) run("Problem msg", "problem message", Screen::Problem, 0, ui, 0, phase ? 500 : 0);
  for (const std::string &name : deviceNames()) {
    UiState u = ui;
    u.deviceName = name.c_str();
    for (int phase = 0; phase < 2; phase++) run("Problem msg", "problem message, display name " + name, Screen::Problem, 0, u, 0, phase ? 500 : 0);
  }
}

int main(int argc, char **argv) {
  matrix.cp437(true);  // like ledmatrix.ino: raw CP437 codes
  UiState ui;
  ui.apSsid = ui.apPass = ui.apIp = ui.staIp = "";
  ui.clock24h = true;

  if (argc > 1 && std::string(argv[1]) == "-u") {  // the firmware-update screens for the states on stdin, as pixels
    // One state per line, tab separated: name, code, seconds left, version, percent, status word. Dumps name.code and name.prog.
    std::string line;
    while (std::getline(std::cin, line)) {
      std::vector<std::string> f;
      size_t from = 0;
      for (;;) {
        size_t tab = line.find('\t', from);
        f.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
        if (tab == std::string::npos) break;
        from = tab + 1;
      }
      if (f.size() != 6) continue;
      UpdateView v;
      strlcpy(v.code, f[1].c_str(), sizeof v.code);
      v.secondsLeft = atoi(f[2].c_str());
      strlcpy(v.version, f[3].c_str(), sizeof v.version);
      v.percent = atoi(f[4].c_str());
      strlcpy(v.line, f[5].c_str(), sizeof v.line);
      UiState u = ui;
      u.update = &v;
      auto dumpScreen = [&](const std::string &name, Screen s) {
        matrix.clear();
        drawScreen(s, 0, 0, 0, u);
        printf("DUMP %s\n", name.c_str());
        for (int y = 0; y < 32; y++) { for (int x = 0; x < 64; x++) printf("%04x", matrix.fb[y][x]); printf("\n"); }
      };
      dumpScreen(f[0] + ".code", Screen::UpdateCode);
      dumpScreen(f[0] + ".prog", Screen::UpdateProgress);
    }
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "-d") {  // the demo screens as RGB565 hex, one line per picture row
    auto dump = [&](const std::string &name) {
      printf("DUMP %s\n", name.c_str());
      for (int y = 0; y < 32; y++) { for (int x = 0; x < 64; x++) printf("%04x", matrix.fb[y][x]); printf("\n"); }
    };
    for (int fi = 0; fi < SAMPLE_FLIGHT_COUNT; fi++)
      for (int l = 0; l < 3; l++) { matrix.clear(); drawScreen(Screen::Flight, l, 0, fi, ui); dump(std::string("flight") + LAYOUT[l] + std::to_string(fi)); }
    for (int l = 0; l < 3; l++) { matrix.clear(); drawScreen(Screen::Weather, l, 0, 0, ui); dump(std::string("weather") + LAYOUT[l]); }
    matrix.clear(); drawScreen(Screen::Idle, 0, 0, 0, ui); dump("idleA");
    matrix.clear(); drawScreen(Screen::Problem, 0, 0, 0, ui); dump("problem");
    // The screens that show the display name: with the default name and with names that are cut (Connected, problem) or move to two small
    // lines (setup). Dumped with the example values renderer.js uses (the setup password and the two addresses).
    UiState d = ui;
    d.apPass = "7K3PQ9XM"; d.apIp = "192.168.4.1"; d.staIp = "192.168.1.100";
    struct Named { const char *tag, *name; };
    for (Named nm : {Named{"", "ledmatrix"}, Named{"Mid", "kitchen"}, Named{"Fifteen", "kitchen-display"}, Named{"Max", "living-room-display-two"}}) {
      d.deviceName = nm.name; d.apSsid = nm.name;
      matrix.clear(); drawScreen(Screen::Connected, 0, 0, 0, d); dump(std::string("connected") + nm.tag);
      matrix.clear(); drawScreen(Screen::Problem, 0, 0, 0, d); dump(std::string("problemName") + nm.tag);
      matrix.clear(); drawScreen(Screen::Setup, 0, 0, 0, d); dump(std::string("setup") + nm.tag);
    }
    // the names right around the setup screen's limits: 10 characters are drawn big, 11 small, 15 fill one line, 16 need two
    for (int n : {10, 11, 14, 15, 16, 24}) {
      std::string nm(n, 'm');
      d.deviceName = d.apSsid = nm.c_str();
      matrix.clear(); drawScreen(Screen::Setup, 0, 0, 0, d); dump("setupLen" + std::to_string(n));
    }
    return 0;
  }

  flights(ui);
  weather(ui);
  idle(ui);
  problem(ui);
  int failures = 0;
  for (auto &kv : stats) {
    const Stat &s = kv.second;
    printf("  %-12s %7ld pictures, lit up to column %2d and row %2d: %s\n", kv.first.c_str(), s.n, s.maxx, s.maxy, s.bad ? "FAIL" : "ok");
    if (s.bad) { printf("      %ld of them touch column 63 or row 31, first: %s\n", s.bad, s.example.c_str()); failures++; }
  }
  return failures ? 1 : 0;
}
