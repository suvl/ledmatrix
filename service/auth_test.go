package main

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// The device authentication tests. The known-answer vectors are read from the shared file at run
// time (shared/device-auth-vectors.json): the same bytes the firmware and tools/provision.py check.

const (
	testMaster = "example-master-secret-0123456789abcdef" // equals the vectors' master_secret (checked below)
	testDevice = "aabbccddeeff"
	testHost   = "ledmatrix.example.com"
	testNow    = int64(1796000000)
	weatherURI = "/v1/weather?lat=38.7&lon=-9.1"
)

type vectorFile struct {
	Primitive struct {
		KeyHex  string `json:"key_hex"`
		Message string `json:"message"`
		HMAC    string `json:"hmac"`
	} `json:"primitive_rfc4231_case2"`
	MasterSecret string          `json:"master_secret"`
	MasterWithWS string          `json:"master_secret_with_whitespace"`
	DeviceID     string          `json:"device_id"`
	DeviceKeyHex string          `json:"device_key_hex"`
	Requests     []vectorRequest `json:"requests"`
	LengthTable  struct {
		Rows []struct {
			Length int    `json:"length"`
			HMAC   string `json:"hmac"`
		} `json:"rows"`
	} `json:"length_table"`
	BadDeviceIDs []string `json:"bad_device_ids"`
}

type vectorRequest struct {
	Name       string `json:"name"`
	Method     string `json:"method"`
	Host       string `json:"host"`
	URI        string `json:"uri"`
	Timestamp  int64  `json:"timestamp"`
	Body       string `json:"body"`
	BodySHA256 string `json:"body_sha256"`
	Canonical  string `json:"canonical"`
	Signature  string `json:"signature"`
}

func loadVectors(t *testing.T) vectorFile {
	t.Helper()
	b, err := os.ReadFile("../shared/device-auth-vectors.json")
	if err != nil {
		t.Fatalf("the shared vectors must be readable (run the tests from a checkout of the whole repository): %v", err)
	}
	var v vectorFile
	if err := json.Unmarshal(b, &v); err != nil {
		t.Fatal(err)
	}
	return v
}

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

type envMap map[string]string

func (e envMap) get(k string) string { return e[k] }

func silent(string, ...any) {}

func fixedClock(unix int64) func() time.Time { return func() time.Time { return time.Unix(unix, 0) } }

// testAuthenticator is an authenticator for testMaster with the clock stopped at testNow.
func testAuthenticator(t *testing.T) *authenticator {
	t.Helper()
	au, err := authFromEnv(envMap{"MASTER_SECRET": testMaster}.get, silent)
	if err != nil {
		t.Fatal(err)
	}
	au.now = fixedClock(testNow)
	return au
}

// signWith builds a request the way the board does: it signs the host as written (no port), the
// request target as sent, the timestamp text and the body hash with the device key.
func signWith(master, device, host, method, uri string, ts int64, body string) *http.Request {
	req := httptest.NewRequest(method, uri, strings.NewReader(body))
	req.Host = host
	tsText := strconv.FormatInt(ts, 10)
	sum := sha256.Sum256([]byte(body))
	canon := canonicalString(method, host, uri, tsText, hex.EncodeToString(sum[:]))
	sig := hmacSHA256(hmacSHA256([]byte(master), []byte(device)), []byte(canon))
	req.Header.Set("X-Device", device)
	req.Header.Set("X-Timestamp", tsText)
	req.Header.Set("X-Signature", "v1="+hex.EncodeToString(sig))
	return req
}

func signedRequest(t *testing.T, method, uri string, ts int64, body string) *http.Request {
	t.Helper()
	return signWith(testMaster, testDevice, testHost, method, uri, ts, body)
}

type answer struct {
	code       int
	reason     string
	errText    string
	serverTime *int64
	raw        string
	header     http.Header
}

func serve(h http.Handler, req *http.Request) answer {
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	var b struct {
		Error      string `json:"error"`
		Reason     string `json:"reason"`
		ServerTime *int64 `json:"serverTime"`
	}
	json.Unmarshal(rec.Body.Bytes(), &b) // plain 200 bodies are not JSON; that is fine here
	return answer{rec.Code, b.Reason, b.Error, b.ServerTime, rec.Body.String(), rec.Header()}
}

// realMux is the service's real routing, with the stub upstreams behind it.
func realMux(t *testing.T) (http.Handler, *app) {
	t.Helper()
	a := newStub(t).app(t, lisbonNoon(t))
	return a.routes(), a
}

// echoMux runs a handler that echoes the body it can still read, behind the signature check.
func echoMux(au *authenticator) http.Handler {
	return au.require(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		w.Write(b)
	}))
}

func wantAccepted(t *testing.T, what string, got answer) {
	t.Helper()
	if got.code == http.StatusUnauthorized || got.reason != "" {
		t.Errorf("%s: want accepted, got %d %s", what, got.code, got.raw)
	}
}

// wantRefused checks a 401 with the given reason; serverTime must be present exactly when given.
func wantRefused(t *testing.T, what string, got answer, reason string, serverTime *int64) {
	t.Helper()
	if got.code != http.StatusUnauthorized || got.reason != reason {
		t.Errorf("%s: want 401 %s, got %d %s", what, reason, got.code, got.raw)
		return
	}
	if got.errText == "" || !strings.HasSuffix(got.errText, ".") {
		t.Errorf("%s: the error must be a plain sentence, got %q", what, got.errText)
	}
	if (serverTime == nil) != (got.serverTime == nil) || serverTime != nil && *serverTime != *got.serverTime {
		t.Errorf("%s: serverTime want %v, got %v (%s)", what, serverTime, got.serverTime, got.raw)
	}
}

func i64(v int64) *int64 { return &v }

