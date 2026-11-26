package main

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// The signed manifest, service side (shared/manifest-signing.md section 6). The service checks the SHAPE of keyid and sig and
// passes them on; it never verifies a signature and holds no key. The shapes come from the shared known-answer vectors
// (shared/manifest-signing-vectors.json): the keys there are throwaway test keys.

type signingVector struct {
	Name     string          `json:"name"`
	Expect   string          `json:"expect"`
	Manifest json.RawMessage `json:"manifest"`
}

func loadSigningVectors(t *testing.T) []signingVector {
	t.Helper()
	b, err := os.ReadFile("../shared/manifest-signing-vectors.json")
	if err != nil {
		t.Fatalf("the shared vectors must be readable (run the tests from a checkout of the whole repository): %v", err)
	}
	var file struct {
		Vectors []signingVector `json:"vectors"`
	}
	if err := json.Unmarshal(b, &file); err != nil || len(file.Vectors) < 30 {
		t.Fatalf("the vectors file: %v, %d vectors", err, len(file.Vectors))
	}
	return file.Vectors
}

func vectorByName(t *testing.T, vs []signingVector, name string) map[string]json.RawMessage {
	t.Helper()
	for _, v := range vs {
		if v.Name == name {
			var m map[string]json.RawMessage
			if err := json.Unmarshal(v.Manifest, &m); err != nil {
				t.Fatal(err)
			}
			return m
		}
	}
	t.Fatalf("no vector named %s", name)
	return nil
}

func rawString(t *testing.T, raw json.RawMessage) string {
	t.Helper()
	var s string
	if err := json.Unmarshal(raw, &s); err != nil {
		t.Fatal(err)
	}
	return s
}

// signedManifestText is a manifest.json for `image` under `version` with `extra` (for example `,"keyid":"..."`) before the closing brace.
func signedManifestText(image []byte, version, extra string) string {
	return fmt.Sprintf(`{"version":"%s","file":"ledmatrix-%s.bin","size":%d,"sha256":"%x","notes":"n"%s}`, version, version, len(image), sha256.Sum256(image), extra)
}

// publishText puts the image and the given manifest text into a new folder and returns a rig on it.
func publishText(t *testing.T, image []byte, version, text string) *fwRig {
	t.Helper()
	dir := t.TempDir()
	putFile(t, filepath.Join(dir, "ledmatrix-"+version+".bin"), image)
	putFile(t, filepath.Join(dir, "manifest.json"), []byte(text))
	return newRig(t, dir)
}

const (
	goodKeyID = "f864ea3b"
	goodSig   = "d29adc49a905a9f46197a1d1b988d04bf9829a65293f9565f9c19a103ddf7c0e39e6f8d57baccf1993a129a2acf231603e97bf90970d88c44baa638bb342c6c4"
)

// A signed manifest is answered with the two fields after notes, unchanged, and the log line names the key id and never the signature.
func TestFirmwareSignedManifestPassesThrough(t *testing.T) {
	vs := loadSigningVectors(t)
	ok := vectorByName(t, vs, "ok-basic")
	keyid, sig := rawString(t, ok["keyid"]), rawString(t, ok["sig"])
	if keyid != goodKeyID || sig != goodSig {
		t.Fatal("the constants of this test must equal the vector ok-basic")
	}
	dir := t.TempDir()
	image := testImage(150_000, 30)
	m := writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) { m.KeyID, m.Sig = keyid, sig })
	r := newRig(t, dir)

	want := fmt.Sprintf(`{"version":"0.10.1","file":"ledmatrix-0.10.1.bin","size":150000,"sha256":"%s","notes":"A short note.","keyid":"%s","sig":"%s"}`, m.SHA256, keyid, sig)
	rec := r.get("GET", "/v1/firmware/manifest")
	if rec.Code != 200 || rec.Body.String() != want {
		t.Fatalf("manifest %d %s\nwant %s", rec.Code, rec.Body.String(), want)
	}
	if rec.Header().Get("Cache-Control") != "no-cache" || rec.Header().Get("Content-Type") != "application/json" {
		t.Errorf("manifest headers: %v", rec.Header())
	}
	r.wantPublished(t, "signed", m, image)
	lines := r.logs.all()
	if len(lines) != 1 || lines[0] != "firmware: version 0.10.1 is published (150000 bytes), signed with key "+goodKeyID {
		t.Fatalf("the log line: %q", lines)
	}
	if strings.Contains(lines[0], sig) || strings.Contains(lines[0], sig[:16]) {
		t.Error("the log line must never carry the signature")
	}
}

