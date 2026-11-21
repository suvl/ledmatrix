---
type: grilling
status: closed
---

## Question

How long may the panel go without updating before it looks broken?

## Resolution

**1 second is the maximum, total.** That is the whole budget for one refresh of the display, not per operation. Owner's words, 21 Nov 2026: "1 second is the maximum we will tolerate."

Consequences recorded so far:
- Every blocking call in the main loop has to fit inside that second together with drawing and serving the portal.
- Measured on the real board with the antenna on (ticket 009): a normal fetch stalls the loop 0.3 to 0.7 s, which fits. The first fetch after boot takes about 4.4 s (TLS handshake), and a network failure can stall 5 to 25 s. Neither fits without a mitigation. Whether boot and reconnects are exempt is still open.
- Owner decided to build features first and run the stall-attribution measurement later, once the real workload exists. The tripwire in ticket 001 (a visible freeze over 1 s after the cheap mitigations) is the trigger to reopen Rust.

### Added 21 Nov 2026 (owner)
- **Boot is exempt** from the 1 s budget.
- **Data cycle cap: 500 ms**, covering everything: DNS, TLS, request out, response in, JSON parsing. Timeouts must be capped well below that. "5 seconds is always unacceptable."
- Not yet decided: reconnects after boot (idle-closed connection, Wi-Fi blip). See below.
- Fit against measurements (ticket 009, antenna on): steady fetches took 0.32 to 0.57 s over a kept-alive connection, so 500 ms is right at the edge and depends on the network and API latency. A fresh connection (DNS + TLS handshake) took about 4.4 s, roughly nine times the cap. `adafruit_requests` cannot enforce a total deadline: DNS has no timeout parameter, TLS handshake steps have per-operation timeouts, and it retries once.
