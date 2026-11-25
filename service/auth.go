package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// Device authentication, scheme v1. The contract is shared/device-auth.md and the known-answer
// vectors are shared/device-auth-vectors.json; read them before changing anything here.
//
// The board signs every request with a key derived from the owner's master secret and its own
// device ID: key = HMAC-SHA256(master secret, device ID). The service derives the key again on
// every request and stores nothing but the master secret and the list of revoked device IDs.

const (
	reasonMissing      = "missing"
	reasonMalformed    = "malformed"
	reasonBadSignature = "bad_signature"
	reasonRevoked      = "revoked"
	reasonClockSkew    = "clock_skew"

	maxSkewSeconds = 120     // a timestamp may be this far from the service's clock, either way
	maxSignedBody  = 4 << 10 // other methods than GET and HEAD may carry at most this many bytes of body
	minSecretChars = 32

	// Refusals are logged at most once per (device, reason) per window and at most refusalBudget
	// lines per window in total, so an internet scanner cannot flood the log or the memory.
	refusalWindow = time.Minute
	refusalBudget = 30
	refusalCap    = 256 // hard limit on remembered (device, reason) pairs
)

var refusalText = map[string]string{
	reasonMissing:      "The request is not signed.",
	reasonMalformed:    "The signature headers are not in the expected form.",
	reasonBadSignature: "The request is signed with the wrong key.",
	reasonRevoked:      "This device is not allowed to use the service.",
	reasonClockSkew:    "The board's clock is more than 2 minutes off.",
}

type authenticator struct {
	off        bool // AUTH_OFF=1: nothing is checked (local development only)
	master     []byte
	revoked    map[string]bool
	publicHost string // replaces the request's Host in the canonical string when set
	now        func() time.Time
	logf       func(format string, args ...any)
	refusals   refusalLog
}

// authFromEnv reads and checks the configuration. It refuses (returns an error) rather than guess:
// no secret and no explicit AUTH_OFF=1, a secret that is too short, both at once, a bad revoked ID.
// No message contains the value of MASTER_SECRET.
func authFromEnv(get func(string) string, logf func(string, ...any)) (*authenticator, error) {
	au := &authenticator{now: time.Now, logf: logf}
	rawSecret := get("MASTER_SECRET")
	switch trimWS(get("AUTH_OFF")) {
	case "1":
		au.off = true
	case "", "0":
	default:
		return nil, errors.New("AUTH_OFF must be 1 (local development only) or left unset")
	}
	if au.off && rawSecret != "" {
		return nil, errors.New("both AUTH_OFF=1 and MASTER_SECRET are set; set only one (MASTER_SECRET for a real service, AUTH_OFF=1 for local development)")
	}
	if !au.off {
		if rawSecret == "" {
			return nil, errors.New("MASTER_SECRET is not set, so the service will not start; create one with `openssl rand -hex 32` (for local development only, set AUTH_OFF=1 instead)")
		}
		secret := trimWS(rawSecret)
		if utf8.RuneCountInString(secret) < minSecretChars {
			return nil, fmt.Errorf("MASTER_SECRET is shorter than %d characters (after removing surrounding whitespace); create one with `openssl rand -hex 32`", minSecretChars)
		}
		au.master = []byte(secret)
	}
	var err error
	if au.revoked, err = parseRevoked(get("REVOKED_DEVICES")); err != nil {
		return nil, err
	}
	if h := trimWS(get("PUBLIC_HOST")); h != "" {
		if strings.ContainsAny(h, "/\\@ \t\r\n") {
			return nil, errors.New("PUBLIC_HOST must be a plain host name such as example.com, without a scheme, path or spaces")
		}
		au.publicHost = normalizeHost(h)
	}
	if au.off {
		logf("WARNING: AUTH_OFF=1, request signatures are NOT checked. Use this only on your own computer, never on a service that other people can reach.")
	}
	return au, nil
}

// trimWS removes the whitespace the contract names (space, tab, CR, LF) and nothing else.
func trimWS(s string) string { return strings.Trim(s, " \t\r\n") }

// parseRevoked reads a comma-separated list of device IDs. Empty entries (a trailing comma) are
// ignored; any other entry that is not a device ID is an error, named by position, never by value.
func parseRevoked(list string) (map[string]bool, error) {
	out := map[string]bool{}
	for i, e := range strings.Split(list, ",") {
		e = trimWS(e)
		if e == "" {
			continue
		}
		if !validDeviceID(e) {
			return nil, fmt.Errorf("REVOKED_DEVICES entry %d is not a device ID (12 lowercase hexadecimal characters)", i+1)
		}
		out[e] = true
	}
	return out, nil
}