func flipHexDigit(s string, i int) string {
	c := byte('0')
	if s[i] == '0' {
		c = '1'
	}
	return s[:i] + string(c) + s[i+1:]
}

// ---- known-answer vectors --------------------------------------------------------------------------------

func TestVectorPrimitive(t *testing.T) {
	v := loadVectors(t)
	got := hmacSHA256(mustHex(t, v.Primitive.KeyHex), []byte(v.Primitive.Message))
	if hex.EncodeToString(got) != v.Primitive.HMAC {
		t.Fatalf("HMAC-SHA256 does not match the RFC 4231 anchor: %x", got)
	}
}

func TestVectorDeviceKey(t *testing.T) {
	v := loadVectors(t)
	if v.MasterSecret != testMaster || v.DeviceID != testDevice {
		t.Fatal("the test constants must equal the vectors' master secret and device ID")
	}
	for name, secret := range map[string]string{"plain": v.MasterSecret, "whitespace around it": v.MasterWithWS} {
		au, err := authFromEnv(envMap{"MASTER_SECRET": secret}.get, silent)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if string(au.master) != v.MasterSecret {
			t.Errorf("%s: the master secret must be used trimmed", name)
		}
		if got := hex.EncodeToString(deviceKey(au.master, v.DeviceID)); got != v.DeviceKeyHex {
			t.Errorf("%s: device key %s, want %s", name, got, v.DeviceKeyHex)
		}
	}
}

func TestVectorRequests(t *testing.T) {
	v := loadVectors(t)
	if len(v.Requests) != 4 {
		t.Fatalf("expected the four vector requests, got %d", len(v.Requests))
	}
	key := mustHex(t, v.DeviceKeyHex)
	for _, r := range v.Requests {
		sum := sha256.Sum256([]byte(r.Body))
		if hex.EncodeToString(sum[:]) != r.BodySHA256 {
			t.Errorf("%s: body hash %x, want %s", r.Name, sum, r.BodySHA256)
		}
		canon := canonicalString(r.Method, r.Host, r.URI, strconv.FormatInt(r.Timestamp, 10), r.BodySHA256)
		if canon != r.Canonical {
			t.Errorf("%s: canonical string\n got %q\nwant %q", r.Name, canon, r.Canonical)
		}
		if got := hex.EncodeToString(hmacSHA256(key, []byte(r.Canonical))); got != r.Signature {
			t.Errorf("%s: signature %s, want %s", r.Name, got, r.Signature)
		}
		if lower := canonicalString(strings.ToLower(r.Method), r.Host, r.URI, strconv.FormatInt(r.Timestamp, 10), r.BodySHA256); lower != r.Canonical {
			t.Errorf("%s: the method must be written in capitals in the canonical string", r.Name)
		}
	}
}

func TestVectorLengthTable(t *testing.T) {
	v := loadVectors(t)
	key := mustHex(t, v.DeviceKeyHex)
	if len(v.LengthTable.Rows) == 0 {
		t.Fatal("no length table rows")
	}
	for _, row := range v.LengthTable.Rows {
		msg := make([]byte, row.Length)
		for i := range msg {
			msg[i] = byte(i*7 + 3)
		}
		if got := hex.EncodeToString(hmacSHA256(key, msg)); got != row.HMAC {
			t.Errorf("length %d: %s, want %s", row.Length, got, row.HMAC)
		}
	}
}

func TestVectorBadDeviceIDs(t *testing.T) {
	v := loadVectors(t)
	if len(v.BadDeviceIDs) == 0 {
		t.Fatal("no malformed device IDs in the vectors")
	}
	for _, id := range v.BadDeviceIDs {
		if validDeviceID(id) {
			t.Errorf("%q must be malformed", id)
		}
	}
	if !validDeviceID(v.DeviceID) {
		t.Errorf("%q must be valid", v.DeviceID)
	}
}

// The four vector requests, byte for byte, through the signature check with the clock at their own
// timestamps; then the first three through the real routes.
func TestVectorRequestsAreAccepted(t *testing.T) {
	v := loadVectors(t)
	vectorReq := func(r vectorRequest) *http.Request {
		req := httptest.NewRequest(r.Method, r.URI, strings.NewReader(r.Body))
		req.Host = r.Host
		req.Header.Set("X-Device", v.DeviceID)
		req.Header.Set("X-Timestamp", strconv.FormatInt(r.Timestamp, 10))
		req.Header.Set("X-Signature", "v1="+r.Signature)
		return req
	}
	for _, r := range v.Requests {
		au := testAuthenticator(t)
		au.now = fixedClock(r.Timestamp)
		got := serve(echoMux(au), vectorReq(r))
		if got.code != 200 || got.raw != r.Body {
			t.Errorf("%s: %d %q", r.Name, got.code, got.raw)
		}
	}
	h, a := realMux(t)
	want := []struct {
		code int
		body string
	}{{200, `"planes"`}, {200, `"loc":"Lisboa"`}, {404, "no logo"}} // 404: authorised, and the logo folder is empty
	for i, w := range want {
		a.auth.now = fixedClock(v.Requests[i].Timestamp)
		got := serve(h, vectorReq(v.Requests[i]))
		if got.code != w.code || !strings.Contains(got.raw, w.body) {
			t.Errorf("%s through the real routes: %d %s", v.Requests[i].Name, got.code, got.raw)
		}
	}
}

// ---- the real routes: every outcome ------------------------------------------------------------------------

