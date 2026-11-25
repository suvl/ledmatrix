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

// ---- checks shared by the portal input and by what NVS gives back, so a damaged value never reaches a header or setenv ----
// IANA name such as Europe/Lisbon: 1..39 characters from A-Z a-z 0-9 _ / + -
static bool validTzName(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 39) return false;
  for (; *s; s++)
    if (!isalnum((uint8_t)*s) && !strchr("_/+-", *s)) return false;
  return true;
}

// POSIX rule such as WET0WEST,M3.5.0/1,M10.5.0/2 or <+03>-3: 1..47 characters, starts with a letter or <, has a digit.
static bool validTzPosix(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 47 || (!isalpha((uint8_t)s[0]) && s[0] != '<')) return false;
  bool digit = false;
  for (; *s; s++) {
    if (!isalnum((uint8_t)*s) && !strchr("<>+,./:-", *s)) return false;
    if (isdigit((uint8_t)*s)) digit = true;
  }
  return digit;
}

// ---- panel protection (slice 7): ranges shared by the portal input and by what NVS gives back ----
static bool validDimAfter(int v) { return v >= 1 && v <= 120; }    // minutes of stillness before dimming
static bool validDimLevel(int v) { return v >= 5 && v <= 100; }    // percent of full brightness
static bool validClockMin(int v) { return v >= 0 && v <= 1439; }   // minutes after local midnight
static bool validRotateMin(int v) { return v >= 1 && v <= 240; }   // minutes between clock and weather swaps

// "HH:MM", 24 hour clock, exactly two digits each side, 00:00 to 23:59. `out` is untouched when this returns false.
static bool parseClock(const char *s, uint16_t &out) {
  if (strlen(s) != 5 || s[2] != ':') return false;
  for (int i : {0, 1, 3, 4})
    if (s[i] < '0' || s[i] > '9') return false;
  int h = (s[0] - '0') * 10 + (s[1] - '0'), m = (s[3] - '0') * 10 + (s[4] - '0');
  if (h > 23 || m > 59) return false;
  out = h * 60 + m;
  return true;
}

// Zero-padded "HH:MM" into a buffer of at least 6 bytes. A value past 1439 cannot happen (it is checked on every way in) but never overruns.
static void formatClock(uint16_t minutes, char *out) {
  if (minutes > 1439) minutes = 0;
  snprintf(out, 6, "%02u:%02u", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
}

// ---- optional JSON fields: missing or null keeps `out`; the wrong JSON type returns false, so a typo is an error and never silently ignored ----
static bool readOptBool(JsonVariantConst v, bool &out) {
  if (v.isNull()) return true;
  if (!v.is<bool>()) return false;
  out = v.as<bool>();
  return true;
}

// A whole number that fits an int. A string, a fraction or a number too big for an int returns false.
static bool readOptInt(JsonVariantConst v, int &out) {
  if (v.isNull()) return true;
  if (!v.is<int>()) return false;
  out = v.as<int>();
  return true;
}

static bool readOptClock(JsonVariantConst v, uint16_t &out) {
  if (v.isNull()) return true;
  if (!v.is<JsonString>()) return false;
  JsonString s = v.as<JsonString>();
  if (s.size() != 5) return false;  // also rejects a string with an embedded NUL
  return parseClock(s.c_str(), out);
}

// A group such as "protection" or "dim": absent or null is fine, anything but an object is a mistake.
static bool isOptGroup(JsonVariantConst v) { return v.isNull() || v.is<JsonObjectConst>(); }

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
  cur.brightness = prefs.getUChar("br", cur.brightness);
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
  String host = prefs.getString("svc", "");
  strlcpy(cur.serviceHost, host.c_str(), sizeof(cur.serviceHost));
  String tzn = prefs.getString("tzn", ""), tzp = prefs.getString("tzp", "");
  if (validTzName(tzn.c_str()) && validTzPosix(tzp.c_str())) {  // both or neither: never a name from one zone and a rule from another
    strlcpy(cur.tzName, tzn.c_str(), sizeof(cur.tzName));
    strlcpy(cur.tzPosix, tzp.c_str(), sizeof(cur.tzPosix));
  }
  // Panel protection: a stored value outside its range is ignored (the default stays), so a damaged flash never gives a bad range.
  cur.drift = prefs.getBool("drift", cur.drift);
  cur.dimOn = prefs.getBool("dimon", cur.dimOn);
  int v = prefs.getUChar("dimmin", cur.dimAfterMin);
  if (validDimAfter(v)) cur.dimAfterMin = v;
  v = prefs.getUChar("dimlvl", cur.dimLevel);
  if (validDimLevel(v)) cur.dimLevel = v;
  cur.nightOn = prefs.getBool("nighton", cur.nightOn);
  v = prefs.getUShort("nightst", cur.nightStartMin);
  if (validClockMin(v)) cur.nightStartMin = v;
  v = prefs.getUShort("nightend", cur.nightEndMin);
  if (validClockMin(v)) cur.nightEndMin = v;
  cur.rotateOn = prefs.getBool("roton", cur.rotateOn);
  v = prefs.getUChar("rotmin", cur.rotateMin);
  if (validRotateMin(v)) cur.rotateMin = v;
  cur.cyclePlanes = prefs.getBool("cycle", cur.cyclePlanes);
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
  prefs.putUChar("br", s.brightness);
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
  prefs.putString("svc", s.serviceHost);
  prefs.putString("tzn", s.tzName);
  prefs.putString("tzp", s.tzPosix);
  prefs.putBool("drift", s.drift);
  prefs.putBool("dimon", s.dimOn);
  prefs.putUChar("dimmin", s.dimAfterMin);
  prefs.putUChar("dimlvl", s.dimLevel);
  prefs.putBool("nighton", s.nightOn);
  prefs.putUShort("nightst", s.nightStartMin);
  prefs.putUShort("nightend", s.nightEndMin);
  prefs.putBool("roton", s.rotateOn);
  prefs.putUChar("rotmin", s.rotateMin);
  prefs.putBool("cycle", s.cyclePlanes);
  prefs.end();
}