// normalizeHost lower-cases a host and removes the port.
func normalizeHost(h string) string {
	h = strings.ToLower(h)
	if strings.HasPrefix(h, "[") { // IPv6 literal: keep the brackets, drop what follows
		if i := strings.IndexByte(h, ']'); i >= 0 {
			return h[:i+1]
		}
		return h
	}
	if i := strings.LastIndexByte(h, ':'); i >= 0 && strings.IndexByte(h, ':') == i {
		return h[:i]
	}
	return h
}

func validDeviceID(s string) bool { return len(s) == 12 && isLowerHex(s) }

func isLowerHex(s string) bool {
	for i := 0; i < len(s); i++ {
		if c := s[i]; !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func validTimestamp(s string) bool {
	if len(s) < 1 || len(s) > 12 {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func hmacSHA256(key, msg []byte) []byte {
	m := hmac.New(sha256.New, key)
	m.Write(msg)
	return m.Sum(nil)
}

func deviceKey(master []byte, deviceID string) []byte { return hmacSHA256(master, []byte(deviceID)) }

// canonicalString is the text the board signs: six lines joined by \n, no trailing newline.
func canonicalString(method, host, requestURI, timestamp, bodySHA256Hex string) string {
	return strings.Join([]string{"v1", strings.ToUpper(method), host, requestURI, timestamp, bodySHA256Hex}, "\n")
}

// signedHeaders is what a well-formed request carries.
type signedHeaders struct {
	device    string
	timestamp string
	sig       []byte
}

// refusal is an answer that is not "accepted". For 401 it is the contract's JSON body; the other
// statuses (body too large or unreadable) are plain errors without a reason.
type refusal struct {
	status     int
	closeConn  bool   // the request body was left unread, so the connection must not be reused or waited on
	device     string // the device ID if it is well formed, else "-"; this is all the log ever shows
	code       string // what the log shows; for 401 also the "reason" in the answer
	msg        string
	serverTime *int64
}

func refuse(device, code string) *refusal {
	return &refusal{status: http.StatusUnauthorized, device: device, code: code, msg: refusalText[code]}
}

func (f *refusal) body() any {
	if f.status != http.StatusUnauthorized {
		return map[string]string{"error": f.msg}
	}
	return struct {
		Error      string `json:"error"`
		Reason     string `json:"reason"`
		ServerTime *int64 `json:"serverTime,omitempty"`
	}{f.msg, f.code, f.serverTime}
}

// readSignedHeaders is check 1: the three headers are present and well formed.
func readSignedHeaders(r *http.Request) (signedHeaders, *refusal) {
	dev, ts, sig := r.Header.Values("X-Device"), r.Header.Values("X-Timestamp"), r.Header.Values("X-Signature")
	if len(dev)+len(ts)+len(sig) == 0 {
		return signedHeaders{}, refuse("-", reasonMissing)
	}
	id := "-"
	if len(dev) == 1 && validDeviceID(dev[0]) {
		id = dev[0]
	}
	// A header sent twice is ambiguous, so it is malformed.
	if id == "-" || len(ts) != 1 || len(sig) != 1 || !validTimestamp(ts[0]) {
		return signedHeaders{}, refuse(id, reasonMalformed)
	}
	raw, ok := strings.CutPrefix(sig[0], "v1=")
	if !ok || len(raw) != 64 || !isLowerHex(raw) {
		return signedHeaders{}, refuse(id, reasonMalformed)
	}
	b, err := hex.DecodeString(raw)
	if err != nil {
		return signedHeaders{}, refuse(id, reasonMalformed)
	}
	return signedHeaders{device: id, timestamp: ts[0], sig: b}, nil
}

// hasBody reports whether a request announces a body: a Content-Length other than 0 (-1 means
// unknown, as in a chunked request) or any Transfer-Encoding.
func hasBody(r *http.Request) bool { return r.ContentLength != 0 || len(r.TransferEncoding) > 0 }

// readBody reads at most maxSignedBody bytes and puts them back for the handler. It runs before the
// signature is checked, so the limit is small: a caller who cannot sign makes us hold this much.
func readBody(w http.ResponseWriter, r *http.Request) ([]byte, error) {
	if r.Body == nil {
		return nil, nil
	}
	b, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxSignedBody))
	if err != nil {
		return nil, err
	}
	r.Body = io.NopCloser(bytes.NewReader(b))
	return b, nil
}

func bodyRefusal(device string, err error) *refusal {
	var tooBig *http.MaxBytesError
	if errors.As(err, &tooBig) {
		return &refusal{status: http.StatusRequestEntityTooLarge, device: device, code: "body_too_large", msg: "The request body is too large."}
	}
	return &refusal{status: http.StatusBadRequest, device: device, code: "body_unreadable", msg: "The request body could not be read."}
}

func (au *authenticator) host(r *http.Request) string {
	if au.publicHost != "" {
		return au.publicHost
	}
	return normalizeHost(r.Host)
}

// verify runs the checks in the contract's order: shape, signature, revoked, time window. It
// returns nil when the request is accepted. Nothing later in the order is revealed earlier: a
// caller who cannot sign learns neither the server time nor whether a device is revoked.
func (au *authenticator) verify(w http.ResponseWriter, r *http.Request) *refusal {
	h, f := readSignedHeaders(r)
	if f != nil {
		return f
	}
	// The board sends only GET. A GET or HEAD with a body is refused without reading a byte of it,
	// so many half-open connections with a large Content-Length cost no memory.
	if (r.Method == http.MethodGet || r.Method == http.MethodHead) && hasBody(r) {
		f := refuse(h.device, reasonMalformed)
		f.closeConn = true
		return f
	}
	body, err := readBody(w, r)
	if err != nil {
		return bodyRefusal(h.device, err)
	}
	sum := sha256.Sum256(body)
	canonical := canonicalString(r.Method, au.host(r), r.RequestURI, h.timestamp, hex.EncodeToString(sum[:]))
	if !hmac.Equal(h.sig, hmacSHA256(deviceKey(au.master, h.device), []byte(canonical))) {
		return refuse(h.device, reasonBadSignature)
	}
	if au.revoked[h.device] {
		return refuse(h.device, reasonRevoked)
	}
	now := au.now().Unix()
	ts, _ := strconv.ParseInt(h.timestamp, 10, 64) // at most 12 digits: cannot overflow
	if d := now - ts; d > maxSkewSeconds || d < -maxSkewSeconds {
		f := refuse(h.device, reasonClockSkew)
		f.serverTime = &now
		return f
	}
	return nil
}

// require wraps a handler so that it only runs for a request with a valid signature. A nil
// authenticator refuses everything (fail closed).
func (au *authenticator) require(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case au == nil:
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "authentication is not configured"})
		case au.off:
			next.ServeHTTP(w, r)
		default:
			if f := au.verify(w, r); f != nil {
				au.refusals.note(au.now(), au.logf, f.device, f.code)
				if f.closeConn {
					w.Header().Set("Connection", "close")
				}
				writeJSON(w, f.status, f.body())
				return
			}
			next.ServeHTTP(w, r)
		}
	})
}