func TestAuthAccepted(t *testing.T) {
	h, _ := realMux(t)
	got := serve(h, signedRequest(t, "GET", weatherURI, testNow, ""))
	if got.code != 200 || !strings.Contains(got.raw, `"loc":"Lisboa"`) {
		t.Fatalf("a signed request must be answered: %d %s", got.code, got.raw)
	}
	// the Host header may carry a port and upper case: the board signs the bare lower-case name
	req := signedRequest(t, "GET", weatherURI, testNow, "")
	req.Host = "LedMatrix.Example.COM:8443"
	if got := serve(h, req); got.code != 200 {
		t.Errorf("port and case in the Host header: %d %s", got.code, got.raw)
	}
	// a request target with escapes and a trailing query is signed as sent
	got = serve(h, signedRequest(t, "GET", "/v1/headlines?q=%C3%A9lections&n=2", testNow, ""))
	if got.code == 401 {
		t.Errorf("escaped query: %s", got.raw)
	}
	// the old header is ignored, not required
	req = signedRequest(t, "GET", weatherURI, testNow, "")
	req.Header.Set("X-Token", "anything")
	if got := serve(h, req); got.code != 200 {
		t.Errorf("a stray X-Token must not matter: %d", got.code)
	}
}

func TestAuthRefusalAnswerFormat(t *testing.T) {
	h, _ := realMux(t)
	got := serve(h, httptest.NewRequest("GET", weatherURI, nil))
	if got.code != 401 || got.header.Get("Content-Type") != "application/json" || got.header.Get("Cache-Control") != "no-store" {
		t.Fatalf("%d %v", got.code, got.header)
	}
	req := signedRequest(t, "GET", weatherURI, testNow, "")
	sig := req.Header.Get("X-Signature")
	req.Header.Set("X-Signature", flipHexDigit(sig, len(sig)-1))
	got = serve(h, req)
	if want := `{"error":"The request is signed with the wrong key.","reason":"bad_signature"}`; got.raw != want {
		t.Errorf("bad signature body %s, want the contract's %s", got.raw, want)
	}
	got = serve(h, signedRequest(t, "GET", weatherURI, testNow-1000, ""))
	if want := `{"error":"The board's clock is more than 2 minutes off.","reason":"clock_skew","serverTime":1796000000}`; got.raw != want {
		t.Errorf("clock skew body %s, want the contract's %s", got.raw, want)
	}
}

func TestAuthMissing(t *testing.T) {
	h, _ := realMux(t)
	for _, uri := range []string{weatherURI, "/v1/flights?lat=1&lon=1", "/v1/headlines?q=x", "/v1/logo/TP"} {
		wantRefused(t, uri, serve(h, httptest.NewRequest("GET", uri, nil)), reasonMissing, nil)
	}
	req := httptest.NewRequest("GET", weatherURI, nil)
	req.Header.Set("X-Token", "the-old-way")
	wantRefused(t, "only the old token header", serve(h, req), reasonMissing, nil)
}

func TestAuthMalformed(t *testing.T) {
	v := loadVectors(t)
	h, _ := realMux(t)
	good := signedRequest(t, "GET", weatherURI, testNow, "")
	sig := strings.TrimPrefix(good.Header.Get("X-Signature"), "v1=")
	if strings.ToUpper(sig) == sig {
		t.Fatal("the test signature must contain a letter, or the upper-case case below proves nothing")
	}
	type mutation struct {
		name   string
		mutate func(h http.Header)
	}
	var cases []mutation
	set := func(name, header, value string) {
		cases = append(cases, mutation{name, func(h http.Header) { h.Set(header, value) }})
	}
	del := func(name string, headers ...string) {
		cases = append(cases, mutation{name, func(h http.Header) {
			for _, k := range headers {
				h.Del(k)
			}
		}})
	}
	// some headers present, some not
	del("only the device", "X-Timestamp", "X-Signature")
	del("only the signature", "X-Device", "X-Timestamp")
	del("no signature", "X-Signature")
	del("no timestamp", "X-Timestamp")
	del("no device", "X-Device")
	cases = append(cases,
		mutation{"device sent twice", func(h http.Header) { h.Add("X-Device", testDevice) }},
		mutation{"timestamp sent twice", func(h http.Header) { h.Add("X-Timestamp", "1796000000") }},
		mutation{"signature sent twice", func(h http.Header) { h.Add("X-Signature", h.Get("X-Signature")) }},
	)
	for _, id := range v.BadDeviceIDs {
		set(fmt.Sprintf("device %q", id), "X-Device", id)
	}
	set("device with a trailing space inside", "X-Device", "aabbccddeef f")
	for _, ts := range []string{"", "abc", "-1", "+1796000000", "1796000000.5", "1796000000 1", "1.796e9", "0x6b3b9c00", "١٧٩٦٠٠٠٠٠٠", "1796000000000" /* 13 digits */, "17960000000000000000000"} {
		set(fmt.Sprintf("timestamp %q", ts), "X-Timestamp", ts)
	}
	for name, s := range map[string]string{
		"empty":                    "",
		"only the prefix":          "v1=",
		"63 digits":                "v1=" + sig[:63],
		"65 digits":                "v1=" + sig + "0",
		"not hex":                  "v1=" + strings.Repeat("g", 64),
		"upper case hex":           "v1=" + strings.ToUpper(sig),
		"version 2":                "v2=" + sig,
		"version 0":                "v0=" + sig,
		"upper case version":       "V1=" + sig,
		"no version":               sig,
		"colon instead of equals":  "v1:" + sig,
		"space after the equals":   "v1= " + sig[:63],
		"another scheme":           "Bearer " + sig,
		"hex in the version place": sig[:2] + "=" + sig,
	} {
		set("signature "+name, "X-Signature", s)
	}
	for _, c := range cases {
		req := signedRequest(t, "GET", weatherURI, testNow, "")
		c.mutate(req.Header)
		wantRefused(t, c.name, serve(h, req), reasonMalformed, nil)
	}

	// a malformed request never gets the server time, even when its timestamp is far off
	req := signedRequest(t, "GET", weatherURI, testNow-100000, "")
	req.Header.Set("X-Signature", "v2="+sig)
	wantRefused(t, "stale and malformed", serve(h, req), reasonMalformed, nil)

	// a real signature under a version this service does not know is malformed, not bad_signature
	req = signedRequest(t, "GET", weatherURI, testNow, "")
	req.Header.Set("X-Signature", "v2="+strings.TrimPrefix(req.Header.Get("X-Signature"), "v1="))
	wantRefused(t, "wrong version", serve(h, req), reasonMalformed, nil)
}

