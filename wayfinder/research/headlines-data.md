# Headline data source research (research date 2026-11-21)

## Summary and recommendation

- **Best: Google News RSS search, behind a small cloud service.** No key, no account, ~240 req/day is trivial, covers pt-PT/pt-BR/en, returns title + source + pubDate, host has AAAA and an ECDSA cert (so cloud-side only). Raw feed is ~120 KB / 100 items, so the device must NOT fetch it directly.
- **Fallback: Currents API** (free, 250 req/day, commercial use allowed with attribution, `language` + `country` filters, AAAA, but ECDSA cert) or **GNews** (100 req/day free, 12 h delay, RSA cert + AAAA). Both need a key; use them from the cloud service too.
- Rejected for this device: NewsAPI.org (dev-only licence, 24 h delay), Mediastack (100 calls/MONTH), NewsData.io (no AAAA, 12 h delay, terms forbid network sharing), GDELT (429s, very slow, no AAAA, no per-article clean titles verified).
- Direct-from-device is only viable for a key-based JSON API with a tiny response (GNews/Currents with page size 1-5); even then each ECDSA host costs ~2.3 s per handshake. Cloud service strongly preferred: it de-dups, shrinks to `[{t,s,ts}]` (~300 B), hides keys, caches per topic, and the device talks to one RSA host.
- Request pattern (cloud side): `https://news.google.com/rss/search?q=<urlencoded topic> when:1d&hl=pt-PT&gl=PT&ceid=PT:pt-150` (English: `hl=en-US&gl=US&ceid=US:en`). Parse `<item><title>`, `<source>`, `<pubDate>`, sort by pubDate desc yourself, strip the trailing " - Source" from title.

## Network facts (dig + openssl s_client run 2026-11-21)

| Host | AAAA | Leaf key | Chain (certs sent) |
|---|---|---|---|
| news.google.com | yes | ECDSA P-256 | 3 (GTS WE2, Root R4) |
| gnews.io | yes | RSA-2048 | 3 (LE YR1, Root YR RSA-4096) |
| api.currentsapi.services | yes (Cloudflare) | ECDSA P-256 | 4 (LE YE1, Root YE, ISRG X2) |
| newsapi.org | no | ECDSA P-256 | 3 (GTS WE1, Root R4) |
| api.mediastack.com | yes (Cloudflare) | ECDSA P-256 | 3 (GTS WE1) |
| newsdata.io | no | RSA-2048 | 2 (GoDaddy G2) |
| content.guardianapis.com | CNAME `kong.content-aws...`; AAAA query returned no address records (treat as no) | RSA-2048 | 3 (Amazon) |
| api.nytimes.com | CNAME chain; no AAAA address returned | RSA-2048 | 3 (GTS WR3, RSA-4096 root) |
| api.gdeltproject.org | no | RSA-2048 (wildcard) | 3 (LE YR2, Root YR) |
| eventregistry.org | no | RSA-2048 | 4 (Sectigo R36, 3072-bit) |

Measured TLS connect (curl, from a Mac): GDELT 6.6 s (!), others 0.1-0.8 s.

## 1. Google News RSS (RECOMMENDED, via cloud)

- Request (real, HTTP 200): `GET https://news.google.com/rss/search?q=weather&hl=pt-PT&gl=PT&ceid=PT:pt-150`. Also `q=weather+when:1d` (time operator works; `when:1h` etc. unverified).
- Fields per `<item>`: `title` (publisher name appended as " - NPR"), `link` (opaque news.google.com redirect, not the publisher URL), `guid`, `pubDate` (RFC 822 GMT), `description` (HTML snippet), `<source url="https://www.npr.org">NPR</source>`.
- Size: 119,752 B (pt-PT, 100 items) and 128,182 B (en-US `when:1d`, 100 items). No server-side item limit parameter found; cannot shrink on the wire. Items were NOT strictly newest-first (pubDate sequence 18:22, 18:03, 16:17, 18:14, 19:16 ...), so sort client-side.
- Terms: the feed `<copyright>` says it is "made available solely for the purpose of rendering Google News results within a personal feed reader for personal, non-commercial use. Any other use ... is expressly prohibited." A personal LED display for one device is plausibly in that spirit but is not literally a feed reader (judgement call; no formal API ToS read). No key, no stated quota; unofficial, may rate-limit (429/CAPTCHA) or change without notice. Keep polling modest (cache per topic, 30 min).
- Cost/key: none. Language/country: the `hl/gl/ceid` triple selects edition (verified pt-PT and en-US; other editions such as pt-BR `hl=pt-BR&gl=BR&ceid=BR:pt-419` unverified). Free-text queries work (city, subject).
- Caveats: the title suffix and duplicates across outlets (same story, many sources) need de-dup; paywalled sources appear; link is a redirect (irrelevant for a display).

