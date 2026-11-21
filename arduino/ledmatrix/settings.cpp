#include "settings.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace settings {

static Preferences prefs;
static SemaphoreHandle_t mu = nullptr;  // guards `cur` and every NVS access
static Settings cur;
static const char *NS = "ledmatrix";

static void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(mu); }

static void loadAll() {
  if (!prefs.begin(NS, true)) return;  // namespace does not exist yet: keep the defaults
  cur.showFlights = prefs.getBool("sf", cur.showFlights);
  cur.showWeather = prefs.getBool("sw", cur.showWeather);
  cur.showNews = prefs.getBool("sn", cur.showNews);
  cur.flightLayout = prefs.getUChar("lf", cur.flightLayout);
  cur.weatherLayout = prefs.getUChar("lw", cur.weatherLayout);
  cur.newsLayout = prefs.getUChar("ln", cur.newsLayout);
  cur.idleClock = prefs.getBool("ic", cur.idleClock);
  cur.clock24h = prefs.getBool("c24", cur.clock24h);
  cur.dwellSec = prefs.getUChar("dw", cur.dwellSec);
  cur.hasLocation = prefs.getBool("hl", cur.hasLocation);
  cur.lat = prefs.getFloat("lat", cur.lat);
  cur.lon = prefs.getFloat("lon", cur.lon);
  cur.radiusKm = prefs.getUShort("rad", cur.radiusKm);
  cur.altMinFt = prefs.getInt("amin", cur.altMinFt);
  cur.altMaxFt = prefs.getInt("amax", cur.altMaxFt);
  for (int i = 0; i < 5; i++) {
    String key = "q" + String(i);
    String v = prefs.getString(key.c_str(), "");
    strlcpy(cur.queries[i], v.c_str(), sizeof(cur.queries[i]));
  }
  prefs.end();
}

static void saveAll(const Settings &s) {
  if (!prefs.begin(NS, false)) return;
  prefs.putBool("sf", s.showFlights);
  prefs.putBool("sw", s.showWeather);
  prefs.putBool("sn", s.showNews);
  prefs.putUChar("lf", s.flightLayout);
  prefs.putUChar("lw", s.weatherLayout);
  prefs.putUChar("ln", s.newsLayout);
  prefs.putBool("ic", s.idleClock);
  prefs.putBool("c24", s.clock24h);
  prefs.putUChar("dw", s.dwellSec);
  prefs.putBool("hl", s.hasLocation);
  prefs.putFloat("lat", s.lat);
  prefs.putFloat("lon", s.lon);
  prefs.putUShort("rad", s.radiusKm);
  prefs.putInt("amin", s.altMinFt);
  prefs.putInt("amax", s.altMaxFt);
  for (int i = 0; i < 5; i++) {
    String key = "q" + String(i);
    prefs.putString(key.c_str(), s.queries[i]);
  }
  prefs.end();
}

void begin() {
  if (mu) return;
  mu = xSemaphoreCreateMutex();
  lock();
  loadAll();
  unlock();
}

WifiCreds loadWifi() {
  WifiCreds c;
  lock();
  if (prefs.begin(NS, true)) {
    c.ssid = prefs.getString("ssid", "");
    c.pass = prefs.getString("pass", "");
    prefs.end();
  }
  unlock();
  return c;
}

void saveWifi(const String &ssid, const String &pass) {
  lock();
  if (prefs.begin(NS, false)) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
  }
  unlock();
}

void clearWifi() {
  lock();
  if (prefs.begin(NS, false)) {
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
  }
  unlock();
}

Settings get() {
  lock();
  Settings s = cur;
  unlock();
  return s;
}

static String trimmed(const char *s) {
  String t(s);
  t.trim();
  return t;
}