func TestAuthBadSignature(t *testing.T) {
	h, a := realMux(t)
	good := signedRequest(t, "GET", weatherURI, testNow, "")
	sig := good.Header.Get("X-Signature")
	for _, i := range []int{len(sig) - 1, len("v1=")} { // the last and the first digit
		req := signedRequest(t, "GET", weatherURI, testNow, "")
		req.Header.Set("X-Signature", flipHexDigit(sig, i))
		wantRefused(t, fmt.Sprintf("digit %d flipped", i), serve(h, req), reasonBadSignature, nil)
	}
	other := func(name string, mutate func(req *http.Request) *http.Request) {
		t.Helper()
		wantRefused(t, name, serve(h, mutate(signedRequest(t, "GET", weatherURI, testNow, ""))), reasonBadSignature, nil)
	}
	other("another device ID claimed", func(r *http.Request) *http.Request { r.Header.Set("X-Device", "112233445566"); return r })
	other("another host", func(r *http.Request) *http.Request { r.Host = "other.example.com"; return r })
	other("another timestamp, inside the window", func(r *http.Request) *http.Request {
		r.Header.Set("X-Timestamp", strconv.FormatInt(testNow+1, 10))
		return r
	})
	other("the timestamp with a leading zero", func(r *http.Request) *http.Request {
		r.Header.Set("X-Timestamp", "0"+strconv.FormatInt(testNow, 10))
		return r
	})
	other("another path", func(r *http.Request) *http.Request {
		r2 := httptest.NewRequest("GET", "/v1/weather?lat=38.8&lon=-9.1", nil)
		r2.Host, r2.Header = r.Host, r.Header
		return r2
	})
	other("another method", func(r *http.Request) *http.Request {
		r2 := httptest.NewRequest("HEAD", weatherURI, nil)
		r2.Host, r2.Header = r.Host, r.Header
		return r2
	})
	wantRefused(t, "signed with another master secret",
		serve(h, signWith("a-different-master-secret-0123456789", testDevice, testHost, "GET", weatherURI, testNow, "")), reasonBadSignature, nil)
	wantRefused(t, "signed with the master secret itself",
		serve(h, func() *http.Request {
			req := signedRequest(t, "GET", weatherURI, testNow, "")
			sum := sha256.Sum256(nil)
			canon := canonicalString("GET", testHost, weatherURI, strconv.FormatInt(testNow, 10), hex.EncodeToString(sum[:]))
			req.Header.Set("X-Signature", "v1="+hex.EncodeToString(hmacSHA256([]byte(testMaster), []byte(canon))))
			return req
		}()), reasonBadSignature, nil)

	// nothing is revealed to a caller who cannot sign: no server time for a stale one, no revoked status
	wantRefused(t, "stale and wrong key",
		serve(h, signWith("a-different-master-secret-0123456789", testDevice, testHost, "GET", weatherURI, testNow-5000, "")), reasonBadSignature, nil)
	a.auth.revoked = map[string]bool{testDevice: true}
	wantRefused(t, "revoked device, wrong key",
		serve(h, signWith("a-different-master-secret-0123456789", testDevice, testHost, "GET", weatherURI, testNow, "")), reasonBadSignature, nil)
}

func TestAuthClockWindow(t *testing.T) {
	h, _ := realMux(t)
	for _, d := range []int64{0, 1, -1, 119, -119, 120, -120} {
		wantAccepted(t, fmt.Sprintf("%+d s", d), serve(h, signedRequest(t, "GET", weatherURI, testNow+d, "")))
	}
	for _, d := range []int64{121, -121, 3600, -3600, -testNow} { // the last is timestamp 0
		wantRefused(t, fmt.Sprintf("%+d s", d), serve(h, signedRequest(t, "GET", weatherURI, testNow+d, "")), reasonClockSkew, i64(testNow))
	}
	// a board that never set its clock, and a timestamp of the longest legal length
	wantRefused(t, "far future", serve(h, signedRequest(t, "GET", weatherURI, 999999999999, "")), reasonClockSkew, i64(testNow))

	// the answer carries the service's clock
	_, a := realMux(t)
	a.auth.now = fixedClock(1796000123)
	got := serve(a.routes(), signedRequest(t, "GET", weatherURI, 1796000123-121, ""))
	wantRefused(t, "server time", got, reasonClockSkew, i64(1796000123))
}

func TestAuthRevoked(t *testing.T) {
	h, a := realMux(t)
	a.auth.revoked = map[string]bool{testDevice: true}
	wantRefused(t, "revoked", serve(h, signedRequest(t, "GET", weatherURI, testNow, "")), reasonRevoked, nil)
	// revoked is checked before the clock: a revoked device gets no server time either
	wantRefused(t, "revoked and stale", serve(h, signedRequest(t, "GET", weatherURI, testNow-1000, "")), reasonRevoked, nil)
	// another device is not affected
	wantAccepted(t, "another device", serve(h, signWith(testMaster, "112233445566", testHost, "GET", weatherURI, testNow, "")))
	// and the owner can lift the revocation: nothing is banned for good
	a.auth.revoked = nil
	wantAccepted(t, "after lifting the revocation", serve(h, signedRequest(t, "GET", weatherURI, testNow, "")))
}