// Ticket 39: the access token is gone, the board signs its requests instead (shared/device-auth.md). An older build saved the token
// under "tok": remove it, once, so it does not stay in the saved settings. (NVS marks the entry erased; the bytes leave the flash
// page when NVS next cleans that page up.)
static void dropOldToken() {
  if (!prefs.begin(NS, true)) return;  // no namespace yet: nothing was ever saved
  bool had = prefs.isKey("tok");
  prefs.end();
  if (!had) return;
  if (prefs.begin(NS, false)) {
    prefs.remove("tok");
    prefs.end();
    Serial.println("SETTINGS removed the access token an older version saved");
  }
}

void begin() {
  if (mu) return;
  mu = xSemaphoreCreateMutex();
  lock();
  loadAll();
  dropOldToken();
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

  if (!doc["brightness"].isNull()) {
    int b = doc["brightness"] | -1;
    if (b < 10 || b > 100) { error = "Brightness must be between 10 and 100 percent."; return false; }
    n.brightness = b;
  }

  if (!readOptBool(doc["cyclePlanes"], n.cyclePlanes)) { error = "Cycling through the planes must be on or off."; return false; }

  // Panel protection. Every group and every field is optional; a missing one keeps its value, a wrong type or range refuses the whole save.
  JsonVariantConst prot = doc["protection"];
  if (!isOptGroup(prot)) { error = "The panel protection settings must be a group of fields."; return false; }
  if (!prot.isNull()) {
    if (!readOptBool(prot["drift"], n.drift)) { error = "Pixel drift must be on or off."; return false; }

    JsonVariantConst dim = prot["dim"];
    if (!isOptGroup(dim)) { error = "The dim settings must be a group of fields."; return false; }
    int after = n.dimAfterMin, level = n.dimLevel;
    if (!readOptBool(dim["on"], n.dimOn)) { error = "Dimming must be on or off."; return false; }
    if (!readOptInt(dim["afterMin"], after) || !validDimAfter(after)) { error = "Dim after must be between 1 and 120 minutes."; return false; }
    if (!readOptInt(dim["level"], level) || !validDimLevel(level)) { error = "The dim level must be between 5 and 100 percent."; return false; }
    n.dimAfterMin = after;
    n.dimLevel = level;

    JsonVariantConst night = prot["night"];
    if (!isOptGroup(night)) { error = "The night settings must be a group of fields."; return false; }
    if (!readOptBool(night["on"], n.nightOn)) { error = "Night blanking must be on or off."; return false; }
    if (!readOptClock(night["start"], n.nightStartMin) || !readOptClock(night["end"], n.nightEndMin)) {
      error = "Night times must look like 23:30 (00:00 to 23:59).";
      return false;
    }

    JsonVariantConst rot = prot["rotate"];
    if (!isOptGroup(rot)) { error = "The rotation settings must be a group of fields."; return false; }
    int every = n.rotateMin;
    if (!readOptBool(rot["on"], n.rotateOn)) { error = "Rotation must be on or off."; return false; }
    if (!readOptInt(rot["everyMin"], every) || !validRotateMin(every)) { error = "Rotation must be every 1 to 240 minutes."; return false; }
    n.rotateMin = every;
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

  JsonObject svc = doc["service"];
  if (!svc.isNull() && svc["host"].is<const char *>()) {
    // accept a pasted address: drop the scheme and anything after the host name
    String h = trimmed(svc["host"]);
    h.toLowerCase();
    if (h.startsWith("https://")) h.remove(0, 8);
    else if (h.startsWith("http://")) h.remove(0, 7);
    int slash = h.indexOf('/');
    if (slash >= 0) h.remove(slash);
    bool ok = h.length() <= 63;
    for (size_t i = 0; i < h.length() && ok; i++) {
      char c = h[i];
      ok = isalnum((unsigned char)c) || c == '.' || c == '-';
    }
    if (!ok) { error = "The service address must be a host name such as apis.example.com."; return false; }
    strlcpy(n.serviceHost, h.c_str(), sizeof(n.serviceHost));
  }

  JsonObject tz = doc["timezone"];
  if (!tz.isNull()) {
    if (!tz["name"].is<const char *>() || !tz["posix"].is<const char *>()) { error = "The time zone needs both a name and a rule."; return false; }
    const char *name = tz["name"], *posix = tz["posix"];
    if (!validTzName(name)) { error = "The time zone name must be 1 to 39 characters: letters, digits and _ / + -"; return false; }
    if (!validTzPosix(posix)) { error = "The time zone rule must be a POSIX rule of 1 to 47 characters, such as WET0WEST,M3.5.0/1,M10.5.0/2"; return false; }
    strlcpy(n.tzName, name, sizeof(n.tzName));
    strlcpy(n.tzPosix, posix, sizeof(n.tzPosix));
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
  doc["brightness"] = s.brightness;
  doc["clock24h"] = s.clock24h;
  doc["cyclePlanes"] = s.cyclePlanes;
  JsonObject prot = doc["protection"].to<JsonObject>();
  prot["drift"] = s.drift;
  JsonObject dim = prot["dim"].to<JsonObject>();
  dim["on"] = s.dimOn;
  dim["afterMin"] = s.dimAfterMin;
  dim["level"] = s.dimLevel;
  char hhmm[6];
  JsonObject night = prot["night"].to<JsonObject>();
  night["on"] = s.nightOn;
  formatClock(s.nightStartMin, hhmm);
  night["start"] = hhmm;  // a char array is copied into the document, so the buffer can be reused
  formatClock(s.nightEndMin, hhmm);
  night["end"] = hhmm;
  JsonObject rot = prot["rotate"].to<JsonObject>();
  rot["on"] = s.rotateOn;
  rot["everyMin"] = s.rotateMin;
  JsonObject loc = doc["location"].to<JsonObject>();
  loc["set"] = s.hasLocation;
  loc["lat"] = s.lat;
  loc["lon"] = s.lon;
  loc["radiusKm"] = s.radiusKm;
  loc["altMinFt"] = s.altMinFt;
  loc["altMaxFt"] = s.altMaxFt;
  JsonArray qs = doc["queries"].to<JsonArray>();
  for (int i = 0; i < 5; i++) qs.add(s.queries[i]);
  doc["service"]["host"] = s.serviceHost;
  doc["timezone"]["name"] = s.tzName;
  doc["timezone"]["posix"] = s.tzPosix;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace settings
