package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// The firmware update routes. Every test works in a temporary folder; the images are made-up bytes.

const (
	fwVersion = "0.10.1"
	fwFile    = "ledmatrix-0.10.1.bin"
	fwNone    = `{"error":"no firmware is published"}`
	fwNoFile  = `{"error":"no such firmware"}`
)

// clock is a stopped clock that can be moved, safe to read from many goroutines.
type clock struct{ ns atomic.Int64 }

func newClock() *clock {
	c := &clock{}
	c.ns.Store(time.Unix(testNow, 0).UnixNano())
	return c
}
func (c *clock) now() time.Time          { return time.Unix(0, c.ns.Load()) }
func (c *clock) advance(d time.Duration) { c.ns.Add(int64(d)) }

// testImage is deterministic made-up content; seed makes two images differ.
func testImage(size int, seed byte) []byte {
	b := make([]byte, size)
	for i := range b {
		b[i] = byte(i*31+i>>8) ^ seed
	}
	return b
}

var mtimeCounter atomic.Int64

// putFile writes a file and gives it a modification time nobody else has, so that "the file
// changed" never depends on the clock of the file system.
func putFile(t *testing.T, path string, b []byte) {
	t.Helper()
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
	mt := time.Unix(1_700_000_000+mtimeCounter.Add(1), 0)
	if err := os.Chtimes(path, mt, mt); err != nil {
		t.Fatal(err)
	}
}

// writeFirmware writes ledmatrix-<version>.bin and a manifest for it (after mutate has changed the
// manifest) into dir, and returns the manifest as written.
func writeFirmware(t *testing.T, dir, version string, image []byte, mutate func(m *firmwareManifest)) firmwareManifest {
	t.Helper()
	sum := sha256.Sum256(image)
	m := firmwareManifest{Version: version, File: "ledmatrix-" + version + ".bin", Size: int64(len(image)), SHA256: hex.EncodeToString(sum[:]), Notes: "A short note."}
	if mutate != nil {
		mutate(&m)
	}
	putFile(t, filepath.Join(dir, "ledmatrix-"+version+".bin"), image)
	b, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	putFile(t, filepath.Join(dir, "manifest.json"), b)
	return m
}

type fwRig struct {
	h    http.Handler
	a    *app
	clk  *clock
	logs *logCapture
}

// newRig is the real routes with a firmware store on dir and a clock that can be moved.
func newRig(t *testing.T, dir string) *fwRig {
	t.Helper()
	r := &fwRig{clk: newClock(), logs: &logCapture{}}
	r.a = newStub(t).app(t, lisbonNoon(t))
	r.a.firmware = newFirmware(dir, r.clk.now, r.logs.logf)
	r.a.auth.now = r.clk.now
	r.h = r.a.routes()
	return r
}

func (r *fwRig) get(method, target string, hdr ...string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, target, nil)
	for i := 0; i+1 < len(hdr); i += 2 {
		req.Header.Set(hdr[i], hdr[i+1])
	}
	rec := httptest.NewRecorder()
	r.h.ServeHTTP(rec, req)
	return rec
}

func (r *fwRig) wantPublished(t *testing.T, what string, m firmwareManifest, image []byte) {
	t.Helper()
	rec := r.get("GET", "/v1/firmware/manifest")
	var got firmwareManifest
	if rec.Code != 200 || json.Unmarshal(rec.Body.Bytes(), &got) != nil || got != m {
		t.Fatalf("%s: manifest %d %s, want %+v", what, rec.Code, rec.Body.String(), m)
	}
	rec = r.get("GET", "/v1/firmware/"+m.File)
	if rec.Code != 200 || !bytes.Equal(rec.Body.Bytes(), image) {
		t.Fatalf("%s: download %d, %d bytes, want the image of %d bytes", what, rec.Code, rec.Body.Len(), len(image))
	}
}

func (r *fwRig) wantNothing(t *testing.T, what, file string) {
	t.Helper()
	rec := r.get("GET", "/v1/firmware/manifest")
	if rec.Code != 404 || rec.Body.String() != fwNone {
		t.Errorf("%s: manifest %d %s, want 404 %s", what, rec.Code, rec.Body.String(), fwNone)
	}
	rec = r.get("GET", "/v1/firmware/"+url.PathEscape(file))
	if rec.Code != 404 || rec.Body.String() != fwNoFile {
		t.Errorf("%s: download %d %.60s, want 404 %s", what, rec.Code, rec.Body.String(), fwNoFile)
	}
}

// ---- the happy path --------------------------------------------------------------------------------------