func TestAuthPublicHost(t *testing.T) {
	h, a := realMux(t)
	viaProxy := func(signedFor string) *http.Request {
		req := signWith(testMaster, testDevice, signedFor, "GET", weatherURI, testNow, "")
		req.Host = "ledmatrix-api.default.svc.cluster.local" // what a proxy might rewrite the Host header to
		return req
	}
	wantRefused(t, "rewritten Host, no PUBLIC_HOST", serve(h, viaProxy(testHost)), reasonBadSignature, nil)

	au, err := authFromEnv(envMap{"MASTER_SECRET": testMaster, "PUBLIC_HOST": "  LedMatrix.Example.COM:443\n"}.get, silent)
	if err != nil {
		t.Fatal(err)
	}
	au.now = fixedClock(testNow)
	a.auth = au
	h = a.routes()
	wantAccepted(t, "rewritten Host, PUBLIC_HOST set", serve(h, viaProxy(testHost)))
	wantRefused(t, "signed for the rewritten name", serve(h, viaProxy("ledmatrix-api.default.svc.cluster.local")), reasonBadSignature, nil)
	wantAccepted(t, "Host as sent, PUBLIC_HOST set", serve(h, signedRequest(t, "GET", weatherURI, testNow, "")))
}

func TestAuthBodyIsPartOfTheSignature(t *testing.T) {
	au := testAuthenticator(t)
	h := echoMux(au)
	body := `{"a":1}`
	got := serve(h, signedRequest(t, "POST", "/v1/example", testNow, body))
	if got.code != 200 || got.raw != body {
		t.Fatalf("a signed body must be accepted and still readable by the handler: %d %q", got.code, got.raw)
	}
	// signed for an empty body, sent with one; signed for a body, sent without
	req := signedRequest(t, "POST", "/v1/example", testNow, "")
	req.Body, req.ContentLength = io.NopCloser(strings.NewReader(body)), int64(len(body))
	wantRefused(t, "body added", serve(h, req), reasonBadSignature, nil)
	req = signedRequest(t, "POST", "/v1/example", testNow, body)
	req.Body, req.ContentLength = http.NoBody, 0
	wantRefused(t, "body removed", serve(h, req), reasonBadSignature, nil)
	wantRefused(t, "one byte changed", serve(h, func() *http.Request {
		r := signedRequest(t, "POST", "/v1/example", testNow, body)
		r.Body = io.NopCloser(strings.NewReader(`{"a":2}`))
		return r
	}()), reasonBadSignature, nil)

	// at most 4 KB are read before the signature is checked
	if maxSignedBody != 4096 {
		t.Fatalf("the contract says 4 KB, the code says %d", maxSignedBody)
	}
	if got := serve(h, signedRequest(t, "POST", "/v1/example", testNow, strings.Repeat("x", 4096))); got.code != 200 || len(got.raw) != 4096 {
		t.Errorf("exactly 4096 bytes must be accepted: %d, %d bytes", got.code, len(got.raw))
	}
	got = serve(h, signedRequest(t, "POST", "/v1/example", testNow, strings.Repeat("x", 4097)))
	if got.code != http.StatusRequestEntityTooLarge || strings.Contains(got.raw, "reason") || got.errText == "" {
		t.Errorf("4097 bytes must be refused with 413 and a plain error: %d %s", got.code, got.raw)
	}
}

// watchedBody is a request body that records whether anybody tried to read it.
type watchedBody struct{ reads int }

func (b *watchedBody) Read([]byte) (int, error) { b.reads++; return 0, io.EOF }
func (b *watchedBody) Close() error             { return nil }

// A GET or HEAD that announces a body is malformed and its body is never read: the board sends
// only GET, and the body would be buffered before anybody has proved who they are.
func TestAuthGetWithBodyIsMalformedAndUnread(t *testing.T) {
	au := testAuthenticator(t)
	var logged logCapture
	au.logf = logged.logf
	h := echoMux(au)
	cases := []struct {
		name   string
		method string
		mutate func(r *http.Request)
	}{
		{"GET, Content-Length 65536 and nothing sent", "GET", func(r *http.Request) { r.ContentLength = 65536 }},
		{"GET, Content-Length 1", "GET", func(r *http.Request) { r.ContentLength = 1 }},
		{"GET, chunked", "GET", func(r *http.Request) { r.ContentLength, r.TransferEncoding = -1, []string{"chunked"} }},
		{"GET, unknown length", "GET", func(r *http.Request) { r.ContentLength = -1 }},
		{"GET, some other Transfer-Encoding", "GET", func(r *http.Request) { r.TransferEncoding = []string{"gzip", "chunked"} }},
		{"HEAD, Content-Length 65536", "HEAD", func(r *http.Request) { r.ContentLength = 65536 }},
		{"HEAD, chunked", "HEAD", func(r *http.Request) { r.ContentLength, r.TransferEncoding = -1, []string{"chunked"} }},
	}
	for _, c := range cases {
		// signed correctly for the empty body, so only the announced body can be the reason
		req := signedRequest(t, c.method, weatherURI, testNow, "")
		body := &watchedBody{}
		req.Body = body
		c.mutate(req)
		got := serve(h, req)
		wantRefused(t, c.name, got, reasonMalformed, nil)
		if body.reads != 0 {
			t.Errorf("%s: the body was read %d times, it must never be", c.name, body.reads)
		}
	}
	if lines := logged.all(); len(lines) != 1 || lines[0] != "auth refused: device=aabbccddeeff reason=malformed" {
		t.Errorf("the device ID is logged as usual: %q", lines)
	}
	// a POST is still allowed to carry a body, and a GET without one is still accepted
	if got := serve(h, signedRequest(t, "POST", "/v1/example", testNow, "abc")); got.code != 200 || got.raw != "abc" {
		t.Errorf("POST with a body: %d %q", got.code, got.raw)
	}
	if got := serve(h, signedRequest(t, "GET", weatherURI, testNow, "")); got.code != 200 {
		t.Errorf("GET without a body: %d", got.code)
	}
}

