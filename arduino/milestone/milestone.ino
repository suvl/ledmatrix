// Milestone 012: Protomatter + Wi-Fi/TLS interference test on the Matrix Portal S3.
//
//  core 1: render task      64x32 animation at 4-bit depth, 50 fps, measures its own frame gaps
//  core 0: fetch task       Wi-Fi, then a COLD HTTPS fetch every 30 s (new client, new handshake),
//                           rotating hosts (ECDSA flights, ECDSA weather, RSA comparison),
//                           then an NVS write with the matrix paused
//  core 0: portal task      WebServer on :80 with /status, advertised as flights.local
//
// Serial output (115200) logs per-phase timings: dns, connect (TCP+TLS), request/response, parse.
// Crash forensics: the last stage reached by each task is kept in RTC memory and printed after reboot,
// together with the reset reason.

#include <Adafruit_Protomatter.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_system.h>

#include "certs.h"
#include "secrets.h"

// ---- panel wiring (Matrix Portal S3), 64x32 uses address lines A-D --------------------------
static uint8_t rgbPins[] = {42, 41, 40, 38, 39, 37};
static uint8_t addrPins[] = {45, 36, 48, 35};
static const uint8_t clockPin = 2;
static const uint8_t latchPin = 47;
static const uint8_t oePin = 14;
Adafruit_Protomatter matrix(64, 4, 1, rgbPins, 4, addrPins, clockPin, latchPin, oePin, true);

// ---- test hosts --------------------------------------------------------------------------------
struct Host {
  const char *name;
  const char *path;
  const char *rootCa;
  bool headOnly;
  bool json;
  const char *kind;
};
// Arbitrary coordinates (central Europe), not any real location of the owner.
static const Host HOSTS[] = {
  {"api.adsb.lol", "/v2/lat/48.0/lon/10.0/dist/30", ROOT_CA_api_adsb_lol, false, true, "ECDSA flights"},
  {"api.open-meteo.com", "/v1/forecast?latitude=48.0&longitude=10.0&current=temperature_2m,weather_code",
   ROOT_CA_api_open_meteo_com, false, true, "ECDSA weather"},
  {"www.amazon.com", "/", ROOT_CA_www_amazon_com, false, false, "RSA-2048 compare"},
};
static const size_t HOST_COUNT = sizeof(HOSTS) / sizeof(HOSTS[0]);
static const uint32_t FETCH_EVERY_MS = 30000;
// Experiment switch: 1 = stop/resume the matrix around the NVS write, 0 = write with the matrix running.
// Suspect: matrix.resume() leaves the panel black after the first settings write (about 5 s after boot).
#ifndef PAUSE_FOR_NVS
#define PAUSE_FOR_NVS 0
#endif
static const size_t BODY_MAX = 16 * 1024;

// ---- shared stats (written by one task each, read by others; plain volatile is enough here) ------
struct FetchResult {
  int httpStatus = 0;
  int err = 0;
  uint32_t dnsMs = 0, connectMs = 0, requestMs = 0, parseMs = 0, totalMs = 0;
  size_t bytes = 0;
};
static volatile uint32_t gFrames = 0;
static volatile uint32_t gGapMaxBoot = 0;
static volatile uint32_t gGapMaxWindow = 0;
static volatile uint32_t gPauseMs = 0;
static volatile bool gPauseReq = false;
static volatile bool gPaused = false;
static volatile uint32_t gFetchCount = 0, gFetchErrors = 0, gNvsCount = 0;
static FetchResult gLast;
static const char *gLastHost = "";
static bool gMatrixOk = false;

// Survive a panic/watchdog reset so we can see where each task was.
static const uint32_t STAGE_MAGIC = 0xC0FFEE12;
RTC_NOINIT_ATTR static uint32_t gStageMagic;
RTC_NOINIT_ATTR static volatile uint32_t gStageFetch;
RTC_NOINIT_ATTR static volatile uint32_t gStageRender;
RTC_NOINIT_ATTR static uint32_t gStageUptime;

static WebServer server(80);
static Preferences prefs;

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
  }
}

