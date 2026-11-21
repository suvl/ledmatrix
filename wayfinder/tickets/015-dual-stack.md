---
type: grilling
status: closed
---

## Question

Which IP versions must the device support?

## Resolution (owner, 21 Nov 2026)

**Dual stack, IPv4 and IPv6, everywhere.** The owner expects it as a given in 2026.

Verified on the real board (21 Nov 2026):
- The board gets an IPv4 address, an IPv6 link-local address and a **global IPv6 address** (SLAAC) on the owner's network.
- The portal answers on both: `curl -4` and `curl -6` to `ledmatrix.local` return 200 in 30 to 100 ms. mDNS publishes the IPv6 address as well.
- Without `WiFi.enableIPv6()` the board answered mDNS with IPv4 only, and macOS waited a full 5 s for the missing IPv6 record on every lookup.

Still to verify (slice 4, live data):
- Outbound fetches to the flight, weather and news servers work over IPv6 and over IPv4, including when only one of them is usable. lwIP has no happy-eyeballs fallback, so a broken IPv6 route could make fetches hang. The fetch code needs a short connect timeout and a retry on the other family.
- The setup access point (`192.168.4.1`) is IPv4 only. That is deliberate for now: phones joining an isolated setup network need nothing else.
- The global IPv6 address is derived from the MAC address (EUI-64), so it is stable and identifies the device. Acceptable for a LAN gadget; revisit if the portal is ever exposed beyond the LAN.

### Update 21 Nov 2026: the service link
- The service hostname has only an IPv4 `A` record (a DNS-only record to a private LAN address; public resolvers return the same). It is not a Cloudflare tunnel name, and the owner cannot add an AAAA record for it.
- Decision: the board-to-service link stays **IPv4 on the dual-stack LAN** for now. The device itself is dual-stack (portal on IPv4 and IPv6, uses IPv6 for any host that publishes it); the limit is the record, not the firmware.
- Publishing the node's global IPv6 address in public DNS was rejected: it is publicly routable and, depending on the router firewall, could expose a service that has no login.
- To verify in slice 4: the board's HTTPS client works against hosts with IPv4 only, IPv6 only and both.