// Over a real connection: a GET that promises 64 KB and sends none is answered at once, and the
// connection is closed rather than kept waiting for the body (Go would otherwise wait for it).
func TestAuthGetWithBodyOverTheWire(t *testing.T) {
	h, _ := realMux(t)
	srv := httptest.NewServer(h)
	defer srv.Close()
	signed := signedRequest(t, "GET", weatherURI, testNow, "")
	for name, framing := range map[string]string{
		"Content-Length 65536": "Content-Length: 65536\r\n",
		"chunked":              "Transfer-Encoding: chunked\r\n",
	} {
		conn, err := net.Dial("tcp", srv.Listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nX-Device: %s\r\nX-Timestamp: %s\r\nX-Signature: %s\r\n%s\r\n",
			weatherURI, testHost, signed.Header.Get("X-Device"), signed.Header.Get("X-Timestamp"), signed.Header.Get("X-Signature"), framing)
		resp, err := http.ReadResponse(bufio.NewReader(conn), nil)
		if err != nil {
			t.Errorf("%s: no prompt answer: %v", name, err)
			conn.Close()
			continue
		}
		b, _ := io.ReadAll(resp.Body)
		conn.Close()
		if resp.StatusCode != 401 || !strings.Contains(string(b), `"reason":"malformed"`) || !resp.Close {
			t.Errorf("%s: %d close=%v %s", name, resp.StatusCode, resp.Close, b)
		}
	}
}

func TestAuthHealthzIsOpen(t *testing.T) {
	h, a := realMux(t)
	a.auth.revoked = map[string]bool{testDevice: true}
	for name, req := range map[string]*http.Request{
		"no headers": httptest.NewRequest("GET", "/healthz", nil),
		"garbage headers": func() *http.Request {
			r := httptest.NewRequest("GET", "/healthz", nil)
			r.Header.Set("X-Device", "nonsense")
			r.Header.Set("X-Timestamp", "yesterday")
			r.Header.Set("X-Signature", "v9=zz")
			return r
		}(),
		"a revoked device with a stale timestamp": signedRequest(t, "GET", "/healthz", 1, ""),
	} {
		if got := serve(h, req); got.code != 200 || got.raw != "ok" {
			t.Errorf("%s: %d %q", name, got.code, got.raw)
		}
	}
}

func TestAuthOff(t *testing.T) {
	var logged logCapture
	au, err := authFromEnv(envMap{"AUTH_OFF": "1"}.get, logged.logf)
	if err != nil {
		t.Fatal(err)
	}
	if lines := logged.all(); len(lines) != 1 || !strings.Contains(lines[0], "WARNING") || !strings.Contains(lines[0], "AUTH_OFF=1") {
		t.Fatalf("starting with AUTH_OFF=1 must log one loud warning: %q", lines)
	}
	h, a := realMux(t)
	a.auth = au
	h = a.routes()
	if got := serve(h, httptest.NewRequest("GET", weatherURI, nil)); got.code != 200 {
		t.Errorf("AUTH_OFF=1, no headers: %d %s", got.code, got.raw)
	}
	req := httptest.NewRequest("GET", weatherURI, nil)
	req.Header.Set("X-Signature", "garbage")
	if got := serve(h, req); got.code != 200 {
		t.Errorf("AUTH_OFF=1, garbage headers: %d %s", got.code, got.raw)
	}
}

func TestAuthNotConfiguredRefusesEverythingButHealthz(t *testing.T) {
	a := newStub(t).app(t, lisbonNoon(t))
	a.auth = nil
	h := a.routes()
	if got := serve(h, signedRequest(t, "GET", weatherURI, testNow, "")); got.code != http.StatusServiceUnavailable {
		t.Errorf("a service without an authenticator must refuse: %d", got.code)
	}
	if got := serve(h, httptest.NewRequest("GET", "/healthz", nil)); got.code != 200 {
		t.Errorf("healthz: %d", got.code)
	}
}

// ---- configuration ---------------------------------------------------------------------------------------