// ---- render task (core 1) ---------------------------------------------------------------------------
static void renderTask(void *) {
  gStageRender = 1;
  ProtomatterStatus st = matrix.begin();
  Serial.printf("RENDER matrix.begin() = %d\n", (int)st);
  gMatrixOk = (st == PROTOMATTER_OK);
  uint32_t last = millis();
  int x = 0;
  for (;;) {
    uint32_t now = millis();
    uint32_t gap = now - last;
    last = now;
    if (gap > gGapMaxBoot) gGapMaxBoot = gap;
    if (gap > gGapMaxWindow) gGapMaxWindow = gap;
    gFrames = gFrames + 1;
    gStageUptime = now;

    if (gPauseReq) {  // settings write in progress on core 0: stop the refresh around flash access
      gStageRender = 30;
      matrix.stop();
      gPaused = true;
      gStageRender = 31;
      uint32_t p0 = millis();
      while (gPauseReq) vTaskDelay(1);
      gStageRender = 32;
      matrix.resume();
      gStageRender = 33;
      gPaused = false;
      gPauseMs = millis() - p0;
      last = millis();
    }

    if (gMatrixOk) {
      gStageRender = 40;
      matrix.fillScreen(0);
      matrix.drawRect(0, 0, 64, 32, matrix.color565(0, 64, 255));
      matrix.fillRect(x, 12, 4, 4, matrix.color565(255, 176, 0));
      matrix.setCursor(3, 3);
      matrix.setTextColor(matrix.color565(255, 255, 255));
      matrix.print("F:");
      matrix.print(gFrames);
      matrix.setCursor(3, 21);
      matrix.setTextColor(matrix.color565(0, 255, 0));
      matrix.print(gFetchCount);
      matrix.print("/");
      matrix.print(gFetchErrors);
      gStageRender = 41;
      matrix.show();
      gStageRender = 42;
      x = (x + 1) % 60;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---- HTTP helpers -------------------------------------------------------------------------------------------
static bool readExact(NetworkClientSecure &c, uint8_t *dst, size_t want, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < want && millis() - last < timeoutMs) {
    int avail = c.available();
    if (avail > 0) {
      int r = c.read(dst + got, min((size_t)avail, want - got));
      if (r > 0) {
        got += r;
        last = millis();
      }
    } else if (!c.connected()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  return got == want;
}

// Reads the body (plain or chunked) into body[0..max-2]; returns the byte count.
static size_t readBody(NetworkClientSecure &c, char *body, size_t max, bool chunked) {
  size_t n = 0;
  if (!chunked) {
    uint32_t last = millis();
    while ((c.connected() || c.available()) && n < max - 1 && millis() - last < 5000) {
      int avail = c.available();
      if (avail > 0) {
        int r = c.read((uint8_t *)body + n, min((size_t)avail, max - 1 - n));
        if (r > 0) {
          n += r;
          last = millis();
        }
      } else {
        vTaskDelay(1);
      }
    }
    return n;
  }
  for (;;) {
    String sizeLine = c.readStringUntil('\n');
    size_t chunk = strtoul(sizeLine.c_str(), nullptr, 16);
    if (chunk == 0) break;
    if (n + chunk > max - 1) break;  // larger than this sketch's buffer: stop
    if (!readExact(c, (uint8_t *)body + n, chunk, 5000)) break;
    n += chunk;
    c.readStringUntil('\n');  // CRLF after the chunk
  }
  return n;
}

// ---- one cold HTTPS fetch ----------------------------------------------------------------------------
static bool fetchOnce(const Host &h, FetchResult &r) {
  gStageFetch = 1;
  uint32_t t0 = millis();
  IPAddress ip;
  if (!WiFi.hostByName(h.name, ip)) {
    r.err = -1;
    return false;
  }
  r.dnsMs = millis() - t0;
  gStageFetch = 2;

  NetworkClientSecure client;  // new object each time: no reuse, full handshake
  client.setCACert(h.rootCa);
  client.setHandshakeTimeout(8000);
  client.setTimeout(5000);
  uint32_t t1 = millis();
  if (!client.connect(h.name, 443, 8000)) {
    char buf[96] = {0};
    r.err = client.lastError(buf, sizeof(buf));
    Serial.printf("FETCH %s connect failed: %s\n", h.name, buf);
    return false;
  }
  r.connectMs = millis() - t1;
  gStageFetch = 3;

  uint32_t t2 = millis();
  client.printf("%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: matrix-milestone\r\nAccept: application/json\r\n"
                "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                h.headOnly ? "HEAD" : "GET", h.path, h.name);
  gStageFetch = 4;
  String statusLine = client.readStringUntil('\n');
  int sp = statusLine.indexOf(' ');
  r.httpStatus = (sp > 0) ? statusLine.substring(sp + 1).toInt() : 0;
  bool chunked = false;
  for (;;) {  // skip headers
    String line = client.readStringUntil('\n');
    if (line == "\r" || (line.length() == 0 && !client.connected())) break;
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0) chunked = true;
  }
  gStageFetch = 5;
  char *body = (char *)malloc(BODY_MAX);
  if (!body) {
    r.err = -2;
    return false;
  }
  size_t n = h.headOnly ? 0 : readBody(client, body, BODY_MAX, chunked);
  body[n] = 0;
  r.bytes = n;
  r.requestMs = millis() - t2;
  gStageFetch = 6;

  if (h.json && n > 0) {
    uint32_t t3 = millis();
    JsonDocument filter;
    filter["ac"][0]["hex"] = true;
    filter["ac"][0]["flight"] = true;
    filter["current"]["temperature_2m"] = true;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body, n, DeserializationOption::Filter(filter));
    r.parseMs = millis() - t3;
    if (e) {
      r.err = 100 + (int)e.code();
      Serial.printf("FETCH %s json error: %s\n", h.name, e.c_str());
    }
  }
  gStageFetch = 7;
  free(body);
  client.stop();
  gStageFetch = 8;
  r.totalMs = millis() - t0;
  return r.httpStatus >= 200 && r.httpStatus < 400 && r.err == 0;
}

// ---- fetch task (core 0) -----------------------------------------------------------------------------
static void fetchTask(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t w0 = millis();
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(100));
  Serial.printf("WIFI up in %lu ms ip %s rssi %d\n", (unsigned long)(millis() - w0), WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
  if (MDNS.begin("flights")) MDNS.addService("http", "tcp", 80);

  prefs.begin("ms", false);
  size_t idx = 0;
  uint32_t next = millis();
  for (;;) {
    if ((int32_t)(millis() - next) >= 0) {
      next += FETCH_EVERY_MS;
      const Host &h = HOSTS[idx];
      idx = (idx + 1) % HOST_COUNT;
      FetchResult r;
      bool ok = fetchOnce(h, r);
      gLast = r;
      gLastHost = h.name;
      gFetchCount = gFetchCount + 1;
      if (!ok) gFetchErrors = gFetchErrors + 1;
      uint32_t win = gGapMaxWindow;
      gGapMaxWindow = 0;
      Serial.printf("FETCH %-20s %-16s %s http=%d err=%d dns=%lu connect=%lu req=%lu parse=%lu total=%lu bytes=%u "
                    "render_gap_max=%lu heap=%u psram=%u rssi=%d\n",
                    h.name, h.kind, ok ? "ok " : "FAIL", r.httpStatus, r.err, (unsigned long)r.dnsMs,
                    (unsigned long)r.connectMs, (unsigned long)r.requestMs, (unsigned long)r.parseMs,
                    (unsigned long)r.totalMs, (unsigned)r.bytes, (unsigned long)win, (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getFreePsram(), WiFi.RSSI());

      // settings write with the matrix paused (Protomatter issue 71: flash writes can blank the display)
      gStageFetch = 20;
      gPauseReq = (PAUSE_FOR_NVS != 0);
      uint32_t p0 = millis();
      while (PAUSE_FOR_NVS && !gPaused && millis() - p0 < 500) vTaskDelay(1);
      gStageFetch = 21;
      uint32_t w1 = millis();
      gNvsCount = gNvsCount + 1;
      prefs.putUInt("n", gNvsCount);
      uint32_t wms = millis() - w1;
      gStageFetch = 22;
      gPauseReq = false;
      gStageFetch = 23;
      Serial.printf("NVS write %lu ms (paused=%d)\n", (unsigned long)wms, (int)gPaused);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---- portal task (core 0) -------------------------------------------------------------------------------
static void handleStatus() {
  char buf[480];
  snprintf(buf, sizeof(buf),
           "{\"uptime_s\":%lu,\"frames\":%lu,\"render_gap_max_boot_ms\":%lu,\"fetches\":%lu,\"fetch_errors\":%lu,"
           "\"last_host\":\"%s\",\"last_connect_ms\":%lu,\"last_total_ms\":%lu,\"nvs_writes\":%lu,"
           "\"last_pause_ms\":%lu,\"heap\":%u,\"rssi\":%d}",
           (unsigned long)(millis() / 1000), (unsigned long)gFrames, (unsigned long)gGapMaxBoot,
           (unsigned long)gFetchCount, (unsigned long)gFetchErrors, gLastHost, (unsigned long)gLast.connectMs,
           (unsigned long)gLast.totalMs, (unsigned long)gNvsCount, (unsigned long)gPauseMs, (unsigned)ESP.getFreeHeap(),
           WiFi.RSSI());
  server.send(200, "application/json", buf);
}

static void portalTask(void *) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(200));
  server.on("/", []() { server.send(200, "text/html", "<h1>flights.local</h1><p>milestone 012</p>"); });
  server.on("/status", handleStatus);
  server.begin();
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // give the host a moment to attach before the first logs
  esp_reset_reason_t why = esp_reset_reason();
  Serial.printf("\nBOOT reset_reason=%s (%d)\n", resetReasonName(why), (int)why);
  if (gStageMagic == STAGE_MAGIC && why != ESP_RST_POWERON) {
    Serial.printf("BOOT previous run: last fetch stage=%lu render stage=%lu at uptime %lu ms\n",
                  (unsigned long)gStageFetch, (unsigned long)gStageRender, (unsigned long)gStageUptime);
  }
  gStageMagic = STAGE_MAGIC;
  gStageFetch = 0;
  gStageRender = 0;
  gStageUptime = 0;
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(fetchTask, "fetch", 24576, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(portalTask, "portal", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
