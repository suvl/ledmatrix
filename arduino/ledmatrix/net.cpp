#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <vector>

#include "identity.h"
#include "live.h"
#include "ota.h"
#include "portal_page.h"
#include "settings.h"


extern volatile uint32_t gFrames, gFrameMaxMs, gFramesOver1s;  // render task statistics (ledmatrix.ino)
namespace {

SemaphoreHandle_t lockHandle = nullptr;
NetInfo info;
WebServer server(80);
DNSServer dns;
volatile bool wifiJoinedOnce = false;  // the station loop saw the Wi-Fi joined (netWifiJoined)
volatile uint32_t portalBeatMs = 0;  // the last time the station loop served the web server (netPortalAnswering)

// The display name (devicename.h) this start runs with: the mDNS host name, the DHCP host name and the setup Wi-Fi name. Read once when
// the net task starts. A new name is saved at once but used only after a restart, so the portal restarts the display when it changes
// (registerStationRoutes); this copy is what "the name in use" means.
char deviceName[devicename::MAX_LEN + 1] = "ledmatrix";

void setState(NetState s) {
  xSemaphoreTake(lockHandle, portMAX_DELAY);
  info.state = s;
  xSemaphoreGive(lockHandle);
}

String htmlEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += c;
    }
  }
  return o;
}

String pageStart(const char *title) {
  String h = F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>");
  h += title;
  h += F("</title><style>body{font-family:system-ui,sans-serif;background:#0d0f12;color:#e6e8ec;margin:0;padding:20px}"
         "main{max-width:420px;margin:0 auto}h1{font-size:22px;margin:0 0 4px}p{color:#8d96a3}"
         "label{display:block;margin:16px 0 4px;font-size:14px;color:#8d96a3}"
         "input,select,button{width:100%;box-sizing:border-box;font:inherit;padding:10px;border-radius:8px;border:1px solid #232830;background:#14171c;color:#e6e8ec}"
         "button{background:#ffb000;color:#1a1300;border:0;font-weight:600;margin-top:20px;cursor:pointer}"
         "code{background:#14171c;padding:2px 6px;border-radius:4px}</style></head><body><main>");
  return h;
}
const char *const PAGE_END = "</main></body></html>";

// ---- setup mode ------------------------------------------------------------------------------------------
struct NetItem {
  String ssid;
  int rssi;
};

std::vector<NetItem> scanNetworks() {
  std::vector<NetItem> out;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (auto &e : out)
      if (e.ssid == s) {
        dup = true;
        if (WiFi.RSSI(i) > e.rssi) e.rssi = WiFi.RSSI(i);
      }
    if (!dup) out.push_back({s, (int)WiFi.RSSI(i)});
  }
  WiFi.scanDelete();
  for (size_t i = 1; i < out.size(); i++)  // strongest first
    for (size_t j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) std::swap(out[j], out[j - 1]);
  if (out.size() > 20) out.resize(20);
  return out;
}

String randomPassword() {
  static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // no 0/O/1/I look-alikes
  String p;
  for (int i = 0; i < 8; i++) p += alphabet[esp_random() % (sizeof(alphabet) - 1)];
  return p;
}

String setupForm(const std::vector<NetItem> &nets, const String &error) {
  String h = pageStart("ledmatrix setup");
  h += F("<h1>ledmatrix</h1><p>Choose the Wi-Fi network this display should use.</p>");
  if (error.length()) h += "<p style='color:#ff7a45'>" + htmlEscape(error) + "</p>";
  h += F("<form method=post action=/save><label for=ssid>Network</label><select name=ssid id=ssid>");
  for (auto &n : nets) h += "<option value=\"" + htmlEscape(n.ssid) + "\">" + htmlEscape(n.ssid) + "</option>";
  h += F("</select><label for=manual>Network not listed? Type its name</label><input name=manual id=manual maxlength=32 autocapitalize=off autocorrect=off>"
         "<label for=pass>Password</label><input type=password name=pass id=pass maxlength=63 autocomplete=off>"
         "<button type=submit>Save and connect</button></form>");
  h += PAGE_END;
  return h;
}