// A manifest without the two fields is version 1: answered exactly as before, with no key, no null.
func TestFirmwareUnsignedManifestIsAnsweredAsBefore(t *testing.T) {
	image := testImage(150_000, 31)
	for name, extra := range map[string]string{
		"no fields":     "",
		"both null":     `,"keyid":null,"sig":null`,
		"both empty":    `,"keyid":"","sig":""`,
		"null and text": `,"keyid":"","sig":null`,
	} {
		r := publishText(t, image, fwVersion, signedManifestText(image, fwVersion, extra))
		rec := r.get("GET", "/v1/firmware/manifest")
		want := fmt.Sprintf(`{"version":"0.10.1","file":"ledmatrix-0.10.1.bin","size":150000,"sha256":"%x","notes":"n"}`, sha256.Sum256(image))
		if rec.Code != 200 || rec.Body.String() != want {
			t.Errorf("%s: %d %s\nwant %s", name, rec.Code, rec.Body.String(), want)
		}
		for _, bad := range []string{`"keyid"`, `"sig"`, "null"} {
			if strings.Contains(rec.Body.String(), bad) {
				t.Errorf("%s: the v1 answer holds %q", name, bad)
			}
		}
		if lines := r.logs.all(); len(lines) != 1 || lines[0] != "firmware: version 0.10.1 is published (150000 bytes), not signed" {
			t.Errorf("%s: the log line: %q", name, lines)
		}
	}
}

// Every shape the contract calls malformed makes "no firmware is published", logged once, without repeating the values.
func TestFirmwareSignatureShapesAreRefused(t *testing.T) {
	image := testImage(150_000, 32)
	hex8, hex128 := goodKeyID, goodSig
	type tc struct{ name, extra string }
	cases := []tc{
		{"only keyid", fmt.Sprintf(`,"keyid":"%s"`, hex8)},
		{"only sig", fmt.Sprintf(`,"sig":"%s"`, hex128)},
		{"keyid null, sig set", fmt.Sprintf(`,"keyid":null,"sig":"%s"`, hex128)},
		{"keyid set, sig null", fmt.Sprintf(`,"keyid":"%s","sig":null`, hex8)},
		{"keyid empty, sig set", fmt.Sprintf(`,"keyid":"","sig":"%s"`, hex128)},
		{"keyid set, sig empty", fmt.Sprintf(`,"keyid":"%s","sig":""`, hex8)},
		{"keyid of 7 characters", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8[:7], hex128)},
		{"keyid of 9 characters", fmt.Sprintf(`,"keyid":"%s0","sig":"%s"`, hex8, hex128)},
		{"keyid in upper case", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, strings.ToUpper(hex8), hex128)},
		{"keyid not hex", fmt.Sprintf(`,"keyid":"zzzzzzzz","sig":"%s"`, hex128)},
		{"keyid with a space", fmt.Sprintf(`,"keyid":"%s ","sig":"%s"`, hex8[:7], hex128)},
		{"keyid of eight non-ASCII digits", fmt.Sprintf(`,"keyid":"١٢٣٤٥٦٧٨","sig":"%s"`, hex128)},
		{"keyid as a number", fmt.Sprintf(`,"keyid":12345678,"sig":"%s"`, hex128)},
		{"keyid as true", fmt.Sprintf(`,"keyid":true,"sig":"%s"`, hex128)},
		{"keyid as an array", fmt.Sprintf(`,"keyid":["%s"],"sig":"%s"`, hex8, hex128)},
		{"sig of 127 characters", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8, hex128[:127])},
		{"sig of 129 characters", fmt.Sprintf(`,"keyid":"%s","sig":"%s0"`, hex8, hex128)},
		{"sig of 126 characters", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8, hex128[:126])},
		{"sig of 130 characters", fmt.Sprintf(`,"keyid":"%s","sig":"%s00"`, hex8, hex128)},
		{"sig in upper case", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8, strings.ToUpper(hex128))},
		{"sig in mixed case", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8, "A"+hex128[1:])},
		{"sig not hex", fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8, strings.Repeat("z", 128))},
		{"sig with a trailing newline", fmt.Sprintf(`,"keyid":"%s","sig":"%s\n"`, hex8, hex128[:127])},
		{"sig as a number", fmt.Sprintf(`,"keyid":"%s","sig":12345`, hex8)},
		{"sig as an object", fmt.Sprintf(`,"keyid":"%s","sig":{"r":"%s"}`, hex8, hex128[:64])},
		{"sig as base64", fmt.Sprintf(`,"keyid":"%s","sig":"MEUCIQDTzQ+4gE7kJq2X1b3k9sH0aL5oZ0YjPqVwR8nT6uXm1gIgcV3yB9dK2fHq7sLw0aZpNtEoRjXm4uYc8vDb5kQh1Wg="`, hex8)},
	}
	for _, c := range cases {
		r := publishText(t, image, fwVersion, signedManifestText(image, fwVersion, c.extra))
		r.wantNothing(t, c.name, fwFile)
		lines := r.logs.all()
		if len(lines) != 1 || !strings.HasPrefix(lines[0], "firmware: nothing is published, ") {
			t.Errorf("%s: want one line saying nothing is published, got %q", c.name, lines)
			continue
		}
		for _, secret := range []string{hex128[:20], hex8, "zzzzzzzz"} {
			if strings.Contains(lines[0], secret) {
				t.Errorf("%s: the log line %q repeats a value from the manifest", c.name, lines[0])
			}
		}
	}
	// the control: the same manifest with a well-formed pair is published
	r := publishText(t, image, fwVersion, signedManifestText(image, fwVersion, fmt.Sprintf(`,"keyid":"%s","sig":"%s"`, hex8, hex128)))
	if rec := r.get("GET", "/v1/firmware/manifest"); rec.Code != 200 {
		t.Fatalf("the control manifest must be accepted: %d %s", rec.Code, rec.Body.String())
	}
}

