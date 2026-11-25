package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// Firmware updates, service side. The contract is shared/firmware-update.md.
//
// The service image carries the firmware in a folder (FIRMWARE_DIR, /firmware in the image): a
// manifest.json and the image it describes, ledmatrix-<version>.bin. The service only reads it.
// Both routes are public: they ignore the signature headers completely, so a board whose key is
// refused can still fetch a fix. The integrity check is the board's SHA-256 against the manifest.

const (
	firmwareRecheck      = time.Minute      // the folder is looked at again at most this often
	firmwareMinSize      = 100 * 1024       // an image must be larger than this
	firmwareMaxSize      = 2097152          // and at most one app slot
	firmwareMaxNotes     = 200              // characters
	firmwareMaxManifest  = 8 << 10          // bytes; a real manifest is under 1 KB
	firmwareSlots        = 8                // downloads at once; kept apart from the load limit of the data routes
	firmwareWriteTimeout = 10 * time.Minute // the server's 20 s write timeout would cut a slow board off
)

// firmwareManifest is both what manifest.json holds and what the route answers (field order as in
// the contract). The answer is built from the validated fields, never copied from the file.
type firmwareManifest struct {
	Version string `json:"version"`
	File    string `json:"file"`
	Size    int64  `json:"size"`
	SHA256  string `json:"sha256"`
	Notes   string `json:"notes"`
}

// firmwareRelease is a manifest that passed every check together with its image. It is never
// changed after it is built.
type firmwareRelease struct {
	manifest []byte // the 200 answer
	version  string
	file     string // the only name the download route answers to
	path     string // where the image is, built from the validated file name
	size     int64
	sha256   string
}

type firmwareStore struct {
	dir   string
	now   func() time.Time
	logf  func(format string, args ...any)
	slots chan struct{}

	mu      sync.Mutex
	checked time.Time        // when the folder was last looked at
	rel     *firmwareRelease // nil: nothing is published
	state   string           // the state last logged, so a log line says "changed", not "requested"
	sum     hashedImage      // the last image hashed, so a file is hashed once per change
	hashes  int              // how many images have been hashed (for the tests)
}

type hashedImage struct {
	path string
	size int64
	mod  time.Time
	hex  string
}

// newFirmware looks at the folder at once and logs what it found.
func newFirmware(dir string, now func() time.Time, logf func(string, ...any)) *firmwareStore {
	f := &firmwareStore{dir: dir, now: now, logf: logf, slots: make(chan struct{}, firmwareSlots)}
	f.current()
	return f
}

// current returns what is published, looking at the folder again when the last look is a minute old.
func (f *firmwareStore) current() *firmwareRelease {
	if f == nil {
		return nil
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if now := f.now(); f.checked.IsZero() || now.Sub(f.checked) >= firmwareRecheck || now.Before(f.checked) {
		f.checked = now
		rel, why := f.load()
		f.rel = rel
		state := "none: " + why
		if rel != nil {
			state = "published " + rel.version + " " + rel.sha256
		}
		if state != f.state { // one line per change, not per request
			f.state = state
			if rel != nil {
				f.logf("firmware: version %s is published (%d bytes)", rel.version, rel.size)
			} else {
				f.logf("firmware: nothing is published, %s", why)
			}
		}
	}
	return f.rel
}

func isDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return s != ""
}

// validVersion: three dot-separated numbers of 1 to 4 digits.
func validVersion(v string) bool {
	parts := strings.Split(v, ".")
	if len(parts) != 3 {
		return false
	}
	for _, p := range parts {
		if len(p) > 4 || !isDigits(p) {
			return false
		}
	}
	return true
}

// check is every rule about the manifest text itself. The messages are fixed sentences: nothing
// read from the folder is ever copied into the log.
func (m firmwareManifest) check() string {
	switch {
	case !validVersion(m.Version):
		return "the version is not three numbers of 1 to 4 digits"
	case m.File != "ledmatrix-"+m.Version+".bin":
		return "the file name is not ledmatrix-<version>.bin"
	case m.Size <= firmwareMinSize || m.Size > firmwareMaxSize:
		return "the size is not above 100 KB and at most 2097152 bytes"
	case len(m.SHA256) != 64 || !isLowerHex(m.SHA256):
		return "the sha256 is not 64 lowercase hex characters"
	case utf8.RuneCountInString(m.Notes) > firmwareMaxNotes:
		return "the notes are longer than 200 characters"
	}
	return ""
}

