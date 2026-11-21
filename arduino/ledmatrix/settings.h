// Persistent settings in NVS. Slice 2 stores the Wi-Fi credentials; layouts, dwell time, toggles and
// the location arrive with the portal in slice 3.
#pragma once
#include <Arduino.h>

struct WifiCreds {
  String ssid;
  String pass;
  bool valid() const { return ssid.length() > 0; }
};

namespace settings {
void begin();
WifiCreds loadWifi();
void saveWifi(const String &ssid, const String &pass);
void clearWifi();  // forgets the Wi-Fi network only; everything else is kept
}  // namespace settings