[[noreturn]] void runSetup() {
  setState(NetState::Connecting);  // the panel shows "connecting" while we scan
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));
  std::vector<NetItem> nets = scanNetworks();

  String pass = randomPassword();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(deviceName, pass.c_str());
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);  // captive portal: every name resolves to us

  xSemaphoreTake(lockHandle, portMAX_DELAY);
  strlcpy(info.apSsid, deviceName, sizeof(info.apSsid));
  strlcpy(info.apPass, pass.c_str(), sizeof(info.apPass));
  strlcpy(info.apIp, ip.toString().c_str(), sizeof(info.apIp));
  info.state = NetState::Setup;
  xSemaphoreGive(lockHandle);
  Serial.printf("SETUP network \"%s\" password %s at %s\n", deviceName, pass.c_str(), ip.toString().c_str());

  server.on("/", HTTP_GET, [nets]() { server.send(200, "text/html", setupForm(nets, "")); });
  server.on("/save", HTTP_POST, [nets]() {
    String ssid = server.arg("manual");
    ssid.trim();
    if (ssid.length() == 0) ssid = server.arg("ssid");
    String pw = server.arg("pass");
    if (ssid.length() == 0 || ssid.length() > 32 || pw.length() > 63) {
      server.send(400, "text/html", setupForm(nets, "Enter a network name (up to 32 characters) and a password (up to 63)."));
      return;
    }
    settings::saveWifi(ssid, pw);
    Serial.printf("SETUP saved network \"%s\"\n", ssid.c_str());
    String h = pageStart("ledmatrix saved");
    h += "<h1>Saved</h1><p>The display is restarting and will join <b>" + htmlEscape(ssid) +
         "</b>. Reconnect your phone to your usual Wi-Fi, then open <code>http://" + String(deviceName) + ".local</code>. "
         "If the panel shows NO WIFI, hold the UP button for 5 seconds and try again.</p>";
    h += PAGE_END;
    server.send(200, "text/html", h);
    vTaskDelay(pdMS_TO_TICKS(2500));
    ESP.restart();
  });
  server.onNotFound([ip]() {  // captive-portal probes land here
    server.sendHeader("Location", String("http://") + ip.toString() + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  for (;;) {
    dns.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---- station mode ---------------------------------------------------------------------------------------------
void sendOta(const ota::Reply &r) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(r.http, "application/json", r.json);
}

void registerStationRoutes() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", PORTAL_HTML);
  });
  server.on("/api/settings", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", settings::toJson());
  });
  server.on("/api/settings", HTTP_POST, []() {
    String body = server.arg("plain"), error;
    if (!settings::setFromJson(body.c_str(), body.length(), error)) {
      server.send(400, "application/json", String("{\"error\":\"") + error + "\"}");
      return;
    }
    Serial.println("SETTINGS saved");
    server.send(200, "application/json", settings::toJson());
    // A new display name (ticket 45) reaches the network only at the next start: the mDNS host name, the DHCP name and the setup Wi-Fi name
    // are set once. So, like the Wi-Fi save, restart after the answer has gone out (the portal learns the new name from the answer and
    // follows the display). MDNS.end() and begin() again would not be enough: the DHCP name would stay stale and every client's cached
    // address too, so one clean start is simpler and safer. Not while an update is being installed: that ends with a restart of its own,
    // which applies the name.
    const Settings now = settings::get();
    if (strcmp(now.deviceName, deviceName) != 0 && !ota::flashing()) {
      Serial.printf("SETTINGS display name is now \"%s\": restarting to use it\n", now.deviceName);
      vTaskDelay(pdMS_TO_TICKS(1500));
      ESP.restart();
    }
  });
  server.on("/api/live", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", liveStatusJson());
  });
  server.on("/api/planes", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", livePlanesJson());
  });
  server.on("/api/problems", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", liveProblemsJson());
  });
  server.on("/api/selftest", HTTP_GET, []() {  // takes up to about 30 s: three real connections
    server.sendHeader("Cache-Control", "no-store");
    if (ota::flashing()) {  // it would wait for the one-request-at-a-time turn, which a download holds, and block the portal for minutes
      server.send(409, "application/json", "{\"error\":\"An update is being installed right now. Try the connection test afterwards.\"}");
      return;
    }
    server.send(200, "application/json", liveSelfTestJson(settings::get()));
  });
  server.on("/status", HTTP_GET, []() {
    char buf[780];  // the longest answer is about 500 bytes (every field at its maximum, IPv6 addresses as 45 characters), so there is room for more fields
    // uptime_s counts from esp_timer, 64 bits: millis() / 1000 would wrap to zero after about 49 days.
    unsigned long long uptimeS = (unsigned long long)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"ip6_link_local\":\"%s\",\"ip6_global\":\"%s\",\"rssi\":%d,\"uptime_s\":%llu,\"heap\":%u,\"min_heap\":%u,\"frames\":%lu,\"frame_max_ms\":%lu,\"frames_over_1s\":%lu,\"device\":\"%s\",\"identity\":\"%s\",\"signer\":\"%s\",\"signer_check\":\"%s\"}",
             ota::version(), WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.linkLocalIPv6().toString().c_str(),
             WiFi.globalIPv6().toString().c_str(), WiFi.RSSI(), uptimeS, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             (unsigned long)gFrames, (unsigned long)gFrameMaxMs, (unsigned long)gFramesOver1s, identity::deviceId(), identity::statusName(),
             identity::signerName(), identity::signerCheckName());
    server.send(200, "application/json", buf);
  });
  // Firmware updates (shared/firmware-update.md): every answer is the same JSON, built by ota.cpp. No answer ever holds the code.
  server.on("/api/update", HTTP_GET, []() { sendOta(ota::apiState()); });
  server.on("/api/update/check", HTTP_POST, []() { sendOta(ota::apiCheck()); });
  server.on("/api/update/start", HTTP_POST, []() {
    String body = server.arg("plain");
    sendOta(ota::apiStart(body.c_str(), body.length()));
  });
  server.on("/api/update/confirm", HTTP_POST, []() {
    String body = server.arg("plain");
    sendOta(ota::apiConfirm(body.c_str(), body.length()));
  });
  server.on("/api/update/cancel", HTTP_POST, []() { sendOta(ota::apiCancel()); });
  server.on(
      "/api/update/upload", HTTP_POST,
      []() {  // after the whole body: the answer (and the restart, when the file was accepted)
        ota::Reply r = ota::uploadFinish();
        sendOta(r);
        if (r.restart) {
          Serial.println("OTA restarting into the uploaded firmware");
          vTaskDelay(pdMS_TO_TICKS(2000));
          ESP.restart();
        }
      },
      []() {  // while the body arrives; the form fields before the file (the code) are known by the time the file starts
        HTTPUpload &u = server.upload();
        switch (u.status) {
          case UPLOAD_FILE_START: {
            String code = server.arg("code");
            ota::uploadBegin(code.c_str(), u.name.c_str(), server.clientContentLength());
            break;
          }
          case UPLOAD_FILE_WRITE: ota::uploadWrite(u.buf, u.currentSize); break;
          case UPLOAD_FILE_END: ota::uploadEnd(); break;
          case UPLOAD_FILE_ABORTED: ota::uploadAbort(); break;
        }
      });
  server.on("/forget", HTTP_POST, []() {
    String h = pageStart("ledmatrix");
    h += "<h1>Forgotten</h1><p>The display is restarting in setup mode. Join the network <b>" + String(deviceName) + "</b> using the password shown on the panel.</p>";
    h += PAGE_END;
    server.send(200, "text/html", h);
    vTaskDelay(pdMS_TO_TICKS(1500));
    netWipeAndRestart();
  });
}

