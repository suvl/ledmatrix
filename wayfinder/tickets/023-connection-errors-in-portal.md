---
type: task
status: open
---

## Question

Show the last connection and TLS error, with its reason, in the portal, and keep a short history.

## Notes (22-24 Nov 2026)

Today the portal shows only the last problem as one line. Wanted: the last N errors with time, what was being fetched (flights, weather, headlines, logo, clock), and the reason in plain words (certificate not yet valid because the clock is wrong, unknown certificate authority, handshake timeout, DNS failure, HTTP status), so a lapsed certificate store or a broken network can be told apart without a serial cable. Ties to ticket 019 (trust store) and 015 (dual stack).

### Decided (owner, 24 Nov 2026)
- **Portal:** a list of the last 10 problems, each with its time, what was being fetched (flights, weather, headlines, logo, clock, update check) and the reason in plain words. Kept in memory, empty after a restart.
- **Panel:** keeps the blinking stale-data dot, and also shows a **message that tells the user to look at the portal**.

Still to decide: when the message appears (what counts as a problem worth interrupting for), how often and for how long it shows, what it says, and how it respects planes, night blanking and the burn-in rules.