func TestFirmwareManifestAndDownload(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 1)
	m := writeFirmware(t, dir, fwVersion, image, nil)
	r := newRig(t, dir)

	rec := r.get("GET", "/v1/firmware/manifest")
	want := `{"version":"0.10.1","file":"ledmatrix-0.10.1.bin","size":150000,"sha256":"` + m.SHA256 + `","notes":"A short note."}`
	if rec.Code != 200 || rec.Body.String() != want {
		t.Fatalf("manifest %d %s\nwant %s", rec.Code, rec.Body.String(), want)
	}
	if rec.Header().Get("Cache-Control") != "no-cache" || rec.Header().Get("Content-Type") != "application/json" {
		t.Errorf("manifest headers: %v", rec.Header())
	}

	rec = r.get("GET", "/v1/firmware/"+fwFile)
	if rec.Code != 200 || !bytes.Equal(rec.Body.Bytes(), image) {
		t.Fatalf("download %d, %d bytes", rec.Code, rec.Body.Len())
	}
	if h := rec.Header(); h.Get("Content-Type") != "application/octet-stream" || h.Get("Content-Length") != "150000" || h.Get("Accept-Ranges") != "bytes" || h.Get("ETag") != `"`+m.SHA256+`"` {
		t.Errorf("download headers: %v", h)
	}
	if lines := r.logs.all(); len(lines) != 1 || !strings.Contains(lines[0], "version 0.10.1 is published") {
		t.Errorf("start-up must log what it found, once: %q", lines)
	}
}