bool setFromJson(const char *json, size_t len, String &error) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) {
    error = "That was not valid JSON.";
    return false;
  }
  Settings n = get();

  if (doc["showFlights"].is<bool>()) n.showFlights = doc["showFlights"];
  if (doc["showWeather"].is<bool>()) n.showWeather = doc["showWeather"];
  if (doc["showNews"].is<bool>()) n.showNews = doc["showNews"];
  if (doc["clock24h"].is<bool>()) n.clock24h = doc["clock24h"];

  JsonObject lay = doc["layouts"];
  if (!lay.isNull()) {
    int f = lay["flight"] | (int)n.flightLayout, w = lay["weather"] | (int)n.weatherLayout, nw = lay["news"] | (int)n.newsLayout;
    if (f < 0 || f > 2) { error = "The flight layout must be A, B or C."; return false; }
    if (w < 0 || w > 2) { error = "The weather layout must be A, B or C."; return false; }
    if (nw < 0 || nw > 1) { error = "The headlines layout must be A or B."; return false; }
    n.flightLayout = f; n.weatherLayout = w; n.newsLayout = nw;
    if (lay["idle"].is<const char *>()) n.idleClock = strcmp(lay["idle"], "black") != 0;
  }

  if (!doc["dwellSec"].isNull()) {
    int d = doc["dwellSec"] | -1;
    if (d < 3 || d > 120) { error = "A plane must stay on screen between 3 and 120 seconds."; return false; }
    n.dwellSec = d;
  }

  JsonObject loc = doc["location"];
  if (!loc.isNull()) {
    bool set = loc["set"] | false;
    double lat = loc["lat"] | 0.0, lon = loc["lon"] | 0.0;
    int rad = loc["radiusKm"] | (int)n.radiusKm;
    int amin = loc["altMinFt"] | (int)n.altMinFt, amax = loc["altMaxFt"] | (int)n.altMaxFt;
    if (set && (lat < -90 || lat > 90)) { error = "Latitude must be between -90 and 90."; return false; }
    if (set && (lon < -180 || lon > 180)) { error = "Longitude must be between -180 and 180."; return false; }
    if (rad < 1 || rad > 250) { error = "The radius must be between 1 and 250 km."; return false; }
    if (amin < 0 || amax > 60000 || amin > amax) { error = "Altitudes must be 0 to 60000 ft, lowest not above highest."; return false; }
    n.hasLocation = set;
    n.lat = (float)lat;
    n.lon = (float)lon;
    n.radiusKm = rad;
    n.altMinFt = amin;
    n.altMaxFt = amax;
  }

  JsonArray qs = doc["queries"];
  if (!qs.isNull()) {
    for (int i = 0; i < 5; i++) {
      String q = i < (int)qs.size() ? trimmed(qs[i] | "") : String("");
      if (q.length() > 40) { error = "Each headline topic can be at most 40 characters."; return false; }
      strlcpy(n.queries[i], q.c_str(), sizeof(n.queries[i]));
    }
  }

  lock();
  cur = n;
  saveAll(cur);
  unlock();
  return true;
}

String toJson() {
  Settings s = get();
  JsonDocument doc;
  doc["showFlights"] = s.showFlights;
  doc["showWeather"] = s.showWeather;
  doc["showNews"] = s.showNews;
  JsonObject lay = doc["layouts"].to<JsonObject>();
  lay["flight"] = s.flightLayout;
  lay["weather"] = s.weatherLayout;
  lay["news"] = s.newsLayout;
  lay["idle"] = s.idleClock ? "clock" : "black";
  doc["dwellSec"] = s.dwellSec;
  doc["clock24h"] = s.clock24h;
  JsonObject loc = doc["location"].to<JsonObject>();
  loc["set"] = s.hasLocation;
  loc["lat"] = s.lat;
  loc["lon"] = s.lon;
  loc["radiusKm"] = s.radiusKm;
  loc["altMinFt"] = s.altMinFt;
  loc["altMaxFt"] = s.altMaxFt;
  JsonArray qs = doc["queries"].to<JsonArray>();
  for (int i = 0; i < 5; i++) qs.add(s.queries[i]);
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace settings