## 2. GDELT DOC 2.0 (no key)

- Request: `https://api.gdeltproject.org/api/v2/doc/doc?query=weather%20sourcelang:english&mode=artlist&maxrecords=5&format=json&sort=datedesc` (params per https://blog.gdeltproject.org/gdelt-doc-2-0-api-debuts/: `maxrecords` default 75 max 250, `sort=datedesc`, `sourcelang`, `sourcecountry`, `timespan` min 15 min).
- Live test: THREE GETs (spaced 8-20 s, 3 total) all returned HTTP 429 after 8-15 s: "Please limit requests to one every 5 seconds or contact ..."; real response size therefore **unverified**. Per-field JSON (url, title, seendate, domain, language, sourcecountry) is from memory, unverified this session (docs page does not list them).
- Terms/quota: only the 1 req / 5 s notice seen; no explicit licence read. Free, no key.
- No AAAA; RSA chain of 3; 6.6 s connect time measured; slow, shared-IP throttling. Multilingual (65 languages claimed in general GDELT docs; unverified), query text matched against GDELT-translated/original titles; titles are scraped so can be noisy/duplicated. Verdict: poor fit for a battery-less but timing-sensitive device; usable only from a server as a language-broad fallback.

## 3. NewsAPI.org

- Request: `GET https://newsapi.org/v2/everything?q=<q>&language=pt&sortBy=publishedAt&pageSize=5&apiKey=KEY` (also `/v2/top-headlines` with `country`). Fields (title, source.name, publishedAt) from docs knowledge, not re-read this session (unverified).
- Free "Developer" plan (https://newsapi.org/pricing): 100 requests/day, "Articles have a 24 hour delay", search up to a month back, CORS for localhost only. Terms (https://newsapi.org/terms): "may be used for development and testing in a development environment only, and cannot be used in a staging or production environment (including internally)". A live gadget breaks that; 240 req/day also exceeds 100.
- No AAAA; ECDSA. Verdict: not suitable.

## 4. GNews

- Request (https://docs.gnews.io/endpoints/search-endpoint): `GET https://gnews.io/api/v4/search?q=<q>&lang=pt&country=pt&max=5&sortby=publishedAt&apikey=KEY`. Params: `q` (<=200 chars, AND/OR/NOT), `lang`, `country` (source country), `max` (Free 10), `in` (title,description,content), `nullable`, `from`, `to`, `sortby` (publishedAt default | relevance), `truncate=content`, `page`.
- Response fields (OpenAPI): `articles[].id,title,description,content,url,image,publishedAt (UTC ISO 8601),lang,source{id,name,url,country}`. Free-plan content cut to 250 chars. Size with 5 articles estimated 3-5 KB (unverified; no key). Per-request, the unneeded fields cannot be dropped by parameter except `nullable`, so the cloud service should shrink.
- Free limits (https://gnews.io/pricing): 100 requests/day, up to 10 articles, 12-hour delay, last 30 days; 1 req/s; 403 on quota. 240/day would need paid. Commercial use: "No ... requires a paid plan". Personal hobby use is not addressed explicitly (unverified); the 12 h delay defeats "newest" headlines.
- AAAA yes; RSA-2048 leaf (0.4 s handshake class). Language coverage broad incl. pt and en (list unverified).

## 5. NewsData.io

- Endpoint `https://newsdata.io/api/1/latest?apikey=KEY&q=<q>&language=pt&country=pt&size=10` is from memory; docs page `/documentation/latest-news` returned 404 and the main page did not show it (unverified).
- Free (https://newsdata.io/pricing): 200 credits/day, 1-10 articles per credit, "Data delayed by 12 hours", 48 h history. Terms (https://newsdata.io/terms): must display "Powered by NewsData.io API" attribution; API may not be made "available over a network where it could be used by others"; key not shareable; a free-plan clause was not found.
- No AAAA; RSA-2048 with 2-cert chain. Verdict: workable only via cloud with attribution; 12 h delay.

## 6. Mediastack

- Free plan (https://mediastack.com/product): "100 Calls / mo", "Delayed News Data", "Non-Commercial Use". 100/month cannot cover even one day of the target load. Endpoint docs moved to docs.apilayer.com, not read. AAAA yes, ECDSA. Verdict: out.

## 7. The Guardian Open Platform

- Access page (https://open-platform.theguardian.com/access/): Developer key "for any non-commercial usage", up to 1 call/s, 500 calls/day, article text, free. Key requires registration.
- Request: `https://content.guardianapis.com/search?q=<q>&order-by=newest&page-size=5&show-fields=trailText&api-key=KEY` (parameter names from memory, docs pages are JS-rendered; unverified). Fields: `webTitle, webPublicationDate, sectionName, webUrl`; source name is always "The Guardian". Could not fetch terms page content (attribution rules unverified; the Guardian normally asks for a "Powered by the Guardian" credit).
- English only (UK/US/AU editions), single publisher, no language filter needed. RSA-2048, no AAAA address. Good for English world/UK topics, poor for Portuguese or local news; real-time, no delay.

## 8. NYT APIs

- Article Search / Top Stories need a key (free registration). Limits from memory: 500 requests/day, 5/min (unverified; developer.nytimes.com blocked fetch). Terms restrict use to non-commercial with attribution (unverified). English only, single publisher; no language filter; no AAAA address; RSA. Verdict: not suited to free-text city topics in Portuguese.

## 9. Currents API

- Pricing page (https://www.currentsapi.services/en/pricing) free row: 250 requests/day, 20 results per request, 30 days history; "every self-service plan, including Free, allows commercial use with attribution and a link to the original article". Free delay not stated on page (unverified); no card.
- OpenAPI (https://currentsapi.services/json/swagger.json): `GET /search` params `keywords, language, category, country, start_date, end_date, page_number, page_size, limit, type, domain, domain_not, author, has_image, has_description`; `/latest-news` takes `language, category, country, page_size`. Pattern: `https://api.currentsapi.services/v1/search?keywords=<q>&language=pt&country=PT&page_size=5&apiKey=KEY` (v1 prefix and response field names `news[].title,published,author,url` from memory, unverified; I did not call it without a key).
- AAAA yes (Cloudflare); leaf ECDSA P-256 with a 4-cert chain (largest handshake cost on the device). 250/day is just above the 240/day budget, so pair with caching. Coverage wide; Portuguese listed in /available/languages (not checked).

## 10. Event Registry (NewsAPI.ai)

- https://eventregistry.org/pricing is client-rendered; no free-tier numbers obtained (unverified). From memory it is a token-based, mostly paid product with a small free token allotment and a non-commercial "free" plan; very rich fields. No AAAA, RSA with 4-cert chain. Not recommended for a 240 req/day hobby load.

## 11. Others

- Bing News Search API: Microsoft retired the Bing Search APIs in August 2025 (from memory, not re-verified here); treat as unavailable.
- Publisher RSS (BBC, Publico, RTP, Observador etc.): typically no key, small-ish feeds but fixed topics, no free-text search, so they do not satisfy "owner-set free-text topics" except as a fixed-feed supplement (not tested).

## Device vs cloud service

- Direct from device: only plausible with a keyed JSON API at page size 1-5 (GNews RSA, 0.4 s). Cost: key stored on device, per-host cert handling, up to 5 TLS sessions per cycle, worse reliability and no shared de-dup. Google RSS cannot be fetched directly (120 KB XML).
- Cloud service: fetches Google RSS per topic/locale with cache (30 min), de-dups by normalized title, sorts by pubDate, strips " - Source", returns a ~200-400 B JSON or even a delimited text line per topic over a single RSA-cert host with AAAA, and can fall back to Currents/GNews with a key. This also lets one change source without reflashing. Recommended.

## Unverified items to confirm before building

- Google RSS: rate limit behaviour under steady 30-min polling, pt-BR edition parameters, `when:` granularity.
- Currents v1 base path, response schema, free-plan delay; GNews/Guardian real response sizes (need keys).
- GDELT live response and size (blocked by 429 in this session).
