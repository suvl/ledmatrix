// Networking: Wi-Fi station, the setup access point with its form, mDNS, and a small status page.
// Everything runs in one task pinned to core 0, so it never touches the render task on core 1.
#pragma once
#include <stdint.h>

enum class NetState : uint8_t {
  Connecting,  // joining the saved network (or scanning before the setup network starts)
  Online,      // joined; portal reachable at http://ledmatrix.local
  Offline,     // saved network configured but not reachable
  Setup        // no saved network: the "ledmatrix" access point is up
};

struct NetInfo {
  NetState state = NetState::Connecting;
  char ip[20] = "";       // station address when online
  char apSsid[20] = "";   // setup network name
  char apPass[12] = "";   // setup network password (random each time setup starts)
  char apIp[16] = "";     // setup network address
};

void netBegin();                 // starts the network task
NetInfo netInfo();               // thread-safe snapshot
void netWipeAndRestart();        // forgets the Wi-Fi network and restarts into setup mode
bool netWifiJoined();           // the station has been joined to the Wi-Fi at some time since boot (a flag, for the probation task)
bool netPortalAnswering();       // the portal server is up and its loop ran within the last 5 s (the probation checklist, otalogic.h Health)
