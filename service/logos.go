package main

import (
	"crypto/subtle"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// Airline logos are trademarks, so they are not part of this repository or of the public image:
// the owner mounts a folder of ready-made files (see tools/make_logos.py) and points LOGO_DIR at it.
// Each file is <IATA>.bin: 16x16 pixels, RGB565 big-endian, exactly logoBytes long.
const logoBytes = 16 * 16 * 2

func validLogoCode(c string) bool {
	if len(c) != 2 {
		return false
	}
	for i := 0; i < 2; i++ {
		ch := c[i]
		if !(ch >= 'A' && ch <= 'Z' || ch >= '0' && ch <= '9') {
			return false
		}
	}
	return true
}

// logoHandler serves GET /v1/logo/{code}. The code is checked before it is used as a file name.
func (a *app) logoHandler(w http.ResponseWriter, r *http.Request) {
	if a.token != "" && subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Token")), []byte(a.token)) != 1 {
		writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "missing or wrong token"})
		return
	}
	code := strings.ToUpper(r.PathValue("code"))
	if a.logoDir == "" || !validLogoCode(code) {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no logo"})
		return
	}
	b, err := os.ReadFile(filepath.Join(a.logoDir, code+".bin"))
	if err != nil || len(b) != logoBytes {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "no logo"})
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "public, max-age=86400")
	w.Write(b)
}