// The reasons the service gives are fixed sentences, one per rule.
func TestFirmwareSignatureRuleSentences(t *testing.T) {
	good := firmwareManifest{Version: "1.2.3", File: "ledmatrix-1.2.3.bin", Size: 150_000, SHA256: strings.Repeat("a", 64), Notes: "n"}
	if why := good.check(); why != "" {
		t.Fatalf("the base manifest must pass: %s", why)
	}
	signed := good
	signed.KeyID, signed.Sig = goodKeyID, goodSig
	if why := signed.check(); why != "" {
		t.Fatalf("the signed manifest must pass: %s", why)
	}
	for _, c := range []struct {
		name string
		edit func(m *firmwareManifest)
		want string
	}{
		{"only keyid", func(m *firmwareManifest) { m.Sig = "" }, "both present or both absent"},
		{"only sig", func(m *firmwareManifest) { m.KeyID = "" }, "both present or both absent"},
		{"short keyid", func(m *firmwareManifest) { m.KeyID = m.KeyID[:7] }, "keyid is not 8 lowercase hex"},
		{"upper keyid", func(m *firmwareManifest) { m.KeyID = strings.ToUpper(m.KeyID) }, "keyid is not 8 lowercase hex"},
		{"short sig", func(m *firmwareManifest) { m.Sig = m.Sig[:127] }, "sig is not 128 lowercase hex"},
		{"upper sig", func(m *firmwareManifest) { m.Sig = strings.ToUpper(m.Sig) }, "sig is not 128 lowercase hex"},
		{"newline in notes", func(m *firmwareManifest) { m.Notes = "a\nb" }, "control character"},
	} {
		m := signed
		c.edit(&m)
		if why := m.check(); !strings.Contains(why, c.want) {
			t.Errorf("%s: %q does not say %q", c.name, why, c.want)
		}
	}
}