func TestFirmwareManifestWithoutNotesIsFine(t *testing.T) {
	dir := t.TempDir()
	image := testImage(120_000, 2)
	b, _ := json.Marshal(map[string]any{"version": "1.2.3", "file": "ledmatrix-1.2.3.bin", "size": len(image), "sha256": fmt.Sprintf("%x", sha256.Sum256(image))})
	putFile(t, filepath.Join(dir, "manifest.json"), b)
	putFile(t, filepath.Join(dir, "ledmatrix-1.2.3.bin"), image)
	rec := newRig(t, dir).get("GET", "/v1/firmware/manifest")
	if rec.Code != 200 || !strings.HasSuffix(rec.Body.String(), `,"notes":""}`) {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
}

// The answer is built from the checked fields: anything else in manifest.json is not passed on.
func TestFirmwareManifestAnswerHasOnlyTheContractFields(t *testing.T) {
	dir := t.TempDir()
	image := testImage(120_000, 3)
	sum := sha256.Sum256(image)
	b := fmt.Sprintf(`{"version":"1.2.3","file":"ledmatrix-1.2.3.bin","size":%d,"sha256":"%x","notes":"n","token":"extra","path":"/etc/passwd"}`, len(image), sum)
	putFile(t, filepath.Join(dir, "manifest.json"), []byte(b))
	putFile(t, filepath.Join(dir, "ledmatrix-1.2.3.bin"), image)
	rec := newRig(t, dir).get("GET", "/v1/firmware/manifest")
	if rec.Code != 200 || strings.Contains(rec.Body.String(), "extra") || strings.Contains(rec.Body.String(), "passwd") {
		t.Fatalf("%d %s", rec.Code, rec.Body.String())
	}
}

// ---- everything that makes "no firmware is published" -----------------------------------------------------

func TestFirmwareInvalid(t *testing.T) {
	image := testImage(150_000, 4)
	flip := func(s string) string { return flipHexDigit(s, 5) }
	type tc struct {
		name  string
		file  string // the download name to try
		setup func(t *testing.T, dir string)
	}
	var cases []tc
	withManifest := func(name string, mutate func(m *firmwareManifest)) {
		cases = append(cases, tc{name, fwFile, func(t *testing.T, dir string) { writeFirmware(t, dir, fwVersion, image, mutate) }})
	}
	withManifest("size one too big", func(m *firmwareManifest) { m.Size++ })
	withManifest("size one too small", func(m *firmwareManifest) { m.Size-- })
	withManifest("hash of other bytes", func(m *firmwareManifest) { m.SHA256 = flip(m.SHA256) })
	withManifest("hash in upper case", func(m *firmwareManifest) { m.SHA256 = strings.ToUpper(m.SHA256) })
	withManifest("hash too short", func(m *firmwareManifest) { m.SHA256 = m.SHA256[:63] })
	withManifest("hash too long", func(m *firmwareManifest) { m.SHA256 += "0" })
	withManifest("hash empty", func(m *firmwareManifest) { m.SHA256 = "" })
	withManifest("notes of 201 characters", func(m *firmwareManifest) { m.Notes = strings.Repeat("n", 201) })
	withManifest("notes of 201 two-byte characters", func(m *firmwareManifest) { m.Notes = strings.Repeat("é", 201) })
	for _, f := range []string{"ledmatrix-0.10.2.bin", "ledmatrix-0.10.1.BIN", "Ledmatrix-0.10.1.bin", "ledmatrix-0.10.1.bin ", "firmware.bin", "../ledmatrix-0.10.1.bin", "/ledmatrix-0.10.1.bin", "ledmatrix-0.10.1.bin/../x", "", "manifest.json"} {
		f := f
		withManifest(fmt.Sprintf("file %q", f), func(m *firmwareManifest) { m.File = f })
	}
	for _, v := range []string{"1.2", "1", "1.2.3.4", "1.2.x", "v1.2.3", "1.2.3-rc1", "12345.0.0", "0.12345.0", "0.0.12345", "", "1..3", ".1.2", "1.2.", " 1.2.3", "1.2.3 ", "-1.2.3", "+1.2.3", "1.2.3+4", "١.٢.٣", "0x1.2.3"} {
		v := v
		cases = append(cases, tc{fmt.Sprintf("version %q", v), "ledmatrix-" + v + ".bin", func(t *testing.T, dir string) {
			writeFirmware(t, dir, "9.9.9", image, func(m *firmwareManifest) { m.Version, m.File = v, "ledmatrix-"+v+".bin" })
			putFile(t, filepath.Join(dir, "ledmatrix-"+v+".bin"), image) // the image is there: only the version text can be the reason
		}})
	}
	cases = append(cases,
		tc{"no manifest.json, only the image", fwFile, func(t *testing.T, dir string) {
			putFile(t, filepath.Join(dir, fwFile), image)
		}},
		tc{"only a README", fwFile, func(t *testing.T, dir string) { putFile(t, filepath.Join(dir, "README.md"), []byte("x")) }},
		tc{"image missing", fwFile, func(t *testing.T, dir string) {
			writeFirmware(t, dir, fwVersion, image, nil)
			os.Remove(filepath.Join(dir, fwFile))
		}},
		tc{"image is a directory", fwFile, func(t *testing.T, dir string) {
			writeFirmware(t, dir, fwVersion, image, nil)
			os.Remove(filepath.Join(dir, fwFile))
			os.Mkdir(filepath.Join(dir, fwFile), 0o755)
		}},
		tc{"image one byte shorter than the manifest says", fwFile, func(t *testing.T, dir string) {
			writeFirmware(t, dir, fwVersion, image, nil)
			putFile(t, filepath.Join(dir, fwFile), image[:len(image)-1])
		}},
		tc{"image with other bytes of the same size", fwFile, func(t *testing.T, dir string) {
			writeFirmware(t, dir, fwVersion, image, nil)
			putFile(t, filepath.Join(dir, fwFile), testImage(len(image), 9))
		}},
		tc{"manifest.json is a directory", fwFile, func(t *testing.T, dir string) {
			os.Mkdir(filepath.Join(dir, "manifest.json"), 0o755)
			putFile(t, filepath.Join(dir, fwFile), image)
		}},
	)
	valid := func() string {
		sum := sha256.Sum256(image)
		return fmt.Sprintf(`{"version":"0.10.1","file":"ledmatrix-0.10.1.bin","size":%d,"sha256":"%x","notes":"n"}`, len(image), sum)
	}
	for name, text := range map[string]string{
		"empty manifest":          "",
		"not JSON":                "<html>",
		"an array":                "[]",
		"an empty object":         "{}",
		"null":                    "null",
		"trailing garbage":        valid() + "x",
		"two objects":             valid() + valid(),
		"size as text":            strings.Replace(valid(), fmt.Sprintf(`"size":%d`, len(image)), fmt.Sprintf(`"size":"%d"`, len(image)), 1),
		"size with a fraction":    strings.Replace(valid(), fmt.Sprintf(`"size":%d`, len(image)), fmt.Sprintf(`"size":%d.5`, len(image)), 1),
		"version as a number":     strings.Replace(valid(), `"version":"0.10.1"`, `"version":0.101`, 1),
		"manifest of 9 KB":        valid() + strings.Repeat(" ", 9<<10),
		"a UTF-8 byte order mark": "\xef\xbb\xbf" + valid(),
	} {
		text := text
		cases = append(cases, tc{name, fwFile, func(t *testing.T, dir string) {
			putFile(t, filepath.Join(dir, fwFile), image)
			putFile(t, filepath.Join(dir, "manifest.json"), []byte(text))
		}})
	}
	for _, c := range cases {
		dir := t.TempDir()
		c.setup(t, dir)
		r := newRig(t, dir)
		r.wantNothing(t, c.name, c.file)
		if lines := r.logs.all(); len(lines) != 1 || !strings.HasPrefix(lines[0], "firmware: nothing is published, ") {
			t.Errorf("%s: want one line saying nothing is published, got %q", c.name, lines)
		}
	}
	// the control: the same image with an untouched manifest is published
	dir := t.TempDir()
	putFile(t, filepath.Join(dir, fwFile), image)
	putFile(t, filepath.Join(dir, "manifest.json"), []byte(valid()))
	if rec := newRig(t, dir).get("GET", "/v1/firmware/manifest"); rec.Code != 200 {
		t.Fatalf("the control manifest must be accepted: %d %s", rec.Code, rec.Body.String())
	}
}

func TestFirmwareVersionsAccepted(t *testing.T) {
	image := testImage(120_000, 5)
	for _, v := range []string{"0.0.0", "0.10.0", "1.2.3", "9999.9999.9999", "0001.0.0"} {
		dir := t.TempDir()
		m := writeFirmware(t, dir, v, image, nil)
		newRig(t, dir).wantPublished(t, v, m, image)
	}
}

func TestFirmwareSizeBounds(t *testing.T) {
	for _, c := range []struct {
		size int
		ok   bool
	}{{100*1024 - 1, false}, {100 * 1024, false}, {100*1024 + 1, true}, {2097152 - 1, true}, {2097152, true}, {2097152 + 1, false}} {
		dir := t.TempDir()
		image := testImage(c.size, 6)
		m := writeFirmware(t, dir, fwVersion, image, nil)
		r := newRig(t, dir)
		if c.ok {
			r.wantPublished(t, strconv.Itoa(c.size), m, image)
		} else {
			r.wantNothing(t, strconv.Itoa(c.size), fwFile)
		}
	}
}

func TestFirmwareNotesBounds(t *testing.T) {
	image := testImage(120_000, 7)
	for _, notes := range []string{"", strings.Repeat("n", 200), strings.Repeat("é", 200), strings.Repeat("日", 200)} {
		dir := t.TempDir()
		m := writeFirmware(t, dir, fwVersion, image, func(m *firmwareManifest) { m.Notes = notes })
		newRig(t, dir).wantPublished(t, fmt.Sprintf("%d characters", len([]rune(notes))), m, image)
	}
}

func TestFirmwareMissingFolder(t *testing.T) {
	r := newRig(t, filepath.Join(t.TempDir(), "does-not-exist"))
	r.wantNothing(t, "no folder", fwFile)
	if r.get("GET", "/healthz").Body.String() != "ok" {
		t.Error("healthz must not depend on the firmware folder")
	}
}

// ---- reading the folder again -----------------------------------------------------------------------------

func TestFirmwareLogsOnlyChangesOfState(t *testing.T) {
	dir := t.TempDir()
	r := newRig(t, dir)
	image := testImage(150_000, 8)
	for i := 0; i < 30; i++ {
		r.get("GET", "/v1/firmware/manifest")
		r.get("GET", "/v1/firmware/"+fwFile)
		r.clk.advance(61 * time.Second) // every request looks at the folder again
	}
	if n := len(r.logs.all()); n != 1 {
		t.Fatalf("30 minutes without a manifest logged %d lines, want 1: %q", n, r.logs.all())
	}
	m := writeFirmware(t, dir, fwVersion, image, nil)
	for i := 0; i < 5; i++ {
		r.clk.advance(61 * time.Second)
		r.wantPublished(t, "published", m, image)
	}
	if n := len(r.logs.all()); n != 2 {
		t.Fatalf("publishing logs one more line, got %d: %q", n, r.logs.all())
	}
	putFile(t, filepath.Join(dir, fwFile), testImage(len(image), 9)) // now the hash is wrong
	for i := 0; i < 5; i++ {
		r.clk.advance(61 * time.Second)
		r.get("GET", "/v1/firmware/manifest")
	}
	lines := r.logs.all()
	if len(lines) != 3 || !strings.Contains(lines[2], "hash does not match") {
		t.Fatalf("a broken image logs one line saying why: %q", lines)
	}
	for _, l := range lines {
		for _, s := range []string{m.SHA256, dir, "ledmatrix-"} {
			if strings.Contains(l, s) {
				t.Errorf("the log line %q repeats %q from the folder", l, s)
			}
		}
	}
}

func TestFirmwareReplacedWhileRunning(t *testing.T) {
	dir := t.TempDir()
	img1, img2 := testImage(150_000, 10), testImage(160_000, 11)
	m1 := writeFirmware(t, dir, "0.10.1", img1, nil)
	r := newRig(t, dir)
	r.wantPublished(t, "first", m1, img1)

	// a new image and manifest appear; the old image goes
	m2 := writeFirmware(t, dir, "0.10.2", img2, nil)
	os.Remove(filepath.Join(dir, "ledmatrix-0.10.1.bin"))
	r.clk.advance(59 * time.Second)
	if rec := r.get("GET", "/v1/firmware/manifest"); !strings.Contains(rec.Body.String(), `"0.10.1"`) {
		t.Fatalf("the folder is looked at once a minute at most: %s", rec.Body.String())
	}
	r.clk.advance(time.Second)
	r.wantPublished(t, "after a minute", m2, img2)
	if rec := r.get("GET", "/v1/firmware/ledmatrix-0.10.1.bin"); rec.Code != 404 {
		t.Errorf("the old name must be gone: %d", rec.Code)
	}

	// the manifest goes: nothing is published any more
	os.Remove(filepath.Join(dir, "manifest.json"))
	r.clk.advance(time.Minute)
	r.wantNothing(t, "manifest removed", "ledmatrix-0.10.2.bin")
}

func TestFirmwareImageIsHashedOncePerChange(t *testing.T) {
	dir := t.TempDir()
	m := writeFirmware(t, dir, "0.10.1", testImage(150_000, 12), nil)
	r := newRig(t, dir)
	for i := 0; i < 20; i++ {
		r.clk.advance(61 * time.Second)
		r.get("GET", "/v1/firmware/manifest")
		r.get("GET", "/v1/firmware/"+m.File)
	}
	if n := r.a.firmware.hashes; n != 1 {
		t.Fatalf("the image was hashed %d times in 20 checks, want 1", n)
	}
	writeFirmware(t, dir, "0.10.2", testImage(150_000, 13), nil)
	r.clk.advance(61 * time.Second)
	r.get("GET", "/v1/firmware/manifest")
	r.clk.advance(61 * time.Second)
	r.get("GET", "/v1/firmware/manifest")
	if n := r.a.firmware.hashes; n != 2 {
		t.Fatalf("a new image is hashed once: %d", n)
	}
}

// ---- which names are served ------------------------------------------------------------------------------

func TestFirmwareDownloadNames(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 14)
	m := writeFirmware(t, dir, fwVersion, image, nil)
	putFile(t, filepath.Join(dir, "README.md"), []byte("not for the board"))
	putFile(t, filepath.Join(dir, "secret.txt"), []byte("not for the board"))
	r := newRig(t, dir)

	// these reach the handler and must be refused there
	for _, name := range []string{
		"ledmatrix-0.10.2.bin", "LEDMATRIX-0.10.1.BIN", "ledmatrix-0.10.1.bin%00", "ledmatrix-0.10.1.bin%20", "ledmatrix-0.10.1.bin%2F", "%2Fledmatrix-0.10.1.bin",
		"manifest.json", "README.md", "secret.txt", "%2e", "%2e%2e",
		"..%2Fmanifest.json", "..%2fsecret.txt", "%2e%2e%2fsecret.txt", "%2E%2E%2Fsecret.txt", "..%5Csecret.txt", "..%252Fsecret.txt",
		"%2Fetc%2Fpasswd", "%2Fetc%2Fpasswd%00.bin", "..%2F..%2F..%2Fetc%2Fpasswd", "ledmatrix-0.10.1.bin%2F..%2Fsecret.txt",
		"C:%5Cwindows%5Cwin.ini", "ledmatrix-0.10.1.bin::$DATA",
	} {
		rec := r.get("GET", "/v1/firmware/"+name)
		if rec.Code != 404 || rec.Body.String() != fwNoFile {
			t.Errorf("/v1/firmware/%s: want 404 %s, got %d %.60s", name, fwNoFile, rec.Code, rec.Body.String())
		}
	}
	// these the mux itself cleans or refuses: whatever it answers, nothing is served
	for _, target := range []string{
		"/v1/firmware/.", "/v1/firmware/../manifest.json", "/v1/firmware/../../etc/passwd", "/v1/firmware//etc/passwd", "/v1/firmware/./secret.txt",
		"/v1/firmware/ledmatrix-0.10.1.bin/", "/v1/firmware/ledmatrix-0.10.1.bin/..", "/v1/firmware/", "/v1/firmware", "/v1/firmware/%2e%2e/%2e%2e/etc/passwd",
		"/v1/firmware/subdir/ledmatrix-0.10.1.bin", "/etc/passwd",
	} {
		rec := r.get("GET", target)
		if rec.Code == 200 || bytes.Contains(rec.Body.Bytes(), []byte("not for the board")) || bytes.Contains(rec.Body.Bytes(), image[:64]) {
			t.Errorf("%s: served something (%d)", target, rec.Code)
		}
	}
	// the right name still works, and only it
	r.wantPublished(t, "after the attempts", m, image)
	// the other methods are not routed
	for _, method := range []string{"POST", "PUT", "DELETE", "PATCH"} {
		if rec := r.get(method, "/v1/firmware/"+fwFile); rec.Code != http.StatusMethodNotAllowed {
			t.Errorf("%s: %d", method, rec.Code)
		}
	}
}

