---
type: research
status: closed
---

**Research finished 21 Nov 2026**, awaiting the owner's decision: best is Google News RSS search (no key, title, source and time, language and country filters; unofficial, "personal, non-commercial use" feed notice; about 120 KB per 100 items so only a cloud service can use it). Fallbacks with a key: Currents API (250 requests/day, attribution) and GNews (100/day, 12 h delay). Rejected: NewsAPI (dev-only licence), Mediastack, NewsData.io, GDELT (rate-limited, slow TLS).

## Resolution (21 Nov 2026)

**Google News RSS search**, through the service, which de-duplicates, sorts newest first and returns title, source and time. Unofficial feed; its notice says personal, non-commercial use. Currents API is the keyed fallback if the feed ever blocks the service.

## Question

Which news source returns the newest headlines for up to five owner-set topics, in English and Portuguese, free for a hobby device at up to 240 requests a day, allowed on a personal display, with IPv6 and a cheap TLS handshake? Findings go in `wayfinder/research/headlines-data.md`.
