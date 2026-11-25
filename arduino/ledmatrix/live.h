// Live data: a task on core 0 fetches flights, weather and headlines from the owner's service
// (ledmatrix-api) and decides what is new; the render task asks liveView() what to show.
#pragma once
#include <Arduino.h>
#include <time.h>

#include "problems.h"
#include "settings.h"

struct LivePlane {
  char id[8] = "";
  char cs[10] = "";
  char air[26] = "";
  char ai[4] = "";
  char route[8] = "";
  char from[22] = "";
  char to[22] = "";
  char type[20] = "";
  int alt = 0, spd = 0, trk = 0, vr = 0;
  float dst = 0;
  uint32_t at = 0;  // millis() when the position was received: the altitude is carried forward from here
};

struct LiveWeather {
  bool valid = false;
  char loc[24] = "";
  bool hasT = false, hasHi = false, hasLo = false, hasPop = false, hasH3 = false, hasH6 = false;
  int t = 0, hi = 0, lo = 0, pop = 0, h3t = 0, h6t = 0;
  char icon[6] = "cloud";
  char h3i[6] = "cloud";
  char h6i[6] = "cloud";
};

struct LiveNews {
  bool valid = false;
  char title[104] = "";
  char source[26] = "";
  uint32_t id = 0;
  uint32_t showMs = 15000;  // how long to show it (set when it starts)
};

struct LiveView {
  bool configured = false;  // a service address is saved
  bool showPlane = false;
  LivePlane plane;
  bool showNews = false;
  LiveNews news;
  uint32_t newsAgeMs = 0;
  LiveWeather wx;
  bool stale = false;       // data is older than it should be
  bool problemMessage = false;  // some kind of data has been failing for an hour and still is: the panel may show the message
  bool timeValid = false;
  struct tm now = {};
};

void liveBegin();                                         // starts the data task (core 0)
LiveView liveView(uint32_t nowMs, const Settings &cfg);   // called every frame by the render task
String livePlanesJson();                                  // /api/planes: the last planes seen, newest first
String liveProblemsJson();                                // /api/problems: the last 10 problems with their reasons, and what is failing now
String liveStatusJson();                                  // for /api/live
String liveSelfTestJson(const Settings &cfg);             // /api/selftest: reaches IPv4-only, IPv6-only and dual-stack hosts
bool liveLogo(const char *airlineIata, uint16_t *out256);  // copies the cached 16x16 logo; false = none (use the badge)

// ---- for the firmware update module (ota.cpp, shared/firmware-update.md) ----------------------------------------------------
// The same connection code as the fetchers (IPv6/IPv4, the built-in certificate store, one request at a time), for a download that
// is too big to hold in RAM. Unsigned: the manifest and the images are public routes, and no signature header ever leaves here.
typedef bool (*LiveBodySink)(const uint8_t *data, size_t len, void *ctx);  // false = stop (the transfer ends, nothing more is read)
struct LiveStreamReply {
  int status = 0;              // the HTTP status; 0 = no status line arrived
  int64_t length = -1;         // Content-Length, -1 when the server sent none
  bool chunked = false;        // the body came as Transfer-Encoding: chunked
  bool lengthMismatch = false; // expectLength was given and Content-Length said another number: no body was read
  bool stopped = false;        // the sink said stop
  bool complete = false;       // a 200 whose whole body arrived
  size_t bytes = 0;            // body bytes handed to the sink
  const char *family = "";
  uint32_t ms = 0;
  String error;                // the raw text (problems.h) when the call returns false, e.g. "HTTP 404" or PROBLEM_RAW_NO_ANSWER
};
// GET https://host/path, no redirects. A 200 body goes to `sink` in pieces of up to `chunk` bytes, with a short pause after each so the
// Wi-Fi, the portal and the render task keep running; the heap buffer for a piece is freed on return. `expectLength` >= 0: a
// Content-Length that says another number ends the call before any body is read (lengthMismatch). `idleMs`: no byte for this long
// ends the transfer. Returns true only when the status was 200, the whole body was delivered and the sink took all of it. The caller's
// sink may block (a flash write); nothing here holds a lock.
bool liveHttpsStream(const char *host, const char *path, size_t chunk, int64_t expectLength, uint32_t idleMs, LiveBodySink sink, void *ctx, LiveStreamReply &r);
void liveNoteProblem(ProblemKind kind, const char *raw);  // the problem list and the one-hour rule (live.cpp's noteProblem), lock free
void liveNoteWorking(ProblemKind kind);                    // this kind works again
bool liveServiceAnswered();                                // the saved service host has given one complete HTTP answer (any status) since boot
bool liveStarted();                                        // liveBegin() has run: the lock behind liveNoteProblem exists