func TestFirmwareRangeAndHead(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 15)
	writeFirmware(t, dir, fwVersion, image, nil)
	r := newRig(t, dir)
	target := "/v1/firmware/" + fwFile

	rec := r.get("GET", target, "Range", "bytes=10-19")
	if rec.Code != 206 || !bytes.Equal(rec.Body.Bytes(), image[10:20]) || rec.Header().Get("Content-Range") != "bytes 10-19/150000" || rec.Header().Get("Content-Length") != "10" {
		t.Errorf("range 10-19: %d %v %d bytes", rec.Code, rec.Header(), rec.Body.Len())
	}
	rec = r.get("GET", target, "Range", "bytes=-5")
	if rec.Code != 206 || !bytes.Equal(rec.Body.Bytes(), image[len(image)-5:]) {
		t.Errorf("last 5 bytes: %d", rec.Code)
	}
	rec = r.get("GET", target, "Range", "bytes=100000-")
	if rec.Code != 206 || !bytes.Equal(rec.Body.Bytes(), image[100000:]) {
		t.Errorf("open range: %d, %d bytes", rec.Code, rec.Body.Len())
	}
	if rec = r.get("GET", target, "Range", "bytes=150000-"); rec.Code != http.StatusRequestedRangeNotSatisfiable {
		t.Errorf("a range past the end: %d", rec.Code)
	}
	// a resume that goes with the same image works, one that does not falls back to the whole file
	etag := `"` + fmt.Sprintf("%x", sha256.Sum256(image)) + `"`
	if rec = r.get("GET", target, "Range", "bytes=0-9", "If-Range", etag); rec.Code != 206 {
		t.Errorf("If-Range with the ETag: %d", rec.Code)
	}
	if rec = r.get("GET", target, "Range", "bytes=0-9", "If-Range", `"other"`); rec.Code != 200 || rec.Body.Len() != len(image) {
		t.Errorf("If-Range with another ETag: %d, %d bytes", rec.Code, rec.Body.Len())
	}

	rec = r.get("HEAD", target)
	if rec.Code != 200 || rec.Body.Len() != 0 || rec.Header().Get("Content-Length") != "150000" || rec.Header().Get("Content-Type") != "application/octet-stream" {
		t.Errorf("HEAD: %d %v %d bytes", rec.Code, rec.Header(), rec.Body.Len())
	}
	if rec = r.get("HEAD", "/v1/firmware/ledmatrix-9.9.9.bin"); rec.Code != 404 {
		t.Errorf("HEAD of another name: %d", rec.Code)
	}
}

