---
type: grilling
status: closed
---

## Question

What triggers the reset into setup mode (the Wi-Fi access point), and what name does the device advertise on the LAN?

## Resolution (owner, 21 Nov 2026)

- **Name:** `ledmatrix`, used for both the mDNS name and the setup access point's SSID. The portal is at `http://ledmatrix.local`. (The owner first said `ledmatrixcorredeira`, then shortened it as too long.) Replaces the placeholder `flights` used in the test sketches, which still say `flights` until the next flash.
- **What the long press wipes:** the Wi-Fi credentials only, then the device starts the `ledmatrix` access point. Location, radius, altitude, feature toggles and headline queries are kept.
- **Trigger:** a long press on one of the board's two buttons. The owner does not care which. **UP (GPIO6)** is used. (An earlier version of this note said DOWN is GPIO0, the boot pin, which would make UP the safer choice. That was wrong: the Arduino pin definitions give UP = GPIO6 and DOWN = GPIO7. The boot pin GPIO0 is the separate small Boot button. Either button would work; UP stays as chosen.) The press length is not yet fixed (about 5 s is the suggested starting value).
- **Setup network security (owner agreed, 21 Nov 2026):** the `ledmatrix` access point is WPA2 protected and the panel shows the network name, the password and `192.168.4.1` while setup mode is active, so only someone who can see the panel can join. Default I'd propose, not yet confirmed: a fresh random password each time setup starts (8 characters, no look-alike characters), rather than a fixed one.
- Still open: random versus fixed setup password, the press length (suggested about 5 s), and whether a separate full "factory reset" exists in the portal.