// refusalLog limits the log lines for refusals. The only things it ever logs are a device ID that
// passed the shape check (or "-") and a reason code from a fixed set: no header value, no key, no
// signature, no secret.
type refusalLog struct {
	mu      sync.Mutex
	seen    map[string]time.Time // "device reason" -> when it was last logged
	start   time.Time            // start of the current window
	lines   int                  // lines logged in the current window
	dropped int                  // refusals in the current window that hit the budget or the cap
}

func (l *refusalLog) note(now time.Time, logf func(string, ...any), device, code string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.seen == nil || now.Sub(l.start) >= refusalWindow || now.Before(l.start) {
		if l.dropped > 0 {
			logf("auth refused: %d more refusals in the last minute were not logged", l.dropped)
		}
		for k, t := range l.seen {
			if now.Sub(t) >= refusalWindow || now.Before(t) {
				delete(l.seen, k)
			}
		}
		if l.seen == nil {
			l.seen = map[string]time.Time{}
		}
		l.start, l.lines, l.dropped = now, 0, 0
	}
	key := device + " " + code
	if t, ok := l.seen[key]; ok && now.Sub(t) < refusalWindow {
		return // already logged this minute
	}
	if l.lines >= refusalBudget || len(l.seen) >= refusalCap {
		l.dropped++
		return
	}
	l.seen[key] = now
	l.lines++
	logf("auth refused: device=%s reason=%s", device, code)
}
