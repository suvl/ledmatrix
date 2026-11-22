#include "live.h"

#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <lwip/netdb.h>

namespace {

// Lisbon time with daylight saving. A per-location zone from the service comes later.
const char *const TZ_POSIX = "WET0WEST,M3.5.0/1,M10.5.0/2";
const uint32_t PLANE_COOLDOWN_MS = 30UL * 60 * 1000;   // a plane is shown once, then not again for this long
const uint32_t FLIGHTS_EVERY_MS = 30UL * 1000;
const uint32_t WEATHER_EVERY_MS = 15UL * 60 * 1000;
const uint32_t NEWS_EVERY_MS = 30UL * 60 * 1000;
const uint32_t RETRY_WEATHER_MS = 60UL * 1000;
const uint32_t RETRY_NEWS_MS = 5UL * 60 * 1000;
const int QUEUE_MAX = 3;

SemaphoreHandle_t mu = nullptr;

// ---- shared state (guarded by mu) --------------------------------------------------------------------
LiveWeather wx;
struct Status {
  bool timeSynced = false;
  uint32_t flightsOkAt = 0, weatherOkAt = 0, newsOkAt = 0;  // millis of the last success, 0 = never
  int planesInRange = 0;
  char family[4] = "";
  char lastError[72] = "";
} st;

LivePlane planeQ[QUEUE_MAX];
int planeQn = 0;
bool planeActive = false;
LivePlane planeCur;
uint32_t planeUntil = 0;

LiveNews newsQ[QUEUE_MAX];
int newsQn = 0;
bool newsActive = false;
LiveNews newsCur;
uint32_t newsStart = 0, newsUntil = 0;

struct SeenPlane { char id[8]; uint32_t at; };
SeenPlane seenPlanes[32];
int seenPlaneNext = 0;
uint32_t seenNews[24];
int seenNewsNext = 0;

void lock() { xSemaphoreTake(mu, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mu); }

void cp(char *dst, size_t n, const char *src) { strlcpy(dst, src ? src : "", n); }

uint32_t fnv(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

void setError(const char *what, const String &why) {
  lock();
  snprintf(st.lastError, sizeof(st.lastError), "%s: %s", what, why.c_str());
  unlock();
}

// ---- HTTPS with IPv6/IPv4 fallback -----------------------------------------------------------------------
bool resolveFamily(const char *host, int family, IPAddress &out) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  int err = lwip_getaddrinfo(host, "443", &hints, &res);
  if (err != 0 || !res) {
    Serial.printf("LIVE resolve %s %s failed (error %d)\n", host, family == AF_INET6 ? "AAAA" : "A", err);
    return false;
  }
  if (family == AF_INET6) {
    out = IPAddress(IPv6, ((struct sockaddr_in6 *)res->ai_addr)->sin6_addr.s6_addr);
  } else {
    out = IPAddress(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
  }
  lwip_freeaddrinfo(res);
  return true;
}

bool boardHasGlobalV6() {
  String g = WiFi.globalIPv6().toString();
  return g.length() > 2 && g != "::";
}

// Connects by host name: resolves both families, tries IPv6 first when the board has a global IPv6
// address (otherwise IPv4 first), and falls back to the other family when an attempt fails. Nothing
// here depends on the host having, or not having, an AAAA record.
bool connectDual(NetworkClientSecure &c, const char *host, const char *&family, String &err) {
  IPAddress a6, a4;
  bool h6 = resolveFamily(host, AF_INET6, a6), h4 = resolveFamily(host, AF_INET, a4);
  struct Try { IPAddress ip; const char *fam; } order[2];
  int n = 0;
  if (boardHasGlobalV6()) {
    if (h6) order[n++] = {a6, "v6"};
    if (h4) order[n++] = {a4, "v4"};
  } else {
    if (h4) order[n++] = {a4, "v4"};
    if (h6) order[n++] = {a6, "v6"};
  }
  if (n == 0) {
    IPAddress any;
    int core = WiFi.hostByName(host, any);
    Serial.printf("LIVE core resolver for %s returned %d (%s)\n", host, core, core == 1 ? any.toString().c_str() : "-");
    err = "no address found for the host name";
    return false;
  }
  for (int i = 0; i < n; i++) {
    c.stop();
    c.useBuiltinCACertBundle();  // the standard CA store that ships with the core
    c.setConnectionTimeout(5000);
    c.setHandshakeTimeout(8000);
    if (c.connect(order[i].ip, 443, host, nullptr, nullptr, nullptr)) {
      family = order[i].fam;
      return true;
    }
    char buf[80] = {0};
    c.lastError(buf, sizeof(buf));
    err = String(order[i].fam) + " connect failed: " + buf;
  }
  return false;
}

bool readExact(NetworkClientSecure &c, uint8_t *dst, size_t want) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < want && millis() - last < 5000) {
    int avail = c.available();
    if (avail > 0) {
      int r = c.read(dst + got, min((size_t)avail, want - got));
      if (r > 0) { got += r; last = millis(); }
    } else if (!c.connected()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  return got == want;
}

size_t readBody(NetworkClientSecure &c, char *body, size_t max, bool chunked) {
  size_t n = 0;
  if (!chunked) {
    uint32_t last = millis();
    while ((c.connected() || c.available()) && n < max - 1 && millis() - last < 5000) {
      int avail = c.available();
      if (avail > 0) {
        int r = c.read((uint8_t *)body + n, min((size_t)avail, max - 1 - n));
        if (r > 0) { n += r; last = millis(); }
      } else {
        vTaskDelay(1);
      }
    }
    return n;
  }
  for (;;) {
    String line = c.readStringUntil('\n');
    size_t chunk = strtoul(line.c_str(), nullptr, 16);
    if (chunk == 0 || n + chunk > max - 1) break;
    if (!readExact(c, (uint8_t *)body + n, chunk)) break;
    n += chunk;
    c.readStringUntil('\n');
  }
  return n;
}

struct Reply {
  int status = 0;
  const char *family = "";
  String error;
  uint32_t ms = 0;
};

// GET https://host/path. The body is written to body[] (NUL-terminated); returns its length, or -1.
int httpsGet(const char *host, const String &path, char *body, size_t cap, Reply &r) {
  uint32_t t0 = millis();
  NetworkClientSecure c;
  c.setTimeout(5000);
  if (!connectDual(c, host, r.family, r.error)) { r.ms = millis() - t0; return -1; }
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ledmatrix\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", path.c_str(), host);
  String statusLine = c.readStringUntil('\n');
  int sp = statusLine.indexOf(' ');
  r.status = sp > 0 ? statusLine.substring(sp + 1).toInt() : 0;
  bool chunked = false;
  for (;;) {
    String line = c.readStringUntil('\n');
    if (line == "\r" || (line.length() == 0 && !c.connected())) break;
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0) chunked = true;
  }
  size_t n = readBody(c, body, cap, chunked);
  body[n] = 0;
  c.stop();
  r.ms = millis() - t0;
  if (r.status != 200) {
    r.error = "HTTP " + String(r.status);
    return -1;
  }
  return (int)n;
}

String urlEncode(const char *s) {
  String o;
  static const char hex[] = "0123456789ABCDEF";
  for (; *s; s++) {
    uint8_t c = *s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// ---- what is new ----------------------------------------------------------------------------------------------
bool planeIsNew(const char *id, uint32_t now) {
  for (auto &s : seenPlanes)
    if (strcmp(s.id, id) == 0) return (uint32_t)(now - s.at) >= PLANE_COOLDOWN_MS;
  return true;
}
void planeMarkSeen(const char *id, uint32_t now) {
  for (auto &s : seenPlanes)
    if (strcmp(s.id, id) == 0) { s.at = now; return; }
  cp(seenPlanes[seenPlaneNext].id, sizeof(seenPlanes[0].id), id);
  seenPlanes[seenPlaneNext].at = now;
  seenPlaneNext = (seenPlaneNext + 1) % 32;
}
bool newsIsNew(uint32_t id) {
  for (uint32_t s : seenNews)
    if (s == id) return false;
  return true;
}
void newsMarkSeen(uint32_t id) {
  seenNews[seenNewsNext] = id;
  seenNewsNext = (seenNewsNext + 1) % 24;
}

// ---- fetchers -----------------------------------------------------------------------------------------------------
// ---- airline logos: 16x16 RGB565 from the service, kept in a small cache -----------------------------------
// The data task fetches a logo before the plane that needs it is queued, so the render task never waits.
// A 404 is remembered for an hour (the owner may add logos later); a network failure is not remembered.
struct LogoSlot {
  char code[3] = "";
  uint8_t state = 0;  // 0 empty, 1 have, 2 missing
  uint32_t at = 0;
  uint16_t px[256];
};
const int LOGO_SLOTS = 16;
LogoSlot logos[LOGO_SLOTS];
const uint32_t LOGO_RETRY_MS = 60UL * 60 * 1000;

LogoSlot *logoFind(const char *code) {  // call with the lock held
  for (auto &l : logos)
    if (l.state && strcmp(l.code, code) == 0) return &l;
  return nullptr;
}

void ensureLogo(const Settings &cfg, const char *ai, int &budget) {
  if (!ai || strlen(ai) != 2) return;
  char code[3] = {(char)toupper(ai[0]), (char)toupper(ai[1]), 0};
  uint32_t now = millis();
  lock();
  LogoSlot *have = logoFind(code);
  bool skip = have && (have->state == 1 || now - have->at < LOGO_RETRY_MS);
  unlock();
  if (skip || budget <= 0) return;
  budget--;
  char body[600];
  Reply r;
  String path = String("/v1/logo/") + code;
  int n = httpsGet(cfg.serviceHost, path, body, sizeof(body), r);
  uint8_t state;
  if (n == 512) state = 1;
  else if (n < 0 && r.status == 404) state = 2;
  else return;  // network trouble or a bad answer: try again next time
  lock();
  LogoSlot *slot = logoFind(code);
  if (!slot) {  // take an empty slot, else the oldest
    slot = &logos[0];
    for (auto &l : logos) {
      if (!l.state) { slot = &l; break; }
      if (l.at < slot->at) slot = &l;
    }
  }
  strlcpy(slot->code, code, sizeof(slot->code));
  slot->state = state;
  slot->at = now ? now : 1;
  if (state == 1)
    for (int i = 0; i < 256; i++) slot->px[i] = ((uint16_t)(uint8_t)body[2 * i] << 8) | (uint8_t)body[2 * i + 1];
  unlock();
}

bool fetchFlights(const Settings &cfg) {
  char body[4096];
  Reply r;
  String path = "/v1/flights?lat=" + String(cfg.lat, 4) + "&lon=" + String(cfg.lon, 4) + "&radius_km=" + String(cfg.radiusKm) +
                "&alt_min=" + String(cfg.altMinFt) + "&alt_max=" + String(cfg.altMaxFt) + "&n=5";
  if (httpsGet(cfg.serviceHost, path, body, sizeof(body), r) < 0) { setError("flights", r.error); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) { setError("flights", "unexpected answer"); return false; }
  uint32_t now = millis();
  JsonArray ps = doc["planes"];
  int logoBudget = 3;  // at most three logo downloads per round
  for (JsonObject p : ps) ensureLogo(cfg, p["ai"] | "", logoBudget);
  now = millis();
  lock();
  st.planesInRange = ps.size();
  cp(st.family, sizeof(st.family), r.family);
  for (JsonObject p : ps) {
    const char *id = p["id"] | "";
    if (!id[0] || !planeIsNew(id, now) || planeQn >= QUEUE_MAX) continue;
    LivePlane &q = planeQ[planeQn++];
    q = LivePlane();
    cp(q.id, sizeof(q.id), id);
    cp(q.cs, sizeof(q.cs), p["cs"]);
    cp(q.air, sizeof(q.air), p["air"]);
    cp(q.ai, sizeof(q.ai), p["ai"]);
    cp(q.route, sizeof(q.route), p["rt"]);
    cp(q.from, sizeof(q.from), p["fr"]);
    cp(q.to, sizeof(q.to), p["to"]);
    cp(q.type, sizeof(q.type), p["ty"]);
    q.alt = p["alt"] | 0; q.spd = p["spd"] | 0; q.trk = p["trk"] | 0; q.vr = p["vr"] | 0; q.dst = p["dst"] | 0.0f;
    planeMarkSeen(id, now);
  }
  st.flightsOkAt = now ? now : 1;
  unlock();
  return true;
}

bool fetchWeather(const Settings &cfg) {
  char body[1024];
  Reply r;
  String path = "/v1/weather?lat=" + String(cfg.lat, 4) + "&lon=" + String(cfg.lon, 4);
  if (httpsGet(cfg.serviceHost, path, body, sizeof(body), r) < 0) { setError("weather", r.error); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) { setError("weather", "unexpected answer"); return false; }
  LiveWeather w;
  w.valid = true;
  cp(w.loc, sizeof(w.loc), doc["loc"]);
  cp(w.icon, sizeof(w.icon), doc["i"] | "cloud");
  if (!doc["t"].isNull()) { w.hasT = true; w.t = doc["t"]; }
  if (!doc["hi"].isNull()) { w.hasHi = true; w.hi = doc["hi"]; }
  if (!doc["lo"].isNull()) { w.hasLo = true; w.lo = doc["lo"]; }
  if (!doc["pop"].isNull()) { w.hasPop = true; w.pop = doc["pop"]; }
  if (doc["h3"].is<JsonObject>()) { w.hasH3 = true; w.h3t = doc["h3"]["t"] | 0; cp(w.h3i, sizeof(w.h3i), doc["h3"]["i"] | "cloud"); }
  if (doc["h6"].is<JsonObject>()) { w.hasH6 = true; w.h6t = doc["h6"]["t"] | 0; cp(w.h6i, sizeof(w.h6i), doc["h6"]["i"] | "cloud"); }
  lock();
  wx = w;
  st.weatherOkAt = millis() ? millis() : 1;
  cp(st.family, sizeof(st.family), r.family);
  unlock();
  return true;
}

bool fetchNews(const Settings &cfg) {
  bool anyOk = false;
  static bool firstPass[5] = {true, true, true, true, true};
  for (int i = 0; i < 5; i++) {
    if (!cfg.queries[i][0]) continue;
    char body[2048];
    Reply r;
    String path = "/v1/headlines?n=3&q=" + urlEncode(cfg.queries[i]);
    if (httpsGet(cfg.serviceHost, path, body, sizeof(body), r) < 0) { setError("headlines", r.error); continue; }
    JsonDocument doc;
    if (deserializeJson(doc, body)) { setError("headlines", "unexpected answer"); continue; }
    anyOk = true;
    lock();
    int shown = 0;
    for (JsonObject it : doc["items"].as<JsonArray>()) {
      const char *title = it["t"] | "";
      if (!title[0]) continue;
      uint32_t id = fnv(title);
      if (!newsIsNew(id)) continue;
      newsMarkSeen(id);
      // after a restart only the newest headline per topic is shown, not the whole backlog
      if (firstPass[i] && shown >= 1) continue;
      if (newsQn < QUEUE_MAX) {
        LiveNews &n = newsQ[newsQn++];
        n = LiveNews();
        n.valid = true; n.id = id;
        cp(n.title, sizeof(n.title), title);
        cp(n.source, sizeof(n.source), it["s"]);
        shown++;
      }
    }
    firstPass[i] = false;
    unlock();
    vTaskDelay(pdMS_TO_TICKS(1500));
  }
  if (anyOk) { lock(); st.newsOkAt = millis() ? millis() : 1; unlock(); }
  return anyOk;
}

uint32_t settingsKey(const Settings &c) {
  char buf[200];
  snprintf(buf, sizeof(buf), "%s|%d%d%d|%.4f|%.4f|%u|%d|%d", c.serviceHost, c.showFlights, c.showWeather, c.showNews, c.lat, c.lon, c.radiusKm, (int)c.altMinFt, (int)c.altMaxFt);
  uint32_t h = fnv(buf);
  for (int i = 0; i < 5; i++) h ^= fnv(c.queries[i]) + i;
  return h;
}

void dataTask(void *) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.cloudflare.com");  // certificates have dates; the clock screen needs the time
  uint32_t nextF = 0, nextW = 0, nextN = 0, lastKey = 0;
  bool announcedTime = false;
  for (;;) {
    uint32_t now = millis();
    bool synced = time(nullptr) > 1700000000;
    if (synced && !announcedTime) {
      announcedTime = true;
      Serial.println("LIVE clock synchronised");
    }
    lock(); st.timeSynced = synced; unlock();

    Settings cfg = settings::get();
    uint32_t key = settingsKey(cfg);
    if (key != lastKey) { lastKey = key; nextF = nextW = nextN = now; }  // settings changed: refresh straight away

    if (synced && cfg.serviceHost[0] && WiFi.status() == WL_CONNECTED) {
      if (cfg.showFlights && cfg.hasLocation && (int32_t)(now - nextF) >= 0) {
        bool ok = fetchFlights(cfg);
        nextF = now + (ok ? FLIGHTS_EVERY_MS : 15000);
        Serial.printf("LIVE flights %s\n", ok ? "ok" : "failed");
      }
      if (cfg.showWeather && cfg.hasLocation && (int32_t)(now - nextW) >= 0) {
        bool ok = fetchWeather(cfg);
        nextW = now + (ok ? WEATHER_EVERY_MS : RETRY_WEATHER_MS);
        Serial.printf("LIVE weather %s\n", ok ? "ok" : "failed");
      }
      if (cfg.showNews && (int32_t)(now - nextN) >= 0) {
        bool any = false;
        for (int i = 0; i < 5; i++) any |= cfg.queries[i][0] != 0;
        if (any) {
          bool ok = fetchNews(cfg);
          nextN = now + (ok ? NEWS_EVERY_MS : RETRY_NEWS_MS);
          Serial.printf("LIVE headlines %s\n", ok ? "ok" : "failed");
        } else {
          nextN = now + 60000;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

}  // namespace

bool liveLogo(const char *ai, uint16_t *out) {
  if (!ai || strlen(ai) != 2) return false;
  char code[3] = {(char)toupper(ai[0]), (char)toupper(ai[1]), 0};
  lock();
  LogoSlot *l = logoFind(code);
  bool ok = l && l->state == 1;
  if (ok) memcpy(out, l->px, sizeof(l->px));
  unlock();
  return ok;
}

void liveBegin() {
  mu = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(dataTask, "data", 16384, nullptr, 1, nullptr, 0);
}

LiveView liveView(uint32_t now, const Settings &cfg) {
  LiveView v;
  v.configured = cfg.serviceHost[0] != 0;
  lock();
  time_t t = time(nullptr);
  v.timeValid = t > 1700000000;
  if (v.timeValid) localtime_r(&t, &v.now);

  if (planeActive && (int32_t)(now - planeUntil) >= 0) planeActive = false;
  if (!planeActive && planeQn > 0 && cfg.showFlights) {
    planeCur = planeQ[0];
    for (int i = 1; i < planeQn; i++) planeQ[i - 1] = planeQ[i];
    planeQn--;
    planeActive = true;
    planeUntil = now + (uint32_t)cfg.dwellSec * 1000;
  }
  if (planeActive) {
    v.showPlane = true;
    v.plane = planeCur;
  } else {
    if (newsActive && (int32_t)(now - newsUntil) >= 0) newsActive = false;
    if (!newsActive && newsQn > 0 && cfg.showNews) {
      newsCur = newsQ[0];
      for (int i = 1; i < newsQn; i++) newsQ[i - 1] = newsQ[i];
      newsQn--;
      // layout A scrolls the whole title across; layout B shows up to three pages of four lines
      size_t len = strlen(newsCur.title);
      uint32_t ms = cfg.newsLayout == 0 ? (uint32_t)((64 + len * 6) / 24.0f * 1000) + 1000 : min<uint32_t>(3, (len + 59) / 60) * 4000 + 1000;
      newsCur.showMs = min<uint32_t>(ms, 35000);
      newsActive = true;
      newsStart = now;
      newsUntil = now + newsCur.showMs;
    }
    if (newsActive) {
      v.showNews = true;
      v.news = newsCur;
      v.newsAgeMs = now - newsStart;
    }
  }
  v.wx = wx;
  bool flightsStale = cfg.showFlights && cfg.hasLocation && cfg.serviceHost[0] && ((st.flightsOkAt && now - st.flightsOkAt > 5UL * 60 * 1000) || (!st.flightsOkAt && now > 3UL * 60 * 1000));
  bool weatherStale = cfg.showWeather && cfg.hasLocation && cfg.serviceHost[0] && ((st.weatherOkAt && now - st.weatherOkAt > 2UL * 3600 * 1000) || (!st.weatherOkAt && now > 5UL * 60 * 1000));
  v.stale = flightsStale || weatherStale;
  unlock();
  return v;
}

String liveStatusJson() {
  JsonDocument doc;
  uint32_t now = millis();
  lock();
  doc["timeSynced"] = st.timeSynced;
  if (st.flightsOkAt) doc["flightsAgeS"] = (now - st.flightsOkAt) / 1000;
  if (st.weatherOkAt) doc["weatherAgeS"] = (now - st.weatherOkAt) / 1000;
  if (st.newsOkAt) doc["newsAgeS"] = (now - st.newsOkAt) / 1000;
  doc["planesInRange"] = st.planesInRange;
  doc["planesWaiting"] = planeQn;
  doc["headlinesWaiting"] = newsQn;
  doc["family"] = st.family;
  doc["lastError"] = st.lastError;
  doc["ipv6Global"] = boardHasGlobalV6();
  JsonArray dns = doc["dnsServers"].to<JsonArray>();
  for (int i = 0; i < 3; i++) {
    IPAddress d = WiFi.dnsIP(i);
    String ds = d.toString();
    if (ds != "0.0.0.0" && ds != "::") dns.add(ds);
  }
  if (wx.valid) {
    doc["weather"]["loc"] = wx.loc;
    if (wx.hasT) doc["weather"]["t"] = wx.t;
    doc["weather"]["icon"] = wx.icon;
  }
  unlock();
  String out;
  serializeJson(doc, out);
  return out;
}

// Reaches three kinds of host to prove the connection code needs no firmware change when IPv6 arrives:
// an IPv6-only host, an IPv4-only host (the service), and a dual-stack host.
String liveSelfTestJson(const Settings &cfg) {
  struct Target { const char *label; String host; String path; };
  Target targets[] = {
    {"service (IPv4 only today)", cfg.serviceHost, "/healthz"},
    {"IPv6-only host", "api6.ipify.org", "/"},
    {"dual-stack host", "api64.ipify.org", "/"},
  };
  JsonDocument doc;
  doc["boardHasGlobalIPv6"] = boardHasGlobalV6();
  JsonArray arr = doc["tests"].to<JsonArray>();
  for (auto &t : targets) {
    JsonObject o = arr.add<JsonObject>();
    o["what"] = t.label;
    if (t.host.length() == 0) { o["result"] = "skipped: no service address saved"; continue; }
    char body[160];
    Reply r;
    int n = httpsGet(t.host.c_str(), t.path, body, sizeof(body), r);
    o["host"] = t.host;
    o["ok"] = n >= 0 || r.status == 200;
    o["family"] = r.family;
    o["ms"] = r.ms;
    if (!(n >= 0)) o["error"] = r.error;
  }
  String out;
  serializeJson(doc, out);
  return out;
}
