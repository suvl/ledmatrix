---
type: task
status: closed
---

## Question

Show the last connection and TLS error, with its reason, in the portal, and keep a short history.

## Notes (22-24 Nov 2026)

Today the portal shows only the last problem as one line. Wanted: the last N errors with time, what was being fetched (flights, weather, headlines, logo, clock), and the reason in plain words (certificate not yet valid because the clock is wrong, unknown certificate authority, handshake timeout, DNS failure, HTTP status), so a lapsed certificate store or a broken network can be told apart without a serial cable. Ties to ticket 019 (trust store) and 015 (dual stack).

### Decided (owner, 24 Nov 2026)
- **Portal:** a list of the last 10 problems, each with its time, what was being fetched (flights, weather, headlines, logo, clock, update check) and the reason in plain words. Kept in memory, empty after a restart.
- **Panel:** keeps the blinking stale-data dot, and also shows a **message that tells the user to look at the portal**.

Still to decide: when the message appears (what counts as a problem worth interrupting for), how often and for how long it shows, what it says, and how it respects planes, night blanking and the burn-in rules.

## Resolution (owner, 24 Nov 2026)

- **Portal:** the last 10 problems with time, what was being fetched (flights, weather, headlines, logo, clock, update check) and the reason in plain words (clock wrong so a certificate looks not yet valid, unknown certificate authority, handshake timeout, DNS failure, HTTP status). In memory, empty after a restart.
- **Panel:** keeps the blinking stale dot, and shows a short message telling the user to look at the portal **only after something has failed for one hour in a row** (owner's choice; a router restart or an ISP drop never triggers it).
- Defaults chosen without asking, owner may object: the message shows for 5 seconds once a minute until the problem clears; it never replaces a plane card (planes keep priority) and stays dark during night blanking; it names the portal address; after the problem clears it stops by itself and the portal keeps the history; it counts per kind of data, so a long weather outage alone also triggers it.
