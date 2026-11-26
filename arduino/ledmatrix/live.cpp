#include "live.h"

#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <lwip/netdb.h>

#include "cycle.h"
#include "deviceauth.h"
#include "editions.h"
#include "identity.h"
#include "ota.h"
#include "otalogic.h"
#include "problems.h"
#include "protect.h"

namespace {

const uint32_t PLANE_COOLDOWN_MS = 30UL * 60 * 1000;   // a plane is shown once, then not again for this long
const uint32_t FLIGHTS_EVERY_MS = 30UL * 1000;
const uint32_t WEATHER_EVERY_MS = 15UL * 60 * 1000;
const uint32_t NEWS_EVERY_MS = 30UL * 60 * 1000;
const uint32_t RETRY_WEATHER_MS = 60UL * 1000;
const uint32_t RETRY_NEWS_MS = 5UL * 60 * 1000;
const int QUEUE_MAX = 3;
const uint32_t CYCLE_LIST_MAX_AGE_MS = 2UL * 60 * 1000;  // cycle mode: with no successful fetch for this long the list is dropped (a fetch is due every 30 s)
const uint32_t HEADER_DEADLINE_MS = 15UL * 1000;  // the status line and all the headers together: a server that drips one line every few seconds is not waited for
const uint32_t CLOCK_GRACE_MS = 60UL * 1000;  // Wi-Fi up this long with no time from the servers: that is a problem
const uint32_t CLOCK_NOTE_EVERY_MS = 60UL * 1000;
const uint32_t IDENTITY_NOTE_EVERY_MS = 30UL * 60 * 1000;  // a standing notice (identity::notice) is put in the problem list again now and then, so it is not pushed out
const int PLANE_LOG_MAX = 60;  // newest planes kept in RAM for the portal (about 4 KB); lost on restart

SemaphoreHandle_t mu = nullptr;
volatile bool serviceAnswered = false;  // the service gave one complete HTTP answer, whatever the status, since boot (the probation checklist)
volatile bool netBusy = false;  // one HTTPS request at a time, see NetTurn (a flag, not a FreeRTOS mutex: a mutex held across a request reset the board)

// ---- shared state (guarded by mu) --------------------------------------------------------------------
LiveWeather wx;
struct Status {
  bool timeSynced = false;
  uint32_t flightsOkAt = 0, weatherOkAt = 0, newsOkAt = 0;  // millis of the last success, 0 = never
  int planesInRange = 0;
  char family[4] = "";
  char lastError[72] = "";
  int logoFailures = 0;      // logo downloads that failed (not counting 404)
  char lastLogoError[56] = "";
} st;

ProblemLog plog;     // the last 10 problems for the portal, newest first (problems.h)
ProblemWatch pwatch;  // how long each kind of data has been failing in a row: decides the panel message

LivePlane planeQ[QUEUE_MAX];  // one-shot mode: planes waiting for their turn
int planeQn = 0;
PlaneCycle<LivePlane> pc;     // the plane on the panel (both modes) and the cycle list from the last fetch, see cycle.h

LiveNews newsQ[QUEUE_MAX];
int newsQn = 0;
bool newsActive = false;
LiveNews newsCur;
uint32_t newsStart = 0, newsUntil = 0;

struct LogEntry {
  uint32_t t = 0;  // unix time, 0 = clock not yet synchronised
  char cs[10], air[26], ai[4], route[8], type[20];
  int alt;
};
LogEntry planeLog[PLANE_LOG_MAX];
int planeLogN = 0, planeLogHead = 0;  // head = next slot to write

void logPlane(const LivePlane &q) {  // call with the lock held
  time_t t = time(nullptr);
  LogEntry &e = planeLog[planeLogHead];
  e.t = t > 1700000000 ? (uint32_t)t : 0;
  strlcpy(e.cs, q.cs, sizeof(e.cs)); strlcpy(e.air, q.air, sizeof(e.air)); strlcpy(e.ai, q.ai, sizeof(e.ai));
  strlcpy(e.route, q.route, sizeof(e.route)); strlcpy(e.type, q.type, sizeof(e.type));
  e.alt = q.alt;
  planeLogHead = (planeLogHead + 1) % PLANE_LOG_MAX;
  if (planeLogN < PLANE_LOG_MAX) planeLogN++;
}

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

// A problem happened: the portal's list, the one-hour rule behind the panel message, and one Serial line. Takes the lock
// itself, so call it with the lock free. The words are worked out before the lock and the printing after it.
void noteProblem(ProblemKind kind, const char *raw) {
  char why[sizeof(ProblemEntry::why)], detail[sizeof(ProblemEntry::detail)];
  problemWords(raw, why, sizeof(why), detail, sizeof(detail));
  uint32_t now = millis();
  time_t t = time(nullptr);
  lock();
  plog.record(kind, why, detail, t > 1700000000 ? (uint32_t)t : 0, now / 1000);
  pwatch.failed(kind, now);
  unlock();
  Serial.printf("PROBLEM %s: %s\n", problemKindName(kind), why);
}

// This kind of data works again (the service answered): its hour of failure starts over. Lock free, like noteProblem.
void noteWorking(ProblemKind kind) {
  lock();
  pwatch.ok(kind);
  unlock();
}

void setError(ProblemKind kind, const String &why) {
  lock();
  snprintf(st.lastError, sizeof(st.lastError), "%s: %s", problemKindName(kind), why.c_str());
  unlock();
  noteProblem(kind, why.c_str());
}

// The source works again: drop its old problem message so the portal stops showing a fixed problem. Lock held.
void dropError(const char *what) {
  size_t n = strlen(what);
  if (strncmp(st.lastError, what, n) == 0 && st.lastError[n] == ':') st.lastError[0] = 0;
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
    err = PROBLEM_RAW_NO_ADDRESS;
    return false;
  }
  for (int i = 0; i < n; i++) {
    c.stop();
    c.useBuiltinCACertBundle();  // the standard CA store that ships with the core
    c.setConnectionTimeout(5000);
    c.setHandshakeTimeout(10);  // SECONDS (the core multiplies by 1000; 8000 here meant over two hours of waiting in a handshake)
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
  devauth::Refusal refusal;  // what the service said in a 401 (httpsGet fills it)
  bool authProblem = false;  // the request was refused for authentication, or the board could not sign it (serviceGet sets it)
};

// One HTTPS request at a time. The data task refreshes everything right after a settings save, and the portal's
// connection test may run at that moment: two TLS sessions together left under 25 KB of free heap and made name
// lookups time out. Taking turns costs nothing the display can see (all of this is off the render task).
struct NetTurn {
  bool ok = false;
  NetTurn() {
    for (uint32_t t0 = millis(); millis() - t0 < 60000; vTaskDelay(pdMS_TO_TICKS(50)))
      if (__sync_bool_compare_and_swap(&netBusy, false, true)) { ok = true; return; }
  }
  ~NetTurn() { if (ok) netBusy = false; }
};

// GET https://host/path. The body is written to body[] (NUL-terminated); returns its length, or -1.
// sign adds the three device-signature headers (shared/device-auth.md); only the owner's service may be given them. This is
// raw HTTPS on a socket that follows no redirects (a 3xx is just a failed status), so the signature can never travel to another
// host. Keep it that way: do not add redirect handling without dropping the headers on a host change. A 401 body is read into
// r.refusal.
int httpsGet(const char *host, const String &path, char *body, size_t cap, Reply &r, bool sign = false) {
  NetTurn turn;
  if (!turn.ok) { r.error = PROBLEM_RAW_BUSY; return -1; }
  uint32_t t0 = millis();
  // The owner's service gets its host name in lower case, the same text that is signed. Other hosts are written as given.
  char hostLc[devauth::HOST_MAX + 1] = "";
  if (sign && !devauth::lowerHost(host, hostLc, sizeof(hostLc))) { r.error = PROBLEM_RAW_NO_ADDRESS; return -1; }  // not a host name (a damaged saved value)
  NetworkClientSecure c;
  c.setTimeout(5000);
  if (!connectDual(c, host, r.family, r.error)) { r.ms = millis() - t0; return -1; }
  // The headers are signed now, after the connection is up, so the timestamp is as fresh as it can be.
  char authHeaders[devauth::HEADERS_CAP] = "";
  if (sign && !identity::signGet(hostLc, path.c_str(), authHeaders, sizeof(authHeaders))) {
    c.stop();
    r.ms = millis() - t0;
    r.error = PROBLEM_RAW_SIGN_FAILED;
    r.authProblem = true;
    return -1;
  }
  // one write, so the request leaves as one TLS record as before; unsigned, the bytes are unchanged
  String req = "GET " + path + " HTTP/1.1\r\nHost: " + (sign ? hostLc : host) + "\r\nUser-Agent: ledmatrix\r\nAccept: application/json\r\nAccept-Encoding: identity\r\n";
  req += authHeaders;
  req += "Connection: close\r\n\r\n";
  c.print(req);
  uint32_t headersFrom = millis();
  String statusLine = c.readStringUntil('\n');
  int sp = statusLine.indexOf(' ');
  r.status = sp > 0 ? statusLine.substring(sp + 1).toInt() : 0;
  if (sign && otalogic::isHttpAnswer(r.status)) serviceAnswered = true;  // the owner's service answered (a 401 too): it was reached
  bool chunked = false;
  for (;;) {
    if (millis() - headersFrom > HEADER_DEADLINE_MS) {  // stalled in the middle of the headers: give up, the data task has other work
      c.stop();
      r.ms = millis() - t0;
      r.error = PROBLEM_RAW_NO_ANSWER;
      return -1;
    }
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
    // status 0: no status line came (the server hung up, or nothing arrived in 5 s); "HTTP 0" would tell the owner nothing
    r.error = r.status ? "HTTP " + String(r.status) : String(PROBLEM_RAW_NO_ANSWER);
    if (r.status == 401) devauth::parseRefusal(body, n, r.refusal);
    return -1;
  }
  return (int)n;
}

// The raw text (problems.h) for a reason the service gave, or nullptr for one this firmware does not know.
const char *refusalRaw(devauth::Reason why) {
  switch (why) {
    case devauth::Reason::Missing: return PROBLEM_RAW_AUTH_MISSING;
    case devauth::Reason::Malformed: return PROBLEM_RAW_AUTH_MALFORMED;
    case devauth::Reason::BadSignature: return PROBLEM_RAW_AUTH_BAD_SIGNATURE;
    case devauth::Reason::Revoked: return PROBLEM_RAW_AUTH_REVOKED;
    case devauth::Reason::ClockSkew: return PROBLEM_RAW_AUTH_CLOCK_SKEW;
    case devauth::Reason::Unknown: break;
  }
  return nullptr;
}

// The only way the fetchers reach the service: signs the request and words a 401 plainly (the message ends up in /api/live and
// the portal's problem list, so it must not be a bare "HTTP 401"). Everything here runs on the data task, never on the render task.
//  - A board that cannot sign (no identity record, a record for another board, ...) sends nothing: the plain reason comes back.
//  - clock_skew: the service's time is stored as an offset and the same request is sent once more. Once, not in a loop.
//  - Any other refusal is just a failed fetch: the caller's normal slow schedule applies, nothing is wiped, the last data stays.
int serviceGet(const Settings &cfg, const String &path, char *body, size_t cap, Reply &r) {
  if (identity::status() != devauth::Identity::Ok) {
    r.error = identity::rawProblem();
    r.authProblem = true;
    return -1;
  }
  int n = httpsGet(cfg.serviceHost, path, body, cap, r, true);
  if (n < 0 && r.status == 401 && r.refusal.reason == devauth::Reason::ClockSkew && r.refusal.hasServerTime) {
    identity::noteServerTime(r.refusal.serverTime);
    Serial.println("LIVE the service's clock differs from the board's: signing with its time, trying once more");
    r = Reply();
    n = httpsGet(cfg.serviceHost, path, body, cap, r, true);
  }
  if (n < 0 && r.status == 401) {
    r.authProblem = true;
    const char *raw = refusalRaw(r.refusal.reason);
    if (raw) r.error = raw;  // an unknown reason stays "HTTP 401"
  }
  return n;
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
// A logo is refreshed every 12 hours; a 404 is remembered for ten minutes (the owner may add logos later); a network failure is not remembered.
struct LogoSlot {
  char code[3] = "";
  uint8_t state = 0;  // 0 empty, 1 have, 2 missing
  uint32_t at = 0;
  uint16_t px[256];
};
const int LOGO_SLOTS = 16;
LogoSlot logos[LOGO_SLOTS];
const uint32_t LOGO_RETRY_MS = 10UL * 60 * 1000;
const uint32_t LOGO_REFRESH_MS = 12UL * 60 * 60 * 1000;  // logos we have are fetched again now and then, so updated art arrives

LogoSlot *logoFind(const char *code) {  // call with the lock held
  for (auto &l : logos)
    if (l.state && strcmp(l.code, code) == 0) return &l;
  return nullptr;
}

// True when the logo question is settled for this airline: we have it, or the service has none, or there is no
// airline. A plane whose logo is still unknown waits for the next round instead of being shown without one.
bool logoResolved(const char *ai) {  // call with the lock held
  if (!ai || strlen(ai) != 2) return true;
  char code[3] = {(char)toupper(ai[0]), (char)toupper(ai[1]), 0};
  return logoFind(code) != nullptr;
}

void ensureLogo(const Settings &cfg, const char *ai, int &budget) {
  if (!ai || strlen(ai) != 2) return;
  char code[3] = {(char)toupper(ai[0]), (char)toupper(ai[1]), 0};
  uint32_t now = millis();
  lock();
  LogoSlot *have = logoFind(code);
  bool skip = have && now - have->at < (have->state == 1 ? LOGO_REFRESH_MS : LOGO_RETRY_MS);
  unlock();
  if (skip || budget <= 0) return;
  budget--;
  char body[600];
  Reply r;
  String path = String("/v1/logo/") + code;
  int n = serviceGet(cfg, path, body, sizeof(body), r);
  uint8_t state;
  if (n == 512) state = 1;
  else if (n < 0 && r.status == 404) state = 2;
  else {  // network trouble or a bad answer: try again next round
    lock();
    st.logoFailures++;
    snprintf(st.lastLogoError, sizeof(st.lastLogoError), "%s: %s", code, r.error.length() ? r.error.c_str() : "bad answer");
    unlock();
    noteProblem(ProblemKind::Logo, r.error.length() ? r.error.c_str() : PROBLEM_RAW_UNEXPECTED);
    return;
  }
  noteWorking(ProblemKind::Logo);  // the service answered; a 404 only means it has no logo for this airline, which is not a problem
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

// The three fetchers return whether the fetch worked. `refused` is set when it failed because the service refused the board's
// signature or the board cannot sign (Reply::authProblem): the caller then waits longer than its normal retry (devauth::retryDelayMs).
bool fetchFlights(const Settings &cfg, bool &refused) {
  // 10 planes fit: a plane is about 200 bytes (260 with every field at its usual longest), so 2 to 3 KB. A longer answer
  // would be cut and fail as "unexpected answer" rather than show half a list.
  char body[4096];
  Reply r;
  String path = "/v1/flights?lat=" + String(cfg.lat, 4) + "&lon=" + String(cfg.lon, 4) + "&radius_km=" + String(cfg.radiusKm) +
                "&alt_min=" + String(cfg.altMinFt) + "&alt_max=" + String(cfg.altMaxFt) + "&n=" + String(cfg.cyclePlanes ? CYCLE_MAX : 5);
  if (serviceGet(cfg, path, body, sizeof(body), r) < 0) { refused = r.authProblem; setError(ProblemKind::Flights, r.error); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) { setError(ProblemKind::Flights, PROBLEM_RAW_UNEXPECTED); return false; }
  uint32_t now = millis();
  JsonArray ps = doc["planes"];
  int logoBudget = 4;  // at most four logo downloads per round
  for (JsonObject p : ps) ensureLogo(cfg, p["ai"] | "", logoBudget);
  now = millis();
  lock();
  st.planesInRange = ps.size();
  cp(st.family, sizeof(st.family), r.family);
  // Every plane in range goes into the list the cycle uses; it is kept in both modes so switching the toggle on needs no
  // wait for the next fetch. What differs is what happens to a plane the first time it is seen (not seen for 30 minutes).
  pc.n = 0;
  for (JsonObject p : ps) {
    const char *id = p["id"] | "";
    if (!id[0]) continue;
    if (!logoResolved(p["ai"] | "")) continue;  // logo not downloaded yet: not listed or marked seen, so the next round tries again
    bool isNew = planeIsNew(id, now);
    LivePlane q;
    cp(q.id, sizeof(q.id), id);
    cp(q.cs, sizeof(q.cs), p["cs"]);
    cp(q.air, sizeof(q.air), p["air"]);
    cp(q.ai, sizeof(q.ai), p["ai"]);
    cp(q.route, sizeof(q.route), p["rt"]);
    cp(q.from, sizeof(q.from), p["fr"]);
    cp(q.to, sizeof(q.to), p["to"]);
    cp(q.type, sizeof(q.type), p["ty"]);
    q.at = now;
    q.alt = p["alt"] | 0; q.spd = p["spd"] | 0; q.trk = p["trk"] | 0; q.vr = p["vr"] | 0; q.dst = p["dst"] | 0.0f;
    if (pc.n < CYCLE_MAX && pc.find(q.id) < 0) pc.list[pc.n++] = q;
    if (cfg.cyclePlanes) {
      if (isNew) logPlane(q);       // logged once, when it first enters
      planeMarkSeen(id, now);       // while it stays in range it stays "seen" (not logged again after 30 minutes); the cooldown runs from when it leaves
    } else if (isNew && planeQn < QUEUE_MAX) {
      planeQ[planeQn++] = q;
      planeMarkSeen(id, now);
      logPlane(q);
    }
  }
  pc.listReplaced(cfg.cyclePlanes);
  st.flightsOkAt = now ? now : 1;
  dropError("flights");
  unlock();
  noteWorking(ProblemKind::Flights);
  return true;
}

bool fetchWeather(const Settings &cfg, bool &refused) {
  char body[1024];
  Reply r;
  String path = "/v1/weather?lat=" + String(cfg.lat, 4) + "&lon=" + String(cfg.lon, 4);
  if (serviceGet(cfg, path, body, sizeof(body), r) < 0) { refused = r.authProblem; setError(ProblemKind::Weather, r.error); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, body)) { setError(ProblemKind::Weather, PROBLEM_RAW_UNEXPECTED); return false; }
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
  dropError("weather");
  unlock();
  noteWorking(ProblemKind::Weather);
  return true;
}

bool fetchNews(const Settings &cfg, bool &refused) {
  bool anyOk = false, anyFail = false;
  static bool firstPass[5] = {true, true, true, true, true};
  // The language and region the owner picked ("&hl=..&gl=..&ceid=.."), the same for every topic. The default edition is what the
  // service used when the board sent nothing, so a board that never saved the setting asks for the same headlines as before.
  char edition[96];
  if (!editions::queryString(cfg.headlineEdition, edition, sizeof(edition))) edition[0] = 0;  // cannot happen: the longest is 55 bytes
  for (int i = 0; i < 5; i++) {
    if (!cfg.queries[i][0]) continue;
    char body[2048];
    Reply r;
    String path = "/v1/headlines?n=3&q=" + urlEncode(cfg.queries[i]) + edition;
    if (serviceGet(cfg, path, body, sizeof(body), r) < 0) { if (r.authProblem) refused = true; setError(ProblemKind::Headlines, r.error); anyFail = true; continue; }
    JsonDocument doc;
    if (deserializeJson(doc, body)) { setError(ProblemKind::Headlines, PROBLEM_RAW_UNEXPECTED); anyFail = true; continue; }
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
  if (anyOk) { lock(); st.newsOkAt = millis() ? millis() : 1; if (!anyFail) dropError("headlines"); unlock(); }
  if (!anyFail) noteWorking(ProblemKind::Headlines);  // working only when no topic failed
  // One broken topic counts as a failure for the schedule too: the next try is in 5 minutes, not 30. Otherwise the failures would
  // be more than 10 minutes apart, every one would start a new run (problems.h), and the panel message could never appear.
  return anyOk && !anyFail;
}

uint32_t settingsKey(const Settings &c) {
  char buf[200];
  // the headline edition is in the key: another language or region refreshes the headlines at once
  snprintf(buf, sizeof(buf), "%s|%d%d%d|%.4f|%.4f|%u|%d|%d|%u", c.serviceHost, c.showFlights, c.showWeather, c.showNews, c.lat, c.lon, c.radiusKm, (int)c.altMinFt, (int)c.altMaxFt, (unsigned)c.headlineEdition);
  uint32_t h = fnv(buf);
  for (int i = 0; i < 5; i++) h ^= fnv(c.queries[i]) + i;
  return h;
}

// Local time in the board's zone as YYYY-MM-DD HH:MM:SS, and its offset from UTC in minutes (worked out from the two
// broken-down times, so it needs no libc extension). The offset is at most a day apart in either direction.
void localTimeText(time_t t, char *out, size_t cap, int &offsetMin) {
  struct tm lt, ut;
  localtime_r(&t, &lt);
  gmtime_r(&t, &ut);
  strftime(out, cap, "%Y-%m-%d %H:%M:%S", &lt);
  int dayDiff = lt.tm_year != ut.tm_year ? (lt.tm_year > ut.tm_year ? 1 : -1) : lt.tm_yday - ut.tm_yday;
  offsetMin = dayDiff * 1440 + (lt.tm_hour - ut.tm_hour) * 60 + (lt.tm_min - ut.tm_min);
}

void dataTask(void *) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));
  Settings first = settings::get();
  configTzTime(first.tzPosix, "pool.ntp.org", "time.cloudflare.com");  // certificates have dates; the clock screen needs the time
  char tzApplied[sizeof(first.tzPosix)];
  strlcpy(tzApplied, first.tzPosix, sizeof(tzApplied));
  uint32_t nextF = 0, nextW = 0, nextN = 0, lastKey = 0;
  bool lastCycle = first.cyclePlanes;
  bool announcedTime = false;
  uint32_t wifiUpSince = 0, nextClockNote = 0;  // when the Wi-Fi was last seen coming up (0 = down), and when the clock may be noted again
  uint32_t nextIdentityNote = 0;                // when the identity notice (if there is one) may be noted again
  for (;;) {
    uint32_t now = millis();
    bool synced = time(nullptr) > 1700000000;
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    if (!wifiUp) wifiUpSince = 0;
    else if (!wifiUpSince) wifiUpSince = now ? now : 1;
    if (synced && !announcedTime) {
      announcedTime = true;
      Serial.println("LIVE clock synchronised");
      identity::clearClockOffset();  // NTP set the clock: a stored difference to the service's time is stale
      noteWorking(ProblemKind::Clock);
    }
    // No time from the servers although the Wi-Fi has been up for a minute: nothing secure can work without it, so say so in
    // the problem list, about once a minute. Nothing is noted while the Wi-Fi is down (the panel shows that itself).
    if (!synced && wifiUp && now - wifiUpSince > CLOCK_GRACE_MS && (int32_t)(now - nextClockNote) >= 0) {
      nextClockNote = now + CLOCK_NOTE_EVERY_MS;
      noteProblem(ProblemKind::Clock, "The clock is not set: the time servers did not answer. Secure connections need the right time.");
    }
    lock(); st.timeSynced = synced; unlock();
    // The board signs, but the chip's key and the saved key disagree (identity.cpp): say so in the problem list. Needs no network.
    if (identity::notice()[0] && (int32_t)(now - nextIdentityNote) >= 0) {
      nextIdentityNote = now + IDENTITY_NOTE_EVERY_MS;
      noteProblem(ProblemKind::Identity, identity::notice());
    }

    Settings cfg = settings::get();
    if (strcmp(cfg.tzPosix, tzApplied) != 0) {
      // The owner picked another zone: switch the rule now, without a restart and without touching SNTP. The render task
      // calls localtime_r on the other core; newlib takes its own time-zone lock in tzset() and localtime_r(), so this is
      // safe, it is a few microseconds, and the live data lock is not held here.
      setenv("TZ", cfg.tzPosix, 1);
      tzset();
      strlcpy(tzApplied, cfg.tzPosix, sizeof(tzApplied));
      Serial.printf("LIVE time zone now %s\n", cfg.tzName);
    }
    uint32_t key = settingsKey(cfg);
    if (key != lastKey) {  // settings changed: refresh straight away, and a fresh try gets a fresh hour before the panel message
      lastKey = key;
      nextF = nextW = nextN = now;
      lock(); pwatch.clear(); unlock();
    }
    // The cycle toggle changes how many planes are asked for (5 or 10): fetch flights again now, but not weather or headlines.
    if (cfg.cyclePlanes != lastCycle) { lastCycle = cfg.cyclePlanes; nextF = now; }

    if (synced && cfg.serviceHost[0] && WiFi.status() == WL_CONNECTED) {
      if (cfg.showFlights && cfg.hasLocation && (int32_t)(now - nextF) >= 0) {
        bool refused = false;
        bool ok = fetchFlights(cfg, refused);
        nextF = now + (ok ? FLIGHTS_EVERY_MS : devauth::retryDelayMs(refused, 15000));
        Serial.printf("LIVE flights %s\n", ok ? "ok" : "failed");
      }
      if (cfg.showWeather && cfg.hasLocation && (int32_t)(now - nextW) >= 0) {
        bool refused = false;
        bool ok = fetchWeather(cfg, refused);
        nextW = now + (ok ? WEATHER_EVERY_MS : devauth::retryDelayMs(refused, RETRY_WEATHER_MS));
        Serial.printf("LIVE weather %s\n", ok ? "ok" : "failed");
      }
      if (cfg.showNews && (int32_t)(now - nextN) >= 0) {
        bool any = false;
        for (int i = 0; i < 5; i++) any |= cfg.queries[i][0] != 0;
        if (any) {
          bool refused = false;
          bool ok = fetchNews(cfg, refused);
          nextN = now + (ok ? NEWS_EVERY_MS : devauth::retryDelayMs(refused, RETRY_NEWS_MS));
          Serial.printf("LIVE headlines %s\n", ok ? "ok" : "failed");
        } else {
          nextN = now + 60000;
        }
      }
    }
    ota::dataTick(now, synced, cfg);  // firmware updates: the daily check, a confirmed download, the probation checklist (ota.cpp)
    static uint32_t lastStackLog = 0;
    if (now - lastStackLog > 60000) { lastStackLog = now; Serial.printf("LIVE data task stack free %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr)); }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

}  // namespace

// ---- for the firmware update module (ota.cpp) ---------------------------------------------------------------------
namespace {
const uint32_t STREAM_TOTAL_MS = 10UL * 60 * 1000;   // no download takes longer than this

struct HeapBuf {  // the piece buffer, freed on every way out
  uint8_t *p;
  explicit HeapBuf(size_t n) : p((uint8_t *)malloc(n)) {}
  ~HeapBuf() { free(p); }
};
}  // namespace

bool liveHttpsStream(const char *host, const char *path, size_t chunk, int64_t expectLength, uint32_t idleMs, LiveBodySink sink, void *ctx, LiveStreamReply &r) {
  NetTurn turn;
  if (!turn.ok) { r.error = PROBLEM_RAW_BUSY; return false; }
  uint32_t t0 = millis();
  HeapBuf buf(chunk);
  if (!buf.p) { r.error = PROBLEM_RAW_BUSY; return false; }  // no memory for a piece: the same plain "try again later"
  NetworkClientSecure c;
  c.setTimeout(5000);
  if (!connectDual(c, host, r.family, r.error)) { r.ms = millis() - t0; return false; }
  // Unsigned, as given: this goes to the owner's service only (the caller passes the saved host), and follows no redirect.
  String req = String("GET ") + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: ledmatrix\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
  c.print(req);
  uint32_t headersFrom = millis();
  String statusLine = c.readStringUntil('\n');
  int sp = statusLine.indexOf(' ');
  r.status = sp > 0 ? statusLine.substring(sp + 1).toInt() : 0;
  if (otalogic::isHttpAnswer(r.status)) serviceAnswered = true;  // only ever called for the saved service host
  for (;;) {
    if (millis() - headersFrom > HEADER_DEADLINE_MS) {  // stalled in the middle of the headers
      c.stop();
      r.ms = millis() - t0;
      r.error = PROBLEM_RAW_NO_ANSWER;
      return false;
    }
    String line = c.readStringUntil('\n');
    if (line == "\r" || (line.length() == 0 && !c.connected())) break;
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0) r.chunked = true;
    if (line.startsWith("content-length:")) r.length = (int64_t)line.substring(15).toInt();
  }
  if (r.status != 200) {
    r.error = r.status ? "HTTP " + String(r.status) : String(PROBLEM_RAW_NO_ANSWER);
    c.stop();
    r.ms = millis() - t0;
    return false;
  }
  if (expectLength >= 0 && r.length >= 0 && r.length != expectLength) {  // the server announces another size than the manifest: do not even start
    r.lengthMismatch = true;
    r.error = PROBLEM_RAW_UNEXPECTED;
    c.stop();
    r.ms = millis() - t0;
    return false;
  }

  // Reads exactly `want` bytes (want < 0: until the server closes) in pieces, handing each to the sink.
  auto pump = [&](int64_t want) -> bool {
    uint32_t last = millis();
    int64_t got = 0;
    while (want < 0 || got < want) {
      int avail = c.available();
      if (avail > 0) {
        size_t n = (size_t)avail < chunk ? (size_t)avail : chunk;
        if (want >= 0 && (int64_t)n > want - got) n = (size_t)(want - got);
        int rd = c.read(buf.p, n);
        if (rd > 0) {
          if (!sink(buf.p, (size_t)rd, ctx)) { r.stopped = true; return false; }
          got += rd;
          r.bytes += (size_t)rd;
          last = millis();
          vTaskDelay(pdMS_TO_TICKS(2));  // a short pause after every piece: the Wi-Fi and the portal tasks share this core
          continue;
        }
      }
      if (!c.connected() && c.available() <= 0) return want < 0;  // closed: whole only when the server's close was the end
      if (millis() - last > idleMs || millis() - t0 > STREAM_TOTAL_MS) return false;
      vTaskDelay(1);
    }
    return true;
  };

  bool whole = false;
  if (r.chunked) {
    for (;;) {
      if (millis() - t0 > STREAM_TOTAL_MS) break;
      String line = c.readStringUntil('\n');
      if (line.length() == 0) break;  // nothing came: cut off
      size_t sz = 0;
      if (!otalogic::parseChunkSize(line.c_str(), line.length(), sz)) {  // not a chunk size: the framing is broken, which is not "the last chunk"
        r.error = PROBLEM_RAW_UNEXPECTED;
        break;
      }
      if (sz == 0) { whole = true; break; }
      if (!pump((int64_t)sz)) break;
      String end = c.readStringUntil('\n');  // the line that ends the chunk's data
      if (!otalogic::isChunkEnd(end.c_str(), end.length())) {
        r.error = PROBLEM_RAW_UNEXPECTED;
        break;
      }
    }
  } else {
    whole = pump(r.length >= 0 ? r.length : -1);
  }
  c.stop();
  r.ms = millis() - t0;
  r.complete = whole && !r.stopped;
  if (!r.complete && r.error.length() == 0 && !r.stopped) r.error = PROBLEM_RAW_NO_ANSWER;  // cut off, or too slow
  return r.complete;
}

void liveNoteProblem(ProblemKind kind, const char *raw) { noteProblem(kind, raw); }
void liveNoteWorking(ProblemKind kind) { noteWorking(kind); }

bool liveServiceAnswered() { return serviceAnswered; }
bool liveStarted() { return mu != nullptr; }

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
  time_t t = time(nullptr);  // before the lock: localtime_r takes newlib's own time-zone lock, never hold both
  v.timeValid = t > 1700000000;
  if (v.timeValid) localtime_r(&t, &v.now);
  lock();

  // Planes (cycle.h): one-shot queue by default, the whole radar in turn when cfg.cyclePlanes. The plane on the panel always
  // finishes its turn; planes keep priority over headlines in both modes.
  CycleFrame cf;
  cf.now = now;
  cf.dwellMs = (uint32_t)cfg.dwellSec * 1000;
  cf.showFlights = cfg.showFlights;
  cf.cycling = cfg.cyclePlanes;
  // signed: a fetch that finished just after this frame's `now` was taken makes the age slightly negative, which is fresh
  cf.listFresh = pc.n == 0 || (int32_t)(now - st.flightsOkAt) <= (int32_t)CYCLE_LIST_MAX_AGE_MS;
  if (pc.frame(cf, planeQ, planeQn)) {
    v.showPlane = true;
    v.plane = pc.cur;
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
  v.problemMessage = pwatch.longFailure(millis());  // read under the lock: a failure noted before we got it is never "in the future"
  unlock();
  return v;
}

String livePlanesJson() {
  JsonDocument doc;
  JsonArray a = doc["planes"].to<JsonArray>();
  doc["max"] = PLANE_LOG_MAX;
  String out;
  lock();
  for (int i = 0; i < planeLogN; i++) {  // newest first
    const LogEntry &e = planeLog[(planeLogHead - 1 - i + 2 * PLANE_LOG_MAX) % PLANE_LOG_MAX];
    JsonObject o = a.add<JsonObject>();
    o["t"] = e.t; o["cs"] = e.cs; o["air"] = e.air; o["ai"] = e.ai; o["rt"] = e.route; o["ty"] = e.type; o["alt"] = e.alt;
  }
  // Serialized before the unlock: ArduinoJson keeps a const char array (what e.cs is here) by pointer, not as a copy, so the
  // text must be written out while the data task cannot change the log. It is a millisecond at most, and the data task
  // never waits on the render task.
  serializeJson(doc, out);
  unlock();
  return out;
}

// /api/problems: the list the portal shows (problems.h), newest first, and which kinds of data are failing right now.
// Serialized under the lock for the reason given in livePlanesJson (the entries' strings are stored by pointer).
String liveProblemsJson() {
  JsonDocument doc;
  JsonArray list = doc["problems"].to<JsonArray>();
  JsonArray failing = doc["failing"].to<JsonArray>();
  doc["max"] = int(ProblemLog::MAX);
  String out;
  lock();
  uint32_t now = millis();  // after the lock: every time noted before it is not later than this
  for (int i = 0; i < plog.n; i++) {
    const ProblemEntry &e = plog.e[i];
    JsonObject o = list.add<JsonObject>();
    o["what"] = problemKindName(e.kind);
    o["why"] = e.why;
    o["detail"] = e.detail;
    o["n"] = e.count;
    o["t"] = e.lastUnix;       // 0 = the clock was not set yet
    o["first"] = e.firstUnix;
    o["up"] = e.lastUpS;       // seconds since the board started: always known, lets the portal date a problem from the uptime
    o["firstUp"] = e.firstUpS;
  }
  for (int i = 0; i < PROBLEM_KINDS; i++) {
    ProblemKind k = (ProblemKind)i;
    // failing now: a run of failures with the latest one recent (a kind that is no longer asked for would otherwise stay listed)
    if (!pwatch.failing[i] || (uint32_t)(now - pwatch.last[i]) > PROBLEM_FRESH_MS) continue;
    JsonObject o = failing.add<JsonObject>();
    o["what"] = problemKindName(k);
    o["forS"] = pwatch.failingForMs(k, now) / 1000;
    o["long"] = pwatch.kindIsLong(k, now);
  }
  doc["message"] = pwatch.longFailure(now);
  serializeJson(doc, out);
  unlock();
  return out;
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
  int cycleListN = pc.n;
  doc["headlinesWaiting"] = newsQn;
  doc["family"] = st.family;
  doc["lastError"] = st.lastError;
  doc["logoFailures"] = st.logoFailures;
  {
    String c;
    for (auto &l : logos)
      if (l.state) { c += l.code; c += l.state == 1 ? "+ " : "- "; }
    doc["logoCache"] = c;  // + = have it, - = the service has none
  }
  if (st.lastLogoError[0]) doc["lastLogoError"] = st.lastLogoError;
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
  // The zone the owner saved and the time it produces, so a wrong zone is obvious in the portal. Outside the live lock.
  Settings cfg = settings::get();
  doc["cycling"] = cfg.cyclePlanes;
  doc["cycleCount"] = cfg.cyclePlanes && cfg.showFlights ? cycleListN : 0;
  {
    // What the panel is doing to protect itself (protect.h); read from the render task's last frame, no live lock needed.
    ProtectStatus ps = protectStatus();
    JsonObject panel = doc["panel"].to<JsonObject>();
    panel["brightness"] = ps.brightness;
    switch (ps.mode) {
      case PanelMode::Normal: panel["mode"] = "normal"; break;
      case PanelMode::Dim: panel["mode"] = "dim"; break;
      case PanelMode::Night: panel["mode"] = "night"; break;
      case PanelMode::NightAwake: panel["mode"] = "night-awake"; break;
    }
    panel["dx"] = ps.dx;
    panel["dy"] = ps.dy;
  }
  doc["timeZone"] = cfg.tzName;
  time_t t = time(nullptr);
  if (t > 1700000000) {
    char lt[24];
    int offMin;
    localTimeText(t, lt, sizeof(lt), offMin);
    doc["localTime"] = lt;
    doc["utcOffsetMin"] = offMin;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// Reaches three kinds of host to prove the connection code needs no firmware change when IPv6 arrives:
// an IPv6-only host, an IPv4-only host (the service), and a dual-stack host.
String liveSelfTestJson(const Settings &cfg) {
  struct Target { const char *label; String host; String path; bool service; };
  // /healthz needs no signature, so it could not show a wrong key. /v1/logo/ZZ checks the signature first and then
  // answers 404 (no logo) without an upstream call, so 200 or 404 means reachable and the board's signature is accepted.
  Target targets[] = {
    {"service (IPv4 only today)", cfg.serviceHost, "/v1/logo/ZZ", true},
    {"IPv6-only host", "api6.ipify.org", "/", false},
    {"dual-stack host", "api64.ipify.org", "/", false},
  };
  JsonDocument doc;
  doc["boardHasGlobalIPv6"] = boardHasGlobalV6();
  JsonArray arr = doc["tests"].to<JsonArray>();
  for (auto &t : targets) {
    JsonObject o = arr.add<JsonObject>();
    o["what"] = t.label;
    if (t.host.length() == 0) { o["result"] = "skipped: no service address saved"; continue; }
    char body[256];  // a refusal is about 100 bytes of JSON
    Reply r;
    // The signature goes to the owner's service only (serviceGet signs, and retries once when the clock is refused);
    // the third-party hosts get a plain request.
    int n = t.service ? serviceGet(cfg, t.path, body, sizeof(body), r) : httpsGet(t.host.c_str(), t.path, body, sizeof(body), r);
    o["host"] = t.host;
    o["family"] = r.family;
    o["ms"] = r.ms;
    if (t.service) {
      bool ok = r.status == 200 || r.status == 404;
      o["ok"] = ok;
      if (r.authProblem) {  // refused, or the board cannot sign: the same plain sentence the problem list shows
        char why[sizeof(ProblemEntry::why)], detail[sizeof(ProblemEntry::detail)];
        problemWords(r.error.c_str(), why, sizeof(why), detail, sizeof(detail));
        o["error"] = why;  // a char array (not a const one) is copied into the document, so the buffer may go out of scope
      } else if (!ok) o["error"] = r.error;
      continue;
    }
    o["ok"] = n >= 0 || r.status == 200;
    if (!(n >= 0)) o["error"] = r.error;
  }
  String out;
  serializeJson(doc, out);
  return out;
}
