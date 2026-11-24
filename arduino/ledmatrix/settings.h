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
  char serviceToken[65] = ""; // access token sent as X-Token to serviceHost only; empty = no header. Never returned by toJson().
  char tzName[40] = "Europe/Lisbon";                // IANA name, shown in the portal
  char tzPosix[48] = "WET0WEST,M3.5.0/1,M10.5.0/2";  // the rule the clock actually uses
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