// Notes hold no control character (the portal shows them and the signature covers them as text).
func TestFirmwareNotesWithControlCharactersAreRefused(t *testing.T) {
	image := testImage(150_000, 33)
	for name, notes := range map[string]string{
		"newline": "line one\nline two", "carriage return": "a\rb", "tab": "a\tb", "NUL": "a\x00b", "bell": "a\x07b",
		"unit separator": "a\x1fb", "DEL": "a\x7fb", "escape": "\x1b[31mred", "only a newline": "\n", "trailing newline": "done\n",
	} {
		dir := t.TempDir()
		writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) { m.Notes = notes })
		r := newRig(t, dir)
		r.wantNothing(t, name, fwFile)
		if lines := r.logs.all(); len(lines) != 1 || !strings.Contains(lines[0], "the notes hold a control character") {
			t.Errorf("%s: the log line: %q", name, lines)
		}
	}
	// these are not control characters in the contract's sense and are answered (the answer stays valid JSON that decodes back)
	for name, notes := range map[string]string{
		"a space": "a b", "accents": "Atualização: ecrã e fuso horário", "a C1 character": "a\u0085b", "U+2028": "a b",
		"angle brackets and an ampersand": "<b>&</b>", "an emoji": "🚀", "a check mark": "✓", "the quote and the backslash": `say "hi" \`,
	} {
		dir := t.TempDir()
		m := writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) { m.Notes = notes })
		r := newRig(t, dir)
		r.wantPublished(t, name, m, image)
	}
}

// The answer is at most 2,048 bytes, the limit of every board. The longest answer the other rules allow is far below it.
func TestFirmwareAnswerLimit(t *testing.T) {
	if firmwareMaxAnswer != 2048 {
		t.Fatalf("the answer limit is the contract's 2,048 bytes, not %d", firmwareMaxAnswer)
	}
	longest := firmwareManifest{Version: "9999.9999.9999", File: "ledmatrix-9999.9999.9999.bin", Size: firmwareMaxSize, SHA256: strings.Repeat("f", 64),
		Notes: strings.Repeat("<", firmwareMaxNotes), KeyID: goodKeyID, Sig: goodSig} // "<" is the most expensive character: it is escaped as 6 bytes
	if why := longest.check(); why != "" {
		t.Fatalf("the longest valid manifest must pass the rules: %s", why)
	}
	answer, why := buildAnswer(longest)
	if why != "" || len(answer) > firmwareMaxAnswer || len(answer) < 1500 {
		t.Fatalf("the longest valid answer is %d bytes (%q); it must fit in %d", len(answer), why, firmwareMaxAnswer)
	}

	// over the limit (which only a manifest that broke the notes rule could reach): refused, with a sentence, never truncated
	over := longest
	over.Notes = strings.Repeat("<", 400)
	if answer, why := buildAnswer(over); answer != nil || !strings.Contains(why, "2048") {
		t.Errorf("an answer of %d bytes must be refused: %q", len(answer), why)
	}
	// the boundary to the byte: plain notes grown until the answer is exactly at the limit, and one more byte
	grow := longest
	grow.Notes = ""
	base, _ := buildAnswer(grow)
	grow.Notes = strings.Repeat("a", firmwareMaxAnswer-len(base))
	if answer, why := buildAnswer(grow); why != "" || len(answer) != firmwareMaxAnswer {
		t.Errorf("an answer of exactly 2048 bytes is allowed: %d %q", len(answer), why)
	}
	grow.Notes += "a"
	if _, why := buildAnswer(grow); why == "" {
		t.Error("an answer of 2049 bytes must be refused")
	}

	// the worst case is also served through the real route
	image := testImage(150_000, 34)
	dir := t.TempDir()
	m := writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) {
		m.Notes, m.KeyID, m.Sig = strings.Repeat("<", firmwareMaxNotes), goodKeyID, goodSig
	})
	r := newRig(t, dir)
	rec := r.get("GET", "/v1/firmware/manifest")
	var got firmwareManifest
	if rec.Code != 200 || rec.Body.Len() > firmwareMaxAnswer || json.Unmarshal(rec.Body.Bytes(), &got) != nil || got != m {
		t.Fatalf("the worst case: %d, %d bytes", rec.Code, rec.Body.Len())
	}
}

// The shared vectors, through the real route: every shape the contract calls valid is answered with keyid and sig unchanged and in
// order, every other one is "no firmware is published". The service cannot tell a good signature from a bad one and does not try:
// ok, unknown_key and bad_signature are all well-formed here.
func TestFirmwareSigningVectors(t *testing.T) {
	vs := loadSigningVectors(t)
	image := testImage(150_000, 35)
	counts := map[string]int{}
	for _, v := range vs {
		var obj map[string]json.RawMessage
		if err := json.Unmarshal(v.Manifest, &obj); err != nil {
			t.Fatalf("%s: %v", v.Name, err)
		}
		// the vector's own fields, except that size and sha256 describe the image this test really has
		obj["size"] = json.RawMessage(fmt.Sprint(len(image)))
		obj["sha256"] = json.RawMessage(fmt.Sprintf(`"%x"`, sha256.Sum256(image)))
		text, err := json.Marshal(obj)
		if err != nil {
			t.Fatal(err)
		}
		var ver string
		if err := json.Unmarshal(obj["version"], &ver); err != nil {
			t.Fatalf("%s: %v", v.Name, err)
		}
		dir := t.TempDir()
		putFile(t, filepath.Join(dir, "ledmatrix-"+ver+".bin"), image)
		putFile(t, filepath.Join(dir, "manifest.json"), text)
		r := newRig(t, dir)
		rec := r.get("GET", "/v1/firmware/manifest")
		counts[v.Expect]++
		switch v.Expect {
		case "ok", "unknown_key", "bad_signature", "unsigned":
			if rec.Code != 200 {
				t.Errorf("%s (%s): the service must publish a well-formed manifest, got %d %s", v.Name, v.Expect, rec.Code, rec.Body.String())
				continue
			}
			var got map[string]json.RawMessage
			if err := json.Unmarshal(rec.Body.Bytes(), &got); err != nil {
				t.Errorf("%s: the answer is not JSON: %v", v.Name, err)
				continue
			}
			for _, k := range []string{"keyid", "sig"} {
				if want, has := obj[k]; has && string(want) != "null" {
					if string(got[k]) != string(want) {
						t.Errorf("%s: %s is %s, want %s unchanged", v.Name, k, got[k], want)
					}
				} else if _, has := got[k]; has {
					t.Errorf("%s: the answer has a %s the manifest did not", v.Name, k)
				}
			}
			if v.Expect == "unsigned" && (strings.Contains(rec.Body.String(), `"keyid":`) || strings.Contains(rec.Body.String(), `"sig":`)) {
				t.Errorf("%s: an unsigned manifest is answered without the fields: %s", v.Name, rec.Body.String())
			}
			order := []string{`"version"`, `"file"`, `"size"`, `"sha256"`, `"notes"`, `"keyid"`, `"sig"`}
			last := -1
			for _, k := range order {
				if i := strings.Index(rec.Body.String(), k); i >= 0 {
					if i < last {
						t.Errorf("%s: field %s is out of order in %s", v.Name, k, rec.Body.String())
					}
					last = i
				}
			}
		case "malformed", "manifest_invalid":
			r.wantNothing(t, v.Name+" ("+v.Expect+")", "ledmatrix-"+ver+".bin")
		default:
			t.Fatalf("%s: the vector expects %q, which this test does not know", v.Name, v.Expect)
		}
	}
	for _, kind := range []string{"ok", "unsigned", "malformed", "unknown_key", "bad_signature", "manifest_invalid"} {
		if counts[kind] == 0 {
			t.Errorf("the vectors hold no %s case", kind)
		}
	}
}

// The log says "changed" and not "requested": a re-signed release or a release that lost its signature is one new line each, and no line
// ever holds a signature.
func TestFirmwareLogsSigningChangesOnce(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 36)
	r := newRig(t, dir)
	sigA, sigB := goodSig, strings.Repeat("0", 127)+"1"
	publish := func(keyid, sig string) {
		writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) { m.KeyID, m.Sig = keyid, sig })
		for i := 0; i < 4; i++ {
			r.clk.advance(61 * time.Second)
			r.get("GET", "/v1/firmware/manifest")
		}
	}
	publish(goodKeyID, sigA)
	publish(goodKeyID, sigA) // nothing changed: no new line
	publish("db10c343", sigB)
	publish("", "")
	lines := r.logs.all()
	want := []string{
		"firmware: nothing is published, there is no manifest.json in the firmware folder",
		"firmware: version 0.10.1 is published (150000 bytes), signed with key f864ea3b",
		"firmware: version 0.10.1 is published (150000 bytes), signed with key db10c343",
		"firmware: version 0.10.1 is published (150000 bytes), not signed",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("log lines:\n%s\nwant:\n%s", strings.Join(lines, "\n"), strings.Join(want, "\n"))
	}
	for _, l := range lines {
		if strings.Contains(l, sigA[:16]) || strings.Contains(l, sigB[:16]) {
			t.Errorf("the log line %q holds a signature", l)
		}
	}
}

// The routes stay public for a signed release too: a revoked, stale or garbled device signature gets the same manifest.
func TestFirmwareSignedManifestIsPublic(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 37)
	m := writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) { m.KeyID, m.Sig = goodKeyID, goodSig })
	r := newRig(t, dir)
	r.a.auth.revoked = map[string]bool{testDevice: true}
	want := string(mustJSON(t, m))
	if !strings.Contains(want, `"keyid":"`+goodKeyID+`"`) {
		t.Fatalf("the expected answer must be the signed one: %s", want)
	}
	garbled := httptest.NewRequest("GET", "/v1/firmware/manifest", nil)
	garbled.Header.Set("X-Device", "nonsense")
	garbled.Header.Set("X-Timestamp", "yesterday")
	garbled.Header.Set("X-Signature", "v9=zz")
	for name, req := range map[string]*http.Request{
		"no headers":      httptest.NewRequest("GET", "/v1/firmware/manifest", nil),
		"revoked, stale":  signedRequest(t, "GET", "/v1/firmware/manifest", 1, ""),
		"garbage headers": garbled,
	} {
		if got := serve(r.h, req); got.code != 200 || got.raw != want {
			t.Errorf("%s: %d %s", name, got.code, got.raw)
		}
	}
}