func TestAuthConfig(t *testing.T) {
	secret32 := strings.Repeat("s", 32)
	cases := []struct {
		name    string
		env     envMap
		wantErr string // a fragment of the message; empty means it must start
	}{
		{"a 32 character secret", envMap{"MASTER_SECRET": secret32}, ""},
		{"a 31 character secret", envMap{"MASTER_SECRET": secret32[:31]}, "shorter than 32"},
		{"31 characters and whitespace around", envMap{"MASTER_SECRET": " \t" + secret32[:31] + "\r\n"}, "shorter than 32"},
		{"32 characters and whitespace around", envMap{"MASTER_SECRET": "\n " + secret32 + " \t\r\n"}, ""},
		{"32 characters of which one is whitespace inside", envMap{"MASTER_SECRET": secret32[:15] + " " + secret32[:16]}, ""},
		{"only whitespace", envMap{"MASTER_SECRET": " \r\n"}, "shorter than 32"},
		{"32 two-byte characters", envMap{"MASTER_SECRET": strings.Repeat("é", 32)}, ""},
		{"31 two-byte characters", envMap{"MASTER_SECRET": strings.Repeat("é", 31)}, "shorter than 32"},
		{"neither a secret nor AUTH_OFF", envMap{}, "MASTER_SECRET is not set"},
		{"AUTH_OFF=0 and no secret", envMap{"AUTH_OFF": "0"}, "MASTER_SECRET is not set"},
		{"both AUTH_OFF=1 and a secret", envMap{"AUTH_OFF": "1", "MASTER_SECRET": secret32}, "both AUTH_OFF=1 and MASTER_SECRET"},
		{"AUTH_OFF=1 and a short secret", envMap{"AUTH_OFF": "1", "MASTER_SECRET": "short"}, "both AUTH_OFF=1 and MASTER_SECRET"},
		{"AUTH_OFF=1 and a blank secret", envMap{"AUTH_OFF": "1", "MASTER_SECRET": "  "}, "both AUTH_OFF=1 and MASTER_SECRET"},
		{"AUTH_OFF=1 alone", envMap{"AUTH_OFF": "1"}, ""},
		{"AUTH_OFF=0 with a secret", envMap{"AUTH_OFF": "0", "MASTER_SECRET": secret32}, ""},
		{"AUTH_OFF=yes", envMap{"AUTH_OFF": "yes", "MASTER_SECRET": secret32}, "AUTH_OFF must be 1"},
		{"a revoked list", envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": " aabbccddeeff , 112233445566,\n"}, ""},
		{"a revoked list with an empty entry", envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": "aabbccddeeff,,112233445566,"}, ""},
		{"upper case revoked ID", envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": "aabbccddeeff,AABBCCDDEEFF"}, "REVOKED_DEVICES entry 2 is not a device ID"},
		{"revoked ID with colons", envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": "aa:bb:cc:dd:ee:ff"}, "REVOKED_DEVICES entry 1"},
		{"revoked ID too short", envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": "aabbccddeef"}, "REVOKED_DEVICES entry 1"},
		{"revoked IDs split by a semicolon", envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": "aabbccddeeff;112233445566"}, "REVOKED_DEVICES entry 1"},
		{"bad revoked list with AUTH_OFF=1", envMap{"AUTH_OFF": "1", "REVOKED_DEVICES": "nope"}, "REVOKED_DEVICES entry 1"},
		{"a public host", envMap{"MASTER_SECRET": secret32, "PUBLIC_HOST": "ledmatrix.example.com"}, ""},
		{"a public host with a scheme", envMap{"MASTER_SECRET": secret32, "PUBLIC_HOST": "https://ledmatrix.example.com"}, "PUBLIC_HOST must be a plain host name"},
		{"a public host with a path", envMap{"MASTER_SECRET": secret32, "PUBLIC_HOST": "ledmatrix.example.com/x"}, "PUBLIC_HOST must be a plain host name"},
	}
	for _, c := range cases {
		var logged logCapture
		au, err := authFromEnv(c.env.get, logged.logf)
		if c.wantErr == "" {
			if err != nil {
				t.Errorf("%s: must start, got %v", c.name, err)
			}
			if warned := len(logged.all()) > 0; warned != (c.env["AUTH_OFF"] == "1") {
				t.Errorf("%s: the warning must be logged exactly when AUTH_OFF=1, got %q", c.name, logged.all())
			}
			continue
		}
		if err == nil {
			t.Errorf("%s: must refuse to start", c.name)
			continue
		}
		if au != nil || !strings.Contains(err.Error(), c.wantErr) {
			t.Errorf("%s: error %q, want it to say %q", c.name, err, c.wantErr)
		}
		// the message names the problem, never the value
		for _, v := range []string{c.env["MASTER_SECRET"], c.env["REVOKED_DEVICES"]} {
			if len(trimWS(v)) >= 5 && strings.Contains(err.Error(), trimWS(v)) {
				t.Errorf("%s: the error repeats a configured value: %q", c.name, err)
			}
		}
	}

	au, err := authFromEnv(envMap{"MASTER_SECRET": secret32, "REVOKED_DEVICES": "aabbccddeeff, 112233445566", "PUBLIC_HOST": " LedMatrix.Example.COM:8443 "}.get, silent)
	if err != nil {
		t.Fatal(err)
	}
	if len(au.revoked) != 2 || !au.revoked["aabbccddeeff"] || !au.revoked["112233445566"] || au.publicHost != "ledmatrix.example.com" || au.off {
		t.Errorf("parsed configuration: revoked %v, public host %q, off %v", au.revoked, au.publicHost, au.off)
	}
}

func TestNormalizeHost(t *testing.T) {
	for in, want := range map[string]string{
		"ledmatrix.example.com":      "ledmatrix.example.com",
		"LedMatrix.Example.COM":      "ledmatrix.example.com",
		"ledmatrix.example.com:8443": "ledmatrix.example.com",
		"ledmatrix.example.com:":     "ledmatrix.example.com",
		"[::1]:8080":                 "[::1]",
		"[::1]":                      "[::1]",
		"":                           "",
	} {
		if got := normalizeHost(in); got != want {
			t.Errorf("normalizeHost(%q) = %q, want %q", in, got, want)
		}
	}
}

// ---- logging ---------------------------------------------------------------------------------------------

type logCapture struct {
	mu    sync.Mutex
	lines []string
}

func (c *logCapture) logf(format string, args ...any) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.lines = append(c.lines, fmt.Sprintf(format, args...))
}

func (c *logCapture) all() []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return append([]string(nil), c.lines...)
}

var refusalLine = regexp.MustCompile(`^auth refused: device=([0-9a-f]{12}|-) reason=(missing|malformed|bad_signature|revoked|clock_skew|body_too_large)$`)

func TestRefusalLogShowsOnlyDeviceAndReason(t *testing.T) {
	v := loadVectors(t)
	h, a := realMux(t)
	var logged logCapture
	a.auth.logf = logged.logf
	a.auth.revoked = map[string]bool{"112233445566": true}

	trouble := "attacker-header-value"
	// missing
	missing := httptest.NewRequest("GET", weatherURI, nil)
	missing.Host = trouble + ".example.com"
	serve(h, missing)
	// malformed, with a device ID that is not usable and header values that must never be echoed
	bad := signedRequest(t, "GET", weatherURI, testNow, "")
	bad.Header.Set("X-Device", "AABBCCDDEEFF-"+trouble)
	bad.Header.Set("X-Timestamp", "garbage-"+trouble)
	serve(h, bad)
	// bad signature, from the vector's own bytes with one digit changed
	r := v.Requests[0]
	req := httptest.NewRequest(r.Method, r.URI, nil)
	req.Host = r.Host
	req.Header.Set("X-Device", v.DeviceID)
	req.Header.Set("X-Timestamp", strconv.FormatInt(r.Timestamp, 10))
	req.Header.Set("X-Signature", "v1="+flipHexDigit(r.Signature, 0))
	a.auth.now = fixedClock(r.Timestamp)
	serve(h, req)
	// revoked and clock_skew
	serve(h, signWith(testMaster, "112233445566", testHost, "GET", weatherURI, r.Timestamp, ""))
	serve(h, signedRequest(t, "GET", weatherURI, r.Timestamp-500, ""))
	// accepted: no line
	serve(h, signedRequest(t, "GET", weatherURI, r.Timestamp, ""))

	want := []string{
		"auth refused: device=- reason=missing",
		"auth refused: device=- reason=malformed",
		"auth refused: device=aabbccddeeff reason=bad_signature",
		"auth refused: device=112233445566 reason=revoked",
		"auth refused: device=aabbccddeeff reason=clock_skew",
	}
	lines := logged.all()
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("log lines:\n%s\nwant:\n%s", strings.Join(lines, "\n"), strings.Join(want, "\n"))
	}
	text := strings.Join(lines, "\n")
	secrets := []string{testMaster, v.DeviceKeyHex, v.Requests[0].Signature, flipHexDigit(r.Signature, 0), trouble, "AABBCC", "v1=", "v1\nGET", "example.com", r.Host, weatherURI}
	for _, s := range secrets {
		if strings.Contains(text, s) {
			t.Errorf("the log contains %q", s)
		}
	}
	for _, l := range lines {
		if !refusalLine.MatchString(l) {
			t.Errorf("unexpected log line %q", l)
		}
	}
}

// refusing flood: n requests, each claiming another well-formed device ID with a wrong signature.
func flood(h http.Handler, from, n int) {
	for i := from; i < from+n; i++ {
		req := signWith(testMaster, testDevice, testHost, "GET", weatherURI, testNow, "")
		req.Header.Set("X-Device", fmt.Sprintf("%012x", i+1))
		serve(h, req)
	}
}

func TestRefusalLogIsBounded(t *testing.T) {
	au := testAuthenticator(t)
	var logged logCapture
	au.logf = logged.logf
	h := echoMux(au)

	// the same device and reason: once a minute
	for i := 0; i < 50; i++ {
		serve(h, signWith("a-different-master-secret-0123456789", testDevice, testHost, "GET", weatherURI, testNow, ""))
	}
	if n := len(logged.all()); n != 1 {
		t.Fatalf("50 identical refusals logged %d lines, want 1", n)
	}
	// another reason for the same device is its own line
	serve(h, signWith(testMaster, testDevice, testHost, "GET", weatherURI, testNow-500, ""))
	if n := len(logged.all()); n != 2 {
		t.Fatalf("a new (device, reason) must be logged: %d lines", n)
	}
	// after the window the same pair is logged again
	au.now = fixedClock(testNow + 61)
	serve(h, signWith("a-different-master-secret-0123456789", testDevice, testHost, "GET", weatherURI, testNow+61, ""))
	if n := len(logged.all()); n != 3 {
		t.Fatalf("after a minute the same pair is logged again: %d lines", n)
	}

	// a scanner rotating device IDs: at most refusalBudget lines per minute, then silence, then a summary
	var flooded logCapture
	au.logf = flooded.logf
	au.refusals = refusalLog{}
	au.now = fixedClock(testNow)
	flood(h, 0, 200)
	if n := len(flooded.all()); n != refusalBudget {
		t.Fatalf("200 distinct refusals logged %d lines, want the budget of %d", n, refusalBudget)
	}
	au.now = fixedClock(testNow + 61)
	flood(h, 1000, 1)
	lines := flooded.all()
	if len(lines) != refusalBudget+2 || lines[refusalBudget] != fmt.Sprintf("auth refused: %d more refusals in the last minute were not logged", 200-refusalBudget) {
		t.Fatalf("the next window must start with a summary of what was dropped: %q", lines[refusalBudget:])
	}
	// the memory stays bounded across many windows
	for w := 2; w < 40; w++ {
		au.now = fixedClock(testNow + int64(w)*61)
		flood(h, w*1000, 300)
	}
	au.refusals.mu.Lock()
	n := len(au.refusals.seen)
	au.refusals.mu.Unlock()
	if n > refusalCap {
		t.Fatalf("the refusal map holds %d entries, the cap is %d", n, refusalCap)
	}
}

func TestConcurrentRequests(t *testing.T) {
	au := testAuthenticator(t)
	var logged logCapture
	au.logf = logged.logf
	h := echoMux(au)
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			for i := 0; i < 100; i++ {
				if got := serve(h, signedRequest(t, "GET", weatherURI, testNow, "")); got.code != 200 {
					t.Errorf("accepted request refused: %d", got.code)
				}
				req := signedRequest(t, "GET", weatherURI, testNow, "")
				req.Header.Set("X-Device", fmt.Sprintf("%012x", g*1000+i))
				serve(h, req)
			}
		}(g)
	}
	wg.Wait()
	if n := len(logged.all()); n > refusalBudget {
		t.Errorf("%d refusal lines, budget %d", n, refusalBudget)
	}
}
