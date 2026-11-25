// Live data: a task on core 0 fetches flights, weather and headlines from the owner's service
// (ledmatrix-api) and decides what is new; the render task asks liveView() what to show.
#pragma once
#include <Arduino.h>
#include <time.h>

#include "settings.h"

struct LivePlane {
  char id[8] = "";
  char cs[10] = "";
  char air[26] = "";
  char ai[4] = "";
  char route[8] = "";
  char from[22] = "";
  char to[22] = "";
  char type[20] = "";
  int alt = 0, spd = 0, trk = 0, vr = 0;
  float dst = 0;
  uint32_t at = 0;  // millis() when the position was received: the altitude is carried forward from here
};

struct LiveWeather {
  bool valid = false;
  char loc[24] = "";
  bool hasT = false, hasHi = false, hasLo = false, hasPop = false, hasH3 = false, hasH6 = false;
  int t = 0, hi = 0, lo = 0, pop = 0, h3t = 0, h6t = 0;
  char icon[6] = "cloud";
  char h3i[6] = "cloud";
  char h6i[6] = "cloud";
};

struct LiveNews {
  bool valid = false;
  char title[104] = "";
  char source[26] = "";
  uint32_t id = 0;
  uint32_t showMs = 15000;  // how long to show it (set when it starts)
};

struct LiveView {
  bool configured = false;  // a service address is saved
  bool showPlane = false;
  LivePlane plane;
  bool showNews = false;
  LiveNews news;
  uint32_t newsAgeMs = 0;
  LiveWeather wx;
  bool stale = false;       // data is older than it should be
  bool problemMessage = false;  // some kind of data has been failing for an hour and still is: the panel may show the message
  bool timeValid = false;
  struct tm now = {};
};

void liveBegin();                                         // starts the data task (core 0)
LiveView liveView(uint32_t nowMs, const Settings &cfg);   // called every frame by the render task
String livePlanesJson();                                  // /api/planes: the last planes seen, newest first
String liveProblemsJson();                                // /api/problems: the last 10 problems with their reasons, and what is failing now
String liveStatusJson();                                  // for /api/live
String liveSelfTestJson(const Settings &cfg);             // /api/selftest: reaches IPv4-only, IPv6-only and dual-stack hosts
bool liveLogo(const char *airlineIata, uint16_t *out256);  // copies the cached 16x16 logo; false = none (use the badge)