// ---- public, and nothing else changes --------------------------------------------------------------------

func TestFirmwareRoutesAreOpenToUnsignedCallers(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 16)
	m := writeFirmware(t, dir, fwVersion, image, nil)
	r := newRig(t, dir)
	r.a.auth.revoked = map[string]bool{testDevice: true}
	revokedStale := signedRequest(t, "GET", "/v1/firmware/manifest", 1, "") // the key is refused twice over

	badHeaders := func(req *http.Request) *http.Request {
		req.Header.Set("X-Device", "nonsense")
		req.Header.Set("X-Timestamp", "yesterday")
		req.Header.Set("X-Signature", "v9=zz")
		return req
	}
	flipped := signedRequest(t, "GET", "/v1/firmware/manifest", testNow, "")
	flipped.Header.Set("X-Signature", flipHexDigit(flipped.Header.Get("X-Signature"), 5))
	for name, req := range map[string]*http.Request{
		"no headers":              httptest.NewRequest("GET", "/v1/firmware/manifest", nil),
		"a valid signature":       signedRequest(t, "GET", "/v1/firmware/manifest", testNow, ""),
		"a wrong signature":       flipped,
		"garbage headers":         badHeaders(httptest.NewRequest("GET", "/v1/firmware/manifest", nil)),
		"a revoked, stale device": revokedStale,
		"the old token": func() *http.Request {
			q := httptest.NewRequest("GET", "/v1/firmware/manifest", nil)
			q.Header.Set("X-Token", "x")
			return q
		}(),
	} {
		got := serve(r.h, req)
		if got.code != 200 || got.raw != string(mustJSON(t, m)) {
			t.Errorf("manifest with %s: %d %s", name, got.code, got.raw)
		}
		down := httptest.NewRequest("GET", "/v1/firmware/"+m.File, nil)
		down.Header = req.Header.Clone()
		rec := httptest.NewRecorder()
		r.h.ServeHTTP(rec, down)
		if rec.Code != 200 || !bytes.Equal(rec.Body.Bytes(), image) {
			t.Errorf("download with %s: %d", name, rec.Code)
		}
	}

	// with a manifest in the folder, everything else is as before
	r.a.auth.revoked = nil
	if rec := r.get("GET", "/healthz"); rec.Code != 200 || rec.Body.String() != "ok" {
		t.Errorf("healthz: %d", rec.Code)
	}
	for _, uri := range []string{weatherURI, "/v1/flights?lat=1&lon=1", "/v1/headlines?q=x", "/v1/logo/TP"} {
		wantRefused(t, uri+" unsigned", serve(r.h, httptest.NewRequest("GET", uri, nil)), reasonMissing, nil)
	}
	wantRefused(t, "weather with a wrong signature", serve(r.h, flippedWeather(t)), reasonBadSignature, nil)
	if got := serve(r.h, signedRequest(t, "GET", weatherURI, testNow, "")); got.code != 200 || !strings.Contains(got.raw, `"loc":"Lisboa"`) {
		t.Errorf("a signed request for the weather: %d %s", got.code, got.raw)
	}
}

