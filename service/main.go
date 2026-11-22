// ledmatrix-api answers the LedMatrix display in small JSON: nearby planes with their route,
// IPMA weather, and news headlines. It exists so the display makes one cheap request instead of
// fetching and parsing large responses from several servers.
//
//	GET /healthz
//	GET /v1/weather?lat=&lon=
//	GET /v1/flights?lat=&lon=&radius_km=&alt_min=&alt_max=&n=
//	GET /v1/headlines?q=&n=&hl=&gl=&ceid=
//	GET /v1/logo/{iata}   512 bytes, 16x16 RGB565 big-endian; 404 when the airline has none
//
// Configuration (environment): LISTEN (default :8080), API_TOKEN (optional; when set, requests
// must send it in the X-Token header), LOGO_DIR (optional folder of <IATA>.bin logo files).
package main

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"
	_ "time/tzdata"
)

type apiError struct {
	Status int
	Msg    string
}

func (e *apiError) Error() string { return e.Msg }

func badRequest(msg string) error { return &apiError{http.StatusBadRequest, msg} }

type app struct {
	weather  *Weather
	flights  *Flights
	headline *Headlines
	token    string
	logoDir  string // folder of <IATA>.bin logo files; empty = no logos
	sem      chan struct{}
}

func floatParam(r *http.Request, name string, min, max float64, def *float64) (float64, error) {
	s := r.URL.Query().Get(name)
	if s == "" {
		if def != nil {
			return *def, nil
		}
		return 0, badRequest("missing parameter " + name)
	}
	v, err := strconv.ParseFloat(s, 64)
	if err != nil || v < min || v > max {
		return 0, badRequest(name + " must be a number between " + strconv.FormatFloat(min, 'f', -1, 64) + " and " + strconv.FormatFloat(max, 'f', -1, 64))
	}
	return v, nil
}

func intParam(r *http.Request, name string, min, max, def int) (int, error) {
	d := float64(def)
	v, err := floatParam(r, name, float64(min), float64(max), &d)
	return int(v), err
}

func strParam(r *http.Request, name, def string) string {
	if s := r.URL.Query().Get(name); s != "" {
		return s
	}
	return def
}

// wrap adds the common behaviour: token check, load limit, JSON output, errors without detail leaks.
func (a *app) wrap(h func(*http.Request) (any, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if a.token != "" && subtle.ConstantTimeCompare([]byte(r.Header.Get("X-Token")), []byte(a.token)) != 1 {
			writeJSON(w, http.StatusUnauthorized, map[string]string{"error": "missing or wrong token"})
			return
		}
		select {
		case a.sem <- struct{}{}:
			defer func() { <-a.sem }()
		default:
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": "busy"})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 12*time.Second)
		defer cancel()
		v, err := h(r.WithContext(ctx))
		if err != nil {
			var ae *apiError
			switch {
			case errors.As(err, &ae):
				writeJSON(w, ae.Status, map[string]string{"error": ae.Msg})
			case errors.Is(err, errOutsideCoverage):
				writeJSON(w, http.StatusUnprocessableEntity, map[string]string{"error": err.Error()})
			default:
				log.Printf("upstream error: %v", err)
				writeJSON(w, http.StatusBadGateway, map[string]string{"error": "the data source is not answering"})
			}
			return
		}
		writeJSON(w, http.StatusOK, v)
	}
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		status, b = http.StatusInternalServerError, []byte(`{"error":"internal error"}`)
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	w.Write(b)
}

func (a *app) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("ok")) })
	mux.HandleFunc("GET /v1/weather", a.wrap(func(r *http.Request) (any, error) {
		lat, err := floatParam(r, "lat", -90, 90, nil)
		if err != nil {
			return nil, err
		}
		lon, err := floatParam(r, "lon", -180, 180, nil)
		if err != nil {
			return nil, err
		}
		return a.weather.Get(r.Context(), lat, lon)
	}))
	mux.HandleFunc("GET /v1/flights", a.wrap(func(r *http.Request) (any, error) {
		var q FlightQuery
		var err error
		def15, def0, defMax := 15.0, 0.0, 60000.0
		if q.Lat, err = floatParam(r, "lat", -90, 90, nil); err != nil {
			return nil, err
		}
		if q.Lon, err = floatParam(r, "lon", -180, 180, nil); err != nil {
			return nil, err
		}
		if q.RadiusKm, err = floatParam(r, "radius_km", 1, 250, &def15); err != nil {
			return nil, err
		}
		amin, err := floatParam(r, "alt_min", 0, 60000, &def0)
		if err != nil {
			return nil, err
		}
		amax, err := floatParam(r, "alt_max", 0, 60000, &defMax)
		if err != nil {
			return nil, err
		}
		if amin > amax {
			return nil, badRequest("alt_min must not be above alt_max")
		}
		q.AltMin, q.AltMax = int(amin), int(amax)
		if q.N, err = intParam(r, "n", 1, 10, 5); err != nil {
			return nil, err
		}
		return a.flights.Get(r.Context(), q)
	}))
	mux.HandleFunc("GET /v1/headlines", a.wrap(func(r *http.Request) (any, error) {
		q := HeadlineQuery{
			Q:    r.URL.Query().Get("q"),
			HL:   strParam(r, "hl", "pt-PT"),
			GL:   strParam(r, "gl", "PT"),
			CEID: strParam(r, "ceid", "PT:pt-150"),
		}
		if l := len([]rune(q.Q)); l < 1 || l > 80 {
			return nil, badRequest("q must be 1 to 80 characters")
		}
		var err error
		if q.N, err = intParam(r, "n", 1, 5, 3); err != nil {
			return nil, err
		}
		return a.headline.Get(r.Context(), q)
	}))
	mux.HandleFunc("GET /v1/logo/{code}", a.logoHandler)
	return logRequests(mux)
}

// statusRecorder lets the logger see the status code.
type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (s *statusRecorder) WriteHeader(code int) { s.status = code; s.ResponseWriter.WriteHeader(code) }

// logRequests logs method, path, status and duration. The query string is deliberately left out:
// it carries the display's coordinates.
func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: 200}
		next.ServeHTTP(rec, r)
		log.Printf("%s %s %d %dms", r.Method, r.URL.Path, rec.status, time.Since(start).Milliseconds())
	})
}

func newApp(up *Upstream, token string) (*app, error) {
	cache := NewCache()
	w, err := NewWeather(cache, up)
	if err != nil {
		return nil, err
	}
	return &app{weather: w, flights: NewFlights(cache, up), headline: NewHeadlines(cache, up), token: token, sem: make(chan struct{}, 32)}, nil
}

func main() {
	a, err := newApp(NewUpstream(), os.Getenv("API_TOKEN"))
	if err != nil {
		log.Fatal(err)
	}
	a.logoDir = os.Getenv("LOGO_DIR")
	listen := os.Getenv("LISTEN")
	if listen == "" {
		listen = ":8080"
	}
	srv := &http.Server{Addr: listen, Handler: a.routes(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 10 * time.Second, WriteTimeout: 20 * time.Second, IdleTimeout: 60 * time.Second}
	go func() {
		stop := make(chan os.Signal, 1)
		signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
		<-stop
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		srv.Shutdown(ctx)
	}()
	log.Printf("ledmatrix-api listening on %s", listen)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
}
