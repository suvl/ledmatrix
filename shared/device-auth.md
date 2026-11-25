# Device authentication, scheme v1

The board proves to the owner's service that it is the owner's device by signing every request. There is no token and no password. Decision: ticket #37. Build: ticket #39. This page is the contract between the firmware (C++), the service (Go) and `tools/provision.py`. The known-answer vectors in `device-auth-vectors.json` are the test for all three: same key, same bytes, same answer.

Every example value in this repository is a placeholder. The real master secret, device IDs and keys never go in the repository, a ticket, a log or a commit.

## Names and keys

- **Device ID**: the board's factory MAC address as 12 lowercase hexadecimal characters of text, for example `aabbccddeeff`. Valid means exactly `[0-9a-f]{12}`; anything else (upper case, colons, other length) is malformed.
- **Master secret**: text that only the owner knows, at least 32 characters, surrounding whitespace (space, tab, CR, LF) removed, then used as the raw bytes of the HMAC key. It lives outside the repository (a file or an environment variable on the owner's computer, and a secret in the service's cluster). It needs an offline backup: after the permanent key burn a lost master secret cannot be recovered and the board would be locked out of the data service for good.
- **Device key** (32 bytes) = HMAC-SHA256(key = master secret bytes, message = device ID text bytes). The service never stores device keys: it derives the key from the claimed device ID on every request. The only things it keeps are the master secret and the list of revoked device IDs.

## The signed request

The board sends these three headers on every request to the owner's service (never to any other host):

```
X-Device: aabbccddeeff
X-Timestamp: 1796000000
X-Signature: v1=<64 lowercase hex characters>
```

`X-Timestamp` is Unix time in seconds, decimal digits only. The signature is HMAC-SHA256(device key, canonical string), written as lowercase hex. The canonical string is six lines joined with a single `\n`, with no trailing newline:

```
v1
<METHOD in capitals>
<host>
<request target>
<timestamp>
<lowercase hex SHA-256 of the body>
```

- `v1` is the scheme version. The version in `X-Signature` must match; an unknown version is `malformed`.
- `<host>` is the host name exactly as the board writes it in its `Host:` header (no port, lower case). If a proxy rewrites the `Host` header the service can be told the public name with its `PUBLIC_HOST` setting; when `PUBLIC_HOST` is set the service uses that and ignores the request's `Host`.
- `<request target>` is the request target exactly as it appears in the request line: path and query, as sent, for example `/v1/flights?lat=38.7&lon=-9.1`. The service uses `r.RequestURI`, never a re-encoded copy.
- `<timestamp>` is the same text as in `X-Timestamp`.
- The body hash is the SHA-256 of the request body. The board sends only `GET`, so its hash is always that of the empty string, `e3b0c442...b855`. A `GET` or `HEAD` that carries a body (a `Content-Length` other than 0, or any `Transfer-Encoding`) is `malformed` and its body is never read; for other methods the service reads at most 4 KB of body before the signature is checked (413 above that).

## What the service checks, and in which order

1. The three headers are present and well formed (`X-Device` valid, `X-Timestamp` digits only, at most 12 characters, `X-Signature` is `v1=` plus 64 hex characters). Otherwise `missing` (no signature headers at all) or `malformed`.
2. The signature matches the key derived from `X-Device` (constant-time comparison). Otherwise `bad_signature`. Nothing else is revealed to a caller who cannot sign: no `serverTime` field and no revoked status. (The HTTP `Date` header that Go and every proxy add to every response is not a secret and is not covered by this.)
3. The device is not revoked. Otherwise `revoked`.
4. The timestamp is within 120 seconds of the service's clock (either direction). Otherwise `clock_skew`, and only now is the service's time sent back.

A request with a valid signature, a device that is not revoked and a timestamp in the window is accepted.

Routes that do not need a signature (today only `/healthz`; the firmware update manifest and download in slice 10) ignore these headers completely, so a board whose key is refused can still fetch a fix.

## The refusal

Status 401 with a JSON body. `error` is one plain English sentence for a human; `reason` is the code the board acts on; `serverTime` (Unix seconds) is present only for `clock_skew`:

```json
{"error":"The request is signed with the wrong key.","reason":"bad_signature"}
{"error":"The board's clock is more than 2 minutes off.","reason":"clock_skew","serverTime":1796000123}
```

Reasons: `missing`, `malformed`, `bad_signature`, `revoked`, `clock_skew`. (There is no separate "unknown device": any well-formed ID has a derivable key, so a wrong ID looks like a bad signature.) The service logs the device ID and the reason and never a key, a signature or the master secret. It never bans a device permanently: `revoked` comes only from the owner's list.

## What the board does with a refusal

- It keeps its key and its last data. It never wipes anything and never reboots because of a 401.
- `clock_skew`: it stores the offset (serverTime minus its own clock) in RAM and retries the same request once immediately, signing with the corrected time. The offset is used only for signing.
- Any other refusal (or a board that cannot sign): it retries slowly, every 5 minutes for every kind of fetch, never faster, restoring the normal schedule at once when a request succeeds, and puts a plain sentence in its problem list.
- The board does not fetch from the service before NTP has set its clock (TLS certificate checks need the time anyway), so `clock_skew` only happens when the service's clock and the board's NTP time differ by more than 2 minutes. The stored offset is dropped when NTP steps the system clock, and replaced again by the next `clock_skew`.

## The identity record on the board

The read-only flash partition `ident` (8 KB at `0x7FE000`, never written by the firmware) holds two 4 KB sectors, each a full copy of the record. The firmware reads sector 0, and sector 1 if sector 0 is not valid. All integers are little-endian. Layout of one sector (the rest is `0xFF`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `0x544E4449` (the bytes `I D N T`) |
| 4 | 2 | record version, `1` |
| 6 | 2 | payload length in bytes, `48` |
| 8 | 4 | CRC-32 (the plain zlib CRC-32) of the payload |
| 12 | 12 | device ID, ASCII, as defined above |
| 24 | 1 | flags: bit 0 = the key is burned into the chip's eFuse and the software key must not be used |
| 25 | 3 | reserved, zero |
| 28 | 32 | interim software key (the device key), all zero once the key is burned |

The firmware refuses to sign if the record is missing or invalid, if the device ID in the record is not the chip's own factory MAC (a copied record from another board), or if bit 0 is set and the hardware signer is not there yet. Each of those is a plain sentence in the problem list; none of them reboots or wipes anything.

## Vectors

`device-auth-vectors.json` holds: the RFC 4231 anchor, the master secret with and without surrounding whitespace, the device ID and key, four complete requests (canonical string and signature, one with a body), a length table for the hardware signer (0 to 200 bytes) and a list of malformed device IDs. The Go tests, the C++ host test and `tools/provision.py` all read this file.
