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
// Token: visible ASCII without space, at most 64 characters, so it can never inject a header line.
static bool validToken(const char *s) {
  if (strlen(s) > 64) return false;
  for (; *s; s++)
    if ((uint8_t)*s < 0x21 || (uint8_t)*s > 0x7E) return false;
  return true;
}

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
  String tok = prefs.getString("tok", "");
  if (validToken(tok.c_str())) strlcpy(cur.serviceToken, tok.c_str(), sizeof(cur.serviceToken));
  String tzn = prefs.getString("tzn", ""), tzp = prefs.getString("tzp", "");
  if (validTzName(tzn.c_str()) && validTzPosix(tzp.c_str())) {  // both or neither: never a name from one zone and a rule from another
    strlcpy(cur.tzName, tzn.c_str(), sizeof(cur.tzName));
    strlcpy(cur.tzPosix, tzp.c_str(), sizeof(cur.tzPosix));
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
  prefs.putString("tok", s.serviceToken);
  prefs.putString("tzn", s.tzName);
  prefs.putString("tzp", s.tzPosix);
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

  if (!doc["brightness"].isNull()) {
    int b = doc["brightness"] | -1;
    if (b < 10 || b > 100) { error = "Brightness must be between 10 and 100 percent."; return false; }
    n.brightness = b;
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
    // A saved token belongs to the host it was entered for: a different host never inherits it (a token typed in the same save still applies below).
    if (strcmp(n.serviceHost, h.c_str()) != 0) n.serviceToken[0] = 0;
    strlcpy(n.serviceHost, h.c_str(), sizeof(n.serviceHost));
  }

  if (!svc.isNull()) {
    // The token is write-only: a non-empty string replaces it, an empty or missing one leaves it, clearToken removes it.
    // Nothing here prints the token or puts it in an error message.
    if (svc["clearToken"] | false) n.serviceToken[0] = 0;
    if (svc["token"].is<const char *>()) {
      String t = trimmed(svc["token"]);  // a pasted token often carries a stray newline or space at the edge
      if (t.length()) {
        if (!validToken(t.c_str())) { error = "The access token can be at most 64 characters: visible characters only, no spaces."; return false; }
        strlcpy(n.serviceToken, t.c_str(), sizeof(n.serviceToken));
      }
    }
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
  doc["service"]["tokenSet"] = s.serviceToken[0] != 0;  // the token itself is never sent back
  doc["timezone"]["name"] = s.tzName;
  doc["timezone"]["posix"] = s.tzPosix;
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace settings
