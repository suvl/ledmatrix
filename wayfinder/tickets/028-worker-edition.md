---
type: grilling
status: closed
---

## Question

Should the data service also exist as a TypeScript Cloudflare Worker, so a newcomer can host it for free with no server?

## Notes (24 Nov 2026)

Raised by the owner. Findings: feasible; all four jobs are fetch and reshape. Go does not run natively on Workers, so this is a second implementation. Free plan limits (10 ms CPU, 50 outside calls, 1,000 KV writes a day) mean the large IPMA observations file must be shrunk by a scheduled job into KV, or the paid plan used. The certificate chain (Google Trust Services WE1, GTS Root R4, GlobalSign Root CA, ECDSA leaf) is probably in the board's bundle but is untested; the board's cold ECDSA handshake took about 2.3 s earlier. `workers.dev` has IPv6. The service would be public, and logos would be deployed from the owner's machine.

Open: build it or not, which comes first (the first feature set and the documentation), the cost of keeping two implementations in step (a shared test suite of recorded upstream answers would help), and whether Cloud Run (same container, free) already serves the need.

## Resolution (owner, 24 Nov 2026)

**Not pursued.** Two implementations would never both be maintained; if a Worker were the right home, everything would move there, and it is not (CPU limit on the free plan, a rewrite, a public service, an untested certificate path). The free hosted route is Google Cloud Run, which runs the existing container unchanged. Closed as decided against; reopen only as a full move, not an addition.
