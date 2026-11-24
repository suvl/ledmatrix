---
type: grilling
status: closed
---

## Question

Should the board be able to send an access token to the data service, so the service can be hosted somewhere public?

## Notes (24 Nov 2026)

The service already checks an optional token in the `X-Token` header, but the firmware and the portal have no field for it, so today only a LAN-only service works. Any public hosting (Cloud Run, a rented server) needs it, and the documentation's alternative hosting paths depend on it.

Open: a "Service access token" field in the portal's Data service section; stored on the board with the other settings and never shown back in full; sent as `X-Token` on every request including logos and the update check; the self-test shows a clear "wrong token" message on HTTP 401; empty means no header.

## Resolution (owner, 24 Nov 2026)

**Yes.** A "Service access token" field in the portal's Data service section; stored on the board with the other settings and never shown back in full once saved (the portal shows that one is set, with a way to replace or clear it); sent as `X-Token` on every request to the service, including logos and the update check; a wrong token gives a plain message in the connection test and in the problem list (ticket 023); empty means no header, so the owner's LAN-only setup is unchanged.
