// Persistent settings in NVS: Wi-Fi credentials and everything the portal edits.
#pragma once
#include <Arduino.h>

struct WifiCreds {
  String ssid;
  String pass;
  bool valid() const { return ssid.length() > 0; }
};

// What the portal edits. Defaults apply until the owner saves something.
struct Settings {
  bool showFlights = true;
  bool showWeather = true;
  bool showNews = true;
  uint8_t flightLayout = 0;   // 0..2 = A..C
  uint8_t weatherLayout = 0;  // 0..2 = A..C
  uint8_t newsLayout = 0;     // 0..1 = A..B
  bool idleClock = true;      // false = clean black screen
  bool clock24h = true;
  uint8_t brightness = 100;   // percent of full panel brightness, 10..100
  uint8_t dwellSec = 10;      // how long a new plane stays on screen
  bool hasLocation = false;
  float lat = 0, lon = 0;
  uint16_t radiusKm = 15;
  int32_t altMinFt = 0;
  int32_t altMaxFt = 45000;
  char queries[5][41] = {};   // headline topics
  char serviceHost[64] = "";  // host name of the data service (ledmatrix-api); empty = demo screens only
  char tzName[40] = "Europe/Lisbon";                // IANA name, shown in the portal
  char tzPosix[48] = "WET0WEST,M3.5.0/1,M10.5.0/2";  // the rule the clock actually uses

  // ---- slice 7: panel protection (tickets 24 and 25) ----
  bool drift = true;              // move static pictures one pixel every few minutes
  bool dimOn = true;              // dim after stillness (no new plane for dimAfterMin)
  uint8_t dimAfterMin = 2;        // 1..120 minutes
  uint8_t dimLevel = 10;          // 5..100 percent of FULL brightness (same scale as `brightness`); never above `brightness`
  bool nightOn = true;            // blank the panel between two local times
  uint16_t nightStartMin = 0;     // minutes after local midnight, 0..1439 (00:00)
  uint16_t nightEndMin = 360;     // 06:00; start > end wraps past midnight; start == end blanks nothing
  bool rotateOn = false;          // swap weather and clock every rotateMin minutes
  uint8_t rotateMin = 15;         // 1..240
  bool cyclePlanes = false;       // cycle every plane inside the radar for the dwell time while it stays in range
};

namespace settings {
void begin();  // idempotent; call once from setup() before anything reads settings

WifiCreds loadWifi();
void saveWifi(const String &ssid, const String &pass);
void clearWifi();  // forgets the Wi-Fi network only; everything else is kept

Settings get();                                                       // thread-safe copy
bool setFromJson(const char *json, size_t len, String &error);       // validate, save, apply
String toJson();
}  // namespace settings
