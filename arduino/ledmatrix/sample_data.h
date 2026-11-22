// Example data for the demo screens (same values as the preview page). Replaced by live data in slice 4.
#pragma once
#include "art.h"

struct SampleFlight {
  const char *airline;
  const char *route;
  const char *type;
  const char *originName;
  const char *destName;
  const char *alt;     // e.g. "9.9kft"
  const char *spdVal;  // number only, e.g. "330"
  const char *spdUnit; // "kts" or "mph"
  int trackDeg;
  const char *vr;      // e.g. "+1ft/s"
  LogoKind logo;
  const uint16_t *logoPx = nullptr;  // a real logo, when there is one
  const char *code = nullptr;       // set for live planes only: no logo then means no logo area
};

static const SampleFlight SAMPLE_FLIGHTS[] = {
  {"Korean", "LAX-ICN", "747-8", "Los Angeles Intl", "Seoul Incheon", "34.0kft", "540", "mph", 294, "+0ft/s", LogoKind::Roundel},
  {"American", "DCA-ORD", "E75L", "Washington Reagan", "Chicago O'Hare", "9.9kft", "330", "kts", 305, "+1ft/s", LogoKind::Stripes},
  {"United", "GUA-LAX", "737 MAX 8", "Guatemala Aurora", "Los Angeles Intl", "9.8kft", "367", "mph", 279, "-7ft/s", LogoKind::Orb},
  {"PAN AM", "JFK>SEA", "B787-8", "John F Kennedy Intl", "Seattle Tacoma Intl", "12.2kft", "380", "mph", 301, "+9ft/s", LogoKind::Globe},
};
static const int SAMPLE_FLIGHT_COUNT = sizeof(SAMPLE_FLIGHTS) / sizeof(SAMPLE_FLIGHTS[0]);

static const char *const SAMPLE_HEADLINES[] = {
  "City council approves new tram line for the riverside",
  "Local bakery wins national bread award",
  "Heavy rain expected across the north tonight",
};
static const int SAMPLE_HEADLINE_COUNT = sizeof(SAMPLE_HEADLINES) / sizeof(SAMPLE_HEADLINES[0]);
