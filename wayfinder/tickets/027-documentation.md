---
type: grilling
status: open
---

## Question

What documentation does the project need, where does it live, and what must it contain?

## Requested by the owner (24 Nov 2026)

A good README and good documents covering: the architecture; build instructions; install instructions; which components must be kept up to date for security; day-0, day-1 and day-2 operations.

Constraints already known: the repository is public and must contain no private information (no real host names, addresses, names or emails; use placeholders such as `apis.example.com`); the airline logos are trademarks and stay out of the repository; the documents should match the finished first feature set, so they are written after the features in tickets 021 to 026 are built.

### Decided: form (owner, 24 Nov 2026)
Markdown in the repository: a short `README.md` as the front door and a `docs/` folder with one page per subject. Diagrams are written as text (Mermaid) and drawn by GitHub. No documentation site. Documents change in the same commits as the code.

### Proposed page list, owner may object
- `README.md`: what it is, a photo or preview, the parts list, a five-line quick start, links.
- `docs/architecture.md`: the components (board firmware and its tasks, the portal, the service, the data sources, the cluster), data flow, the display's 1 second budget, IPv4/IPv6, trust and privacy decisions, with diagrams.
- `docs/build.md`: tools and versions, building the firmware, the portal build step, running the service tests, the release build.
- `docs/install.md`: wiring and hardware, first flash, setup access point and Wi-Fi, deploying the service (plain Docker and Kubernetes), the logo set, first settings.
- `docs/security.md`: what must be kept up to date and how often (arduino-esp32 and its certificate bundle and TLS library, the other Arduino libraries, the Go toolchain, the container base image, the GitHub Actions versions, the browser libraries the portal loads, the cluster components, the router and Wi-Fi), the portal's no-password LAN-only model, the update code, what is private.
- `docs/operations.md`: day-0 (first install and setup), day-1 (settings, checks, what normal looks like), day-2 (updates, rollback, logs and problems, backups of settings, replacing the board, when things go wrong).
- `docs/decisions.md` or a pointer to the wayfinder tickets for the why.

### Decided: audience (owner, 24 Nov 2026)
**A beginner.** Every step is explained, including installing the tools and what each term means (a glossary page is added to the list). Consequences accepted for the writing: the pages are longer; each command says what it does and what a good result looks like; a "if something goes wrong" note follows each risky step; terms are defined once in the glossary and linked.

Known hard part for a beginner, to be settled: the board needs the data service reachable over HTTPS with a certificate from a normal certificate authority, which means a domain name and a certificate, not just a running container. The install page needs a simple, honest path for that.

### Decided: operating systems (owner, 24 Nov 2026)
**macOS and Linux.** The commands are tested on macOS; Linux notes are marked "not tested here" until someone runs them. Windows is not covered for now (it can be added later after someone has run it there). Tested and untested sections are clearly labelled; nothing is presented as tested that was not.

### Decided: hosting paths for the install page (owner, 24 Nov 2026)
- **Main path: a home server on the LAN** with Docker Compose, Caddy (automatic certificate through the DNS provider) and the owner's own domain name. One Compose file.
- **References, to be tried the same way:** a small rented server (public, token protected), Kubernetes as an advanced appendix (the owner's setup with placeholder names), and **free hosted options**. Facts gathered for the free options (24 Nov 2026, to be re-checked when writing):
  - Google Cloud Run: runs the existing Go container unchanged; permanent free tier of about 2 million requests a month; HTTPS address automatic; needs a billing account; public.
  - Cloudflare Workers: would need a TypeScript port (see ticket 028). Free plan: 10 ms CPU per request, 50 outside calls, 1,000 KV writes a day; certificate chain Google Trust Services WE1 / GTS Root R4 / GlobalSign Root CA with an ECDSA leaf (not yet tested on the board); has IPv6 addresses.

### Decided: free hosted route (owner, 24 Nov 2026)
**Google Cloud Run**, running the existing container; the Worker idea is closed (ticket 028). The page must cover what Cloud Run needs: the container already listens on port 8080, which Cloud Run expects; the service is public on the internet, so it needs an access token (ticket 029); the airline logos cannot go in a public image, so the owner builds a small private image on top of the public one with the logo folder added and pushes it to their own private registry; a cold start of a second or so is fine for the display; the free allowance and billing-account requirement are re-checked when writing.