// ---- DNS guard -------------------------------------------------------------------------------------------------
// With IPv6 enabled, the router's IPv6 announcement can replace the DHCP-provided DNS server with the
// router's link-local address (fe80::...). lwIP cannot use a link-local server (it needs to know the
// interface), so every name lookup then times out. Remember the IPv4 server DHCP gave us and put it
// back whenever the first server stops being usable. A global IPv6 server is fine and left alone.
IPAddress dhcpDns((uint32_t)0);

bool dnsUsable(const IPAddress &d) {
  if (d.type() == IPv4) return (uint32_t)d != 0;
  String s = d.toString();
  return !(s.startsWith("fe80") || s == "::");
}

void ensureDns() {
  IPAddress d0 = WiFi.dnsIP(0);
  if (dnsUsable(d0)) {
    if (d0.type() == IPv4 && (uint32_t)dhcpDns == 0) dhcpDns = d0;
    return;
  }
  IPAddress primary = (uint32_t)dhcpDns != 0 ? dhcpDns : WiFi.gatewayIP();
  WiFi.setDNS(primary, IPAddress((uint32_t)0));
  Serial.printf("DNS: first server %s is not usable, using %s\n", d0.toString().c_str(), primary.toString().c_str());
}

[[noreturn]] void runStation(const WifiCreds &c) {
  setState(NetState::Connecting);
  WiFi.mode(WIFI_STA);
  WiFi.enableIPv6();  // so mDNS can answer AAAA too; without it macOS waits 5 s for the missing record
  WiFi.setHostname(deviceName);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(c.ssid.c_str(), c.pass.c_str());
  uint32_t t0 = millis();
  bool services = false;
  uint32_t lastDnsCheck = 0;
  for (;;) {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up) wifiJoinedOnce = true;
    if (up && millis() - lastDnsCheck > 2000) {
      lastDnsCheck = millis();
      ensureDns();
    }
    if (up && !services) {
      if (MDNS.begin(deviceName)) MDNS.addService("http", "tcp", 80);
      registerStationRoutes();
      server.begin();
      services = true;
      Serial.printf("ONLINE %s as %s.local\n", WiFi.localIP().toString().c_str(), deviceName);
    }
    xSemaphoreTake(lockHandle, portMAX_DELAY);
    if (up) {
      info.state = NetState::Online;
      strlcpy(info.ip, WiFi.localIP().toString().c_str(), sizeof(info.ip));
    } else {
      info.state = (millis() - t0 < 20000) ? NetState::Connecting : NetState::Offline;
    }
    xSemaphoreGive(lockHandle);
    if (services) {
      server.handleClient();
      portalBeatMs = millis() ? millis() : 1;
    }
    vTaskDelay(pdMS_TO_TICKS(up ? 5 : 200));
  }
}

void netTask(void *) {
  strlcpy(deviceName, settings::get().deviceName, sizeof(deviceName));  // settings::begin() has run (ledmatrix.ino setup())
  WifiCreds c = settings::loadWifi();
  if (!c.valid()) runSetup();
  runStation(c);
}

}  // namespace

void netBegin() {
  lockHandle = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

NetInfo netInfo() {
  NetInfo copy;
  xSemaphoreTake(lockHandle, portMAX_DELAY);
  copy = info;
  xSemaphoreGive(lockHandle);
  return copy;
}

bool netWifiJoined() { return wifiJoinedOnce; }

bool netPortalAnswering() {
  uint32_t beat = portalBeatMs;
  return beat != 0 && millis() - beat < 5000;
}

void netWipeAndRestart() {
  settings::clearWifi();
  delay(100);
  ESP.restart();
}
