#include "settings.h"

#include <Preferences.h>

namespace settings {

static Preferences prefs;
static const char *NS = "ledmatrix";

void begin() {
  // Namespace is created on first write. Read/write are opened per call so nothing stays locked.
}

WifiCreds loadWifi() {
  WifiCreds c;
  if (!prefs.begin(NS, true)) return c;  // read-only; fails if the namespace does not exist yet
  c.ssid = prefs.getString("ssid", "");
  c.pass = prefs.getString("pass", "");
  prefs.end();
  return c;
}

void saveWifi(const String &ssid, const String &pass) {
  if (!prefs.begin(NS, false)) return;
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearWifi() {
  if (!prefs.begin(NS, false)) return;
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

}  // namespace settings