func flippedWeather(t *testing.T) *http.Request {
	req := signedRequest(t, "GET", weatherURI, testNow, "")
	req.Header.Set("X-Signature", flipHexDigit(req.Header.Get("X-Signature"), 5))
	return req
}

func mustJSON(t *testing.T, v any) []byte {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// A slow download must not starve the signed routes, and a flood of signed requests must not
// block the update: they use different limits.
func TestFirmwareHasItsOwnLoadLimit(t *testing.T) {
	dir := t.TempDir()
	image := testImage(150_000, 17)
	m := writeFirmware(t, dir, fwVersion, image, nil)
	r := newRig(t, dir)
	signedWeather := func() answer { return serve(r.h, signedRequest(t, "GET", weatherURI, testNow, "")) }

	// every slot of the data routes taken: they answer busy, the firmware is untouched
	fillSlots(t, r.a.sem)
	if got := signedWeather(); got.code != http.StatusServiceUnavailable {
		t.Fatalf("the data routes must be busy in this test: %d", got.code)
	}
	r.wantPublished(t, "data routes saturated", m, image)
	drainSlots(t, r.a.sem)

	// every download slot taken: more downloads are told to wait, the data routes and the manifest are untouched
	fillSlots(t, r.a.firmware.slots)
	if rec := r.get("GET", "/v1/firmware/"+fwFile); rec.Code != http.StatusServiceUnavailable || rec.Body.String() != `{"error":"busy"}` {
		t.Errorf("downloads at the limit: %d %s", rec.Code, rec.Body.String())
	}
	if rec := r.get("GET", "/v1/firmware/ledmatrix-9.9.9.bin"); rec.Code != 404 {
		t.Errorf("a wrong name is 404 even when busy: %d", rec.Code)
	}
	if rec := r.get("GET", "/v1/firmware/manifest"); rec.Code != 200 {
		t.Errorf("the manifest does not need a slot: %d", rec.Code)
	}
	if got := signedWeather(); got.code != 200 {
		t.Errorf("a signed request while downloads are at the limit: %d %s", got.code, got.raw)
	}
	drainSlots(t, r.a.firmware.slots)
	if rec := r.get("GET", "/v1/firmware/"+fwFile); rec.Code != 200 {
		t.Errorf("after the slots are free again: %d", rec.Code)
	}
	if len(r.a.firmware.slots) != 0 || len(r.a.sem) != 0 {
		t.Errorf("slots leaked: downloads %d, data routes %d", len(r.a.firmware.slots), len(r.a.sem))
	}
}

// fillSlots takes every free slot of a limit; if a slot is already taken (a leak) the test fails instead of hanging.
func fillSlots(t *testing.T, ch chan struct{}) {
	t.Helper()
	for i := 0; i < cap(ch); i++ {
		select {
		case ch <- struct{}{}:
		default:
			t.Fatalf("a slot was still taken before the test filled them: %d of %d", len(ch)-i, cap(ch))
		}
	}
}

func drainSlots(t *testing.T, ch chan struct{}) {
	t.Helper()
	for i := 0; i < cap(ch); i++ {
		select {
		case <-ch:
		default:
			t.Fatal("fewer slots taken than the test took")
		}
	}
}

// deadlineWriter is a response writer whose connection can have a write deadline, like the real one.
type deadlineWriter struct {
	*httptest.ResponseRecorder
	deadline time.Time
	calls    int
}

func (d *deadlineWriter) SetWriteDeadline(t time.Time) error {
	d.deadline, d.calls = t, d.calls+1
	return nil
}

// The server's write timeout (20 s) is for small answers; a board downloading 1.4 MB over Wi-Fi
// while it writes flash needs longer. The deadline is lifted for the download only, and it must
// get through the request logger that wraps every response.
func TestFirmwareDownloadLiftsTheWriteDeadline(t *testing.T) {
	dir := t.TempDir()
	writeFirmware(t, dir, fwVersion, testImage(150_000, 18), nil)
	r := newRig(t, dir)
	for _, c := range []struct {
		target string
		set    bool
	}{{"/v1/firmware/" + fwFile, true}, {"/v1/firmware/manifest", false}, {"/v1/firmware/other.bin", false}, {"/healthz", false}} {
		w := &deadlineWriter{ResponseRecorder: httptest.NewRecorder()}
		before := time.Now()
		r.h.ServeHTTP(w, httptest.NewRequest("GET", c.target, nil))
		if (w.calls == 1) != c.set || w.calls > 1 {
			t.Errorf("%s: the write deadline was set %d times, want set=%v", c.target, w.calls, c.set)
			continue
		}
		if c.set && (w.deadline.Before(before.Add(firmwareWriteTimeout-time.Minute)) || w.deadline.After(time.Now().Add(firmwareWriteTimeout+time.Minute))) {
			t.Errorf("%s: deadline %v is not about %v ahead", c.target, w.deadline, firmwareWriteTimeout)
		}
	}
}

// Over a real connection, with Go's own client: exact Content-Length, the exact bytes, ranges.
func TestFirmwareOverTheWire(t *testing.T) {
	dir := t.TempDir()
	image := testImage(300_000, 19)
	m := writeFirmware(t, dir, fwVersion, image, nil)
	r := newRig(t, dir)
	srv := httptest.NewServer(r.h)
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/v1/firmware/" + m.File)
	if err != nil {
		t.Fatal(err)
	}
	b, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 200 || resp.ContentLength != int64(len(image)) || !bytes.Equal(b, image) || resp.Header.Get("Content-Type") != "application/octet-stream" {
		t.Fatalf("%d, Content-Length %d, %d bytes, %v", resp.StatusCode, resp.ContentLength, len(b), resp.Header)
	}
	sum := sha256.Sum256(b)
	if hex.EncodeToString(sum[:]) != m.SHA256 {
		t.Error("the downloaded bytes do not match the manifest's hash")
	}

	req, _ := http.NewRequest("GET", srv.URL+"/v1/firmware/"+m.File, nil)
	req.Header.Set("Range", "bytes=299000-")
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	b, _ = io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != 206 || !bytes.Equal(b, image[299000:]) {
		t.Errorf("range over the wire: %d, %d bytes", resp.StatusCode, len(b))
	}

	// a slow reader of a big file does not hold up a signed request on the same server
	slow, err := http.Get(srv.URL + "/v1/firmware/" + m.File)
	if err != nil {
		t.Fatal(err)
	}
	defer slow.Body.Close()
	signed := signedRequest(t, "GET", weatherURI, testNow, "")
	req, _ = http.NewRequest("GET", srv.URL+weatherURI, nil)
	req.Header = signed.Header
	req.Host = signed.Host
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Errorf("a signed request while a download is open: %d", resp.StatusCode)
	}
}

func TestFirmwareConcurrentRequestsAndChanges(t *testing.T) {
	dir := t.TempDir()
	img1, img2 := testImage(150_000, 20), testImage(150_000, 21)
	m1 := writeFirmware(t, dir, "0.10.1", img1, nil)
	r := newRig(t, dir)
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 60; i++ {
				rec := r.get("GET", "/v1/firmware/manifest")
				if rec.Code != 200 && rec.Code != 404 {
					t.Errorf("manifest: %d", rec.Code)
				}
				if rec := r.get("GET", "/v1/firmware/"+m1.File); rec.Code != 200 && rec.Code != 404 && rec.Code != 503 {
					t.Errorf("download: %d", rec.Code)
				}
				r.clk.advance(7 * time.Second)
			}
		}()
	}
	wg.Add(1)
	go func() { // the folder changes while the requests run
		defer wg.Done()
		for i := 0; i < 10; i++ {
			writeFirmware(t, dir, "0.10.1", img2, nil)
			writeFirmware(t, dir, "0.10.1", img1, nil)
		}
	}()
	wg.Wait()
}