// load reads the manifest and checks it against the image. It returns the release, or why there is none.
func (f *firmwareStore) load() (*firmwareRelease, string) {
	b, err := readLimited(filepath.Join(f.dir, "manifest.json"), firmwareMaxManifest)
	switch {
	case errors.Is(err, fs.ErrNotExist):
		return nil, "there is no manifest.json in the firmware folder"
	case err != nil:
		return nil, "manifest.json cannot be read or is too large"
	}
	var m firmwareManifest
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, "manifest.json is not valid JSON of the expected shape"
	}
	if why := m.check(); why != "" {
		return nil, why
	}
	// m.File is now ledmatrix-<three numbers>.bin: no separator, so joining it to the folder is safe.
	path := filepath.Join(f.dir, m.File)
	size, sum, err := f.hashImage(path)
	switch {
	case errors.Is(err, fs.ErrNotExist):
		return nil, "the image named in the manifest is not in the folder"
	case err != nil:
		return nil, "the image cannot be read"
	case size != m.Size:
		return nil, "the image size does not match the manifest"
	case sum != m.SHA256:
		return nil, "the image hash does not match the manifest"
	}
	answer, err := json.Marshal(m)
	if err != nil {
		return nil, "the manifest cannot be written"
	}
	return &firmwareRelease{manifest: answer, version: m.Version, file: m.File, path: path, size: size, sha256: sum}, ""
}

func readLimited(path string, max int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	b, err := io.ReadAll(io.LimitReader(file, max+1))
	if err != nil {
		return nil, err
	}
	if int64(len(b)) > max {
		return nil, errors.New("too large")
	}
	return b, nil
}

// hashImage returns the size and SHA-256 of a regular file. The hash is kept for as long as the
// file's size and modification time stay the same, so it is computed once per change of the file.
// The caller holds f.mu.
func (f *firmwareStore) hashImage(path string) (int64, string, error) {
	file, err := os.Open(path)
	if err != nil {
		return 0, "", err
	}
	defer file.Close()
	fi, err := file.Stat()
	if err != nil {
		return 0, "", err
	}
	if !fi.Mode().IsRegular() {
		return 0, "", errors.New("not a regular file")
	}
	if c := f.sum; c.path == path && c.size == fi.Size() && c.mod.Equal(fi.ModTime()) {
		return c.size, c.hex, nil
	}
	h := sha256.New()
	n, err := io.Copy(h, file)
	if err != nil {
		return 0, "", err
	}
	f.hashes++
	f.sum = hashedImage{path, n, fi.ModTime(), hex.EncodeToString(h.Sum(nil))}
	return n, f.sum.hex, nil
}

// manifest answers GET /v1/firmware/manifest.
func (f *firmwareStore) manifest(w http.ResponseWriter, r *http.Request) {
	rel := f.current()
	if rel == nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no firmware is published"})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache")
	w.Write(rel.manifest)
}

// download answers GET /v1/firmware/{file}. The requested name is only compared with the
// manifest's file name; it is never used to build a path.
//
// It does not use the data routes' load limit (a slow download must not starve them) but has its
// own few slots, and it lifts the server's write timeout, which is meant for small JSON answers.
func (f *firmwareStore) download(w http.ResponseWriter, r *http.Request) {
	rel := f.current()
	if rel == nil || r.PathValue("file") != rel.file {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no such firmware"})
		return
	}
	select {
	case f.slots <- struct{}{}:
		defer func() { <-f.slots }()
	default:
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "busy"})
		return
	}
	file, err := os.Open(rel.path)
	if err != nil {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no such firmware"})
		return
	}
	defer file.Close()
	// If the file changed under us since it was checked, say so instead of sending different bytes.
	if fi, err := file.Stat(); err != nil || !fi.Mode().IsRegular() || fi.Size() != rel.size {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no such firmware"})
		return
	}
	http.NewResponseController(w).SetWriteDeadline(time.Now().Add(firmwareWriteTimeout)) // not supported by every writer: fine
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("ETag", `"`+rel.sha256+`"`)
	http.ServeContent(w, r, rel.file, time.Time{}, file)
}
