package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// stub is a fake of every upstream, serving the real responses captured in testdata/.
type stub struct {
	srv        *httptest.Server
	obsBody    atomic.Value // string; observations.json body
	obsStatus  atomic.Int32
	adsbHits   atomic.Int32
	routeHits  atomic.Int32
	newsHits   atomic.Int32
	newsBody   atomic.Value // string
	aggHits    atomic.Int32
	adsbStatus atomic.Int32
}

func read(t *testing.T, name string) string {
	t.Helper()
	b, err := os.ReadFile("testdata/" + name)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func newStub(t *testing.T) *stub {
	s := &stub{}
	s.obsBody.Store(read(t, "observations_sample.json"))
	s.obsStatus.Store(200)
	s.adsbStatus.Store(200)
	s.newsBody.Store(rssFixture)
	mux := http.NewServeMux()
	mux.HandleFunc("/public-data/forecast/aggregate/1110600.json", func(w http.ResponseWriter, r *http.Request) {
		s.aggHits.Add(1)
		w.Write([]byte(read(t, "ipma_aggregate_1110600.json")))
	})
	mux.HandleFunc("/open-data/observation/meteorology/stations/observations.json", func(w http.ResponseWriter, r *http.Request) {
		if c := int(s.obsStatus.Load()); c != 200 {
			http.Error(w, "boom", c)
			return
		}
		w.Write([]byte(s.obsBody.Load().(string)))
	})
	mux.HandleFunc("/api/v3/", func(w http.ResponseWriter, r *http.Request) {
		s.adsbHits.Add(1)
		if c := int(s.adsbStatus.Load()); c != 200 {
			http.Error(w, "down", c)
			return
		}
		w.Write([]byte(read(t, "adsbfi_sample.json")))
	})
	mux.HandleFunc("/v0/callsign/", func(w http.ResponseWriter, r *http.Request) {
		s.routeHits.Add(1)
		if strings.HasSuffix(r.URL.Path, "/AFR188") {
			w.Write([]byte(read(t, "adsbdb_callsign_AFR188.json")))
			return
		}
		w.WriteHeader(404)
		w.Write([]byte(`{"response":"unknown callsign"}`))
	})
	mux.HandleFunc("/rss/search", func(w http.ResponseWriter, r *http.Request) {
		s.newsHits.Add(1)
		w.Write([]byte(s.newsBody.Load().(string)))
	})
	s.srv = httptest.NewServer(mux)
	t.Cleanup(s.srv.Close)
	return s
}

func (s *stub) app(t *testing.T, now time.Time) *app {
	t.Helper()
	cache := NewCache()
	up := NewUpstream()
	w, err := NewWeather(cache, up)
	if err != nil {
		t.Fatal(err)
	}
	w.base, w.now = s.srv.URL, func() time.Time { return now }
	f := NewFlights(cache, up)
	f.adsb, f.db = s.srv.URL, s.srv.URL
	h := NewHeadlines(cache, up)
	h.base = s.srv.URL
	return &app{weather: w, flights: f, headline: h, auth: testAuthenticator(t), firmware: newFirmware(t.TempDir(), time.Now, silent), sem: make(chan struct{}, 32)}
}

func lisbonNoon(t *testing.T) time.Time {
	loc, err := time.LoadLocation("Europe/Lisbon")
	if err != nil {
		t.Fatal(err)
	}
	return time.Date(2026, 11, 21, 12, 30, 0, 0, loc)
}

// ---- weather -------------------------------------------------------------------------------------------

// fixtureTemp reads tMed for a given local hour straight from the captured IPMA response.
func fixtureTemp(t *testing.T, hour string) float64 {
	t.Helper()
	var entries []ipmaEntry
	if err := json.Unmarshal([]byte(read(t, "ipma_aggregate_1110600.json")), &entries); err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if e.DataPrev == hour && e.Period == 1 {
			v, ok := ipmaNumber(e.TMed)
			if !ok {
				t.Fatalf("no temperature at %s", hour)
			}
			return v
		}
	}
	t.Fatalf("no hourly entry at %s", hour)
	return 0
}

func TestWeatherForLisbon(t *testing.T) {
	s := newStub(t)
	a := s.app(t, lisbonNoon(t))
	// nearest station has -99 in the newest hour but a valid reading earlier: the earlier one must be used
	near := a.weather.nearestStations(38.7223, -9.1393, 1)[0]
	s.obsBody.Store(fmt.Sprintf(`{"2026-11-21T10:00":{"%[1]s":{"temperatura":13.4}},"2026-11-21T11:00":{"%[1]s":{"temperatura":-99.0}}}`, near.ID))

	out, err := a.weather.Get(context.Background(), 38.7223, -9.1393)
	if err != nil {
		t.Fatal(err)
	}
	if out.Loc != "Lisboa" || out.Src != "IPMA" {
		t.Fatalf("got %+v", out)
	}
	if out.T == nil || *out.T != 13 {
		t.Fatalf("current temperature: got %v, want 13 (valid reading from the earlier hour)", out.T)
	}
	if out.Hi == nil || *out.Hi != 20 || out.Lo == nil || *out.Lo != 15 || out.Pop == nil || *out.Pop != 100 {
		t.Fatalf("today's high/low/rain: %v %v %v", out.Hi, out.Lo, out.Pop)
	}
	for hours, slot := range map[int]*Slot{3: out.H3, 6: out.H6} {
		if slot == nil {
			t.Fatalf("missing +%d h", hours)
		}
		want := fixtureTemp(t, fmt.Sprintf("2026-11-21T%02d:00:00", 12+hours))
		if slot.T != *roundInt(want) {
			t.Fatalf("+%d h: got %d want %.1f", hours, slot.T, want)
		}
		if slot.I != "rain" {
			t.Fatalf("+%d h icon %q, the captured forecast is rainy", hours, slot.I)
		}
	}
	b, _ := json.Marshal(out)
	if len(b) > 220 {
		t.Fatalf("answer is %d bytes, want a small payload: %s", len(b), b)
	}
}

func TestWeatherFallsBackToForecastWhenObservationsFail(t *testing.T) {
	s := newStub(t)
	s.obsStatus.Store(500)
	a := s.app(t, lisbonNoon(t))
	out, err := a.weather.Get(context.Background(), 38.7223, -9.1393)
	if err != nil {
		t.Fatal(err)
	}
	want := fixtureTemp(t, "2026-11-21T12:00:00")
	if out.T == nil || *out.T != *roundInt(want) {
		t.Fatalf("got %v want %.1f from the forecast", out.T, want)
	}
}

func TestWeatherOutsideCoverage(t *testing.T) {
	a := newStub(t).app(t, lisbonNoon(t))
	if _, err := a.weather.Get(context.Background(), 40.4168, -3.7038); !errors.Is(err, errOutsideCoverage) {
		t.Fatalf("Madrid should be outside IPMA coverage, got %v", err)
	}
}

func TestWeatherUsesForecastCacheAndIcons(t *testing.T) {
	s := newStub(t)
	a := s.app(t, lisbonNoon(t))
	for i := 0; i < 3; i++ {
		if _, err := a.weather.Get(context.Background(), 38.7223, -9.1393); err != nil {
			t.Fatal(err)
		}
	}
	if n := s.aggHits.Load(); n != 1 {
		t.Fatalf("forecast fetched %d times, want 1 (cached)", n)
	}
	cases := map[int]string{1: "sun", 22: "sun", 2: "part", 3: "part", 25: "part", 4: "cloud", 27: "cloud", 17: "cloud", 0: "cloud", 9: "rain", 6: "rain", 19: "rain", 18: "rain", 21: "rain"}
	for id, want := range cases {
		if got := iconFor(id); got != want {
			t.Errorf("iconFor(%d) = %s, want %s", id, got, want)
		}
	}
	if zoneFor(37.74, -25.67).String() != "Atlantic/Azores" || zoneFor(32.65, -16.91).String() != "Atlantic/Madeira" || zoneFor(38.7, -9.1).String() != "Europe/Lisbon" {
		t.Error("wrong time zone for the Azores, Madeira or the mainland")
	}
}

// ---- flights ---------------------------------------------------------------------------------------------

func TestFlightsRouteTypeAndFilters(t *testing.T) {
	s := newStub(t)
	a := s.app(t, time.Now())
	// centred on the Air France A350 in the fixture
	q := FlightQuery{Lat: 48.380251, Lon: 8.27444, RadiusKm: 10, AltMin: 0, AltMax: 60000, N: 5}
	out, err := a.flights.Get(context.Background(), q)
	if err != nil {
		t.Fatal(err)
	}
	if len(out.Planes) == 0 || out.Planes[0].ID != "39d2ab" {
		t.Fatalf("nearest plane should be the A350: %+v", out.Planes)
	}
	p := out.Planes[0]
	if p.CS != "AFR188" || p.Air != "Air France" || p.AI != "AF" || p.Route != "CDG-HKG" || p.From != "Paris" || p.To != "Hong Kong" {
		t.Fatalf("route data: %+v", p)
	}
	if p.Type != "A350-900" || p.TC != "A359" || p.Alt != 35175 || p.Spd != 597 || p.VR != 15 || p.Dst > 0.1 {
		t.Fatalf("aircraft data: %+v", p)
	}

	// planes on the ground are never returned
	all, _ := a.flights.Get(context.Background(), FlightQuery{Lat: 48.0, Lon: 10.0, RadiusKm: 250, AltMin: 0, AltMax: 60000, N: 10})
	for _, pl := range all.Planes {
		if pl.ID == "4b17fd" || pl.ID == "4b180c" {
			t.Fatalf("ground traffic returned: %+v", pl)
		}
	}
	// altitude window and count limit
	high, _ := a.flights.Get(context.Background(), FlightQuery{Lat: 48.0, Lon: 10.0, RadiusKm: 250, AltMin: 34000, AltMax: 60000, N: 10})
	for _, pl := range high.Planes {
		if pl.Alt < 34000 {
			t.Fatalf("below the altitude window: %+v", pl)
		}
	}
	two, _ := a.flights.Get(context.Background(), FlightQuery{Lat: 48.0, Lon: 10.0, RadiusKm: 250, AltMin: 0, AltMax: 60000, N: 2})
	if len(two.Planes) > 2 {
		t.Fatalf("N=2 returned %d planes", len(two.Planes))
	}
	if len(all.Planes) > 1 && all.Planes[0].Dst > all.Planes[1].Dst {
		t.Fatal("planes must be sorted nearest first")
	}
}

func TestFlightsCachesAndSurvivesUpstreamFailure(t *testing.T) {
	s := newStub(t)
	a := s.app(t, time.Now())
	q := FlightQuery{Lat: 48.0, Lon: 10.0, RadiusKm: 100, AltMin: 0, AltMax: 60000, N: 5}
	for i := 0; i < 3; i++ {
		if _, err := a.flights.Get(context.Background(), q); err != nil {
			t.Fatal(err)
		}
	}
	if n := s.adsbHits.Load(); n != 1 {
		t.Fatalf("positions fetched %d times, want 1 (15 s cache)", n)
	}
	calls := s.routeHits.Load()
	a.flights.Get(context.Background(), q)
	if s.routeHits.Load() != calls {
		t.Fatal("routes must be cached per callsign")
	}
	// a different place far away forces a new fetch; with the upstream down and nothing cached, that is an error
	s.adsbStatus.Store(503)
	if _, err := a.flights.Get(context.Background(), FlightQuery{Lat: 41.0, Lon: -8.0, RadiusKm: 15, AltMax: 60000, N: 5}); err == nil {
		t.Fatal("expected an error with the upstream down and no cached data")
	}
}

func TestShortType(t *testing.T) {
	cases := map[string]string{"AIRBUS A-350-900": "A350-900", "BOEING 737-800": "737-800", "EMBRAER E-175": "E-175", "ATR 72-600": "72-600", "": "", "CESSNA": "CESSNA"}
	for in, want := range cases {
		if got := shortType(in); got != want {
			t.Errorf("shortType(%q) = %q, want %q", in, got, want)
		}
	}
}

// ---- headlines -------------------------------------------------------------------------------------------

const rssFixture = `<?xml version="1.0" encoding="UTF-8"?><rss version="2.0"><channel><title>x</title>
<item><title>Old story - Jornal A</title><pubDate>Fri, 21 Nov 2026 08:00:00 GMT</pubDate><source url="https://a">Jornal A</source></item>
<item><title>Newest story - Jornal B</title><pubDate>Fri, 21 Nov 2026 21:30:00 GMT</pubDate><source url="https://b">Jornal B</source></item>
<item><title>Newest story! - Jornal C</title><pubDate>Fri, 21 Nov 2026 20:00:00 GMT</pubDate><source url="https://c">Jornal C</source></item>
<item><title>Middle story - Jornal D</title><pubDate>Fri, 21 Nov 2026 12:00:00 GMT</pubDate><source url="https://d">Jornal D</source></item>
<item><title>No date</title><source url="https://e">Jornal E</source></item>
</channel></rss>`

func TestHeadlinesNewestFirstDedupedAndTrimmed(t *testing.T) {
	s := newStub(t)
	a := s.app(t, time.Now())
	out, err := a.headline.Get(context.Background(), HeadlineQuery{Q: "tram", HL: "pt-PT", GL: "PT", CEID: "PT:pt-150", N: 3})
	if err != nil {
		t.Fatal(err)
	}
	if len(out.Items) != 3 {
		t.Fatalf("got %d items: %+v", len(out.Items), out.Items)
	}
	if out.Items[0].T != "Newest story" || out.Items[0].S != "Jornal B" {
		t.Fatalf("title/source trimming or ordering wrong: %+v", out.Items[0])
	}
	if out.Items[1].T != "Middle story" || out.Items[2].T != "Old story" {
		t.Fatalf("order, or the near-duplicate was not removed: %+v", out.Items)
	}
	a.headline.Get(context.Background(), HeadlineQuery{Q: "tram", HL: "pt-PT", GL: "PT", CEID: "PT:pt-150", N: 3})
	if s.newsHits.Load() != 1 {
		t.Fatal("headlines must be cached for 15 minutes")
	}
	if _, err := parseHeadlines([]byte("<html>consent</html>"), 3); err == nil {
		t.Fatal("a non-RSS answer must be an error")
	}
}

// ---- cache -----------------------------------------------------------------------------------------------

func TestCacheStaleOnErrorAndTTL(t *testing.T) {
	c := NewCache()
	now := time.Unix(1000, 0)
	c.now = func() time.Time { return now }
	loads := 0
	good := func() (any, error) { loads++; return "v1", nil }
	bad := func() (any, error) { return nil, errors.New("down") }
	if v, _ := c.Get("k", time.Minute, time.Hour, good); v != "v1" || loads != 1 {
		t.Fatal("first load")
	}
	c.Get("k", time.Minute, time.Hour, good)
	if loads != 1 {
		t.Fatal("must serve from cache inside the TTL")
	}
	now = now.Add(2 * time.Minute)
	if v, err := c.Get("k", time.Minute, time.Hour, bad); err != nil || v != "v1" {
		t.Fatal("must serve the stale value when the reload fails")
	}
	now = now.Add(3 * time.Hour)
	if _, err := c.Get("k", time.Minute, time.Hour, bad); err == nil {
		t.Fatal("a value older than the stale window must not be served")
	}
}

// ---- HTTP layer ------------------------------------------------------------------------------------------

func TestHTTPLayer(t *testing.T) {
	s := newStub(t)
	h := s.app(t, lisbonNoon(t)).routes()
	get := func(path string, signed bool) (int, string) {
		req := httptest.NewRequest("GET", path, nil)
		if signed {
			req = signedRequest(t, "GET", path, testNow, "")
		}
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec.Code, rec.Body.String()
	}
	if code, _ := get("/healthz", false); code != 200 {
		t.Fatalf("healthz: %d", code)
	}
	if code, _ := get("/v1/weather?lat=38.7&lon=-9.1", false); code != 401 {
		t.Fatalf("unsigned: %d", code)
	}
	if code, body := get("/v1/weather?lat=38.7&lon=-9.1", true); code != 200 || !strings.Contains(body, `"loc":"Lisboa"`) {
		t.Fatalf("weather: %d %s", code, body)
	}
	for _, bad := range []string{"/v1/weather?lat=abc&lon=1", "/v1/weather?lat=95&lon=1", "/v1/weather?lat=1", "/v1/flights?lat=1&lon=1&radius_km=999", "/v1/flights?lat=1&lon=1&alt_min=5000&alt_max=100", "/v1/headlines?q=", "/v1/headlines?q=x&n=99"} {
		if code, body := get(bad, true); code != 400 || !strings.Contains(body, "error") {
			t.Errorf("%s: want 400 with an error, got %d %s", bad, code, body)
		}
	}
	if code, _ := get("/v1/weather?lat=40.4168&lon=-3.7038", true); code != 422 {
		t.Errorf("outside coverage should be 422, got %d", code)
	}
	if code, body := get("/v1/flights?lat=48.38&lon=8.27&radius_km=15", true); code != 200 || !strings.Contains(body, `"planes"`) {
		t.Errorf("flights: %d %s", code, body)
	}
	s.adsbStatus.Store(500)
	if code, body := get("/v1/flights?lat=10&lon=10&radius_km=15", true); code != 502 || strings.Contains(body, "127.0.0.1") {
		t.Errorf("upstream failure should be 502 without leaking the upstream: %d %s", code, body)
	}
}

func TestLogos(t *testing.T) {
	s := newStub(t)
	a := s.app(t, lisbonNoon(t))
	dir := t.TempDir()
	good := make([]byte, logoBytes)
	good[0], good[511] = 0xF8, 0x1F
	os.WriteFile(filepath.Join(dir, "TP.bin"), good, 0o644)
	os.WriteFile(filepath.Join(dir, "XX.bin"), []byte("short"), 0o644)
	a.logoDir = dir
	h := a.routes()
	get := func(path string, signed bool) (int, []byte) {
		req := httptest.NewRequest("GET", path, nil)
		if signed {
			req = signedRequest(t, "GET", path, testNow, "")
		}
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec.Code, rec.Body.Bytes()
	}
	if code, b := get("/v1/logo/tp", true); code != 200 || len(b) != logoBytes || b[0] != 0xF8 {
		t.Fatalf("logo: %d, %d bytes", code, len(b))
	}
	if code, _ := get("/v1/logo/TP", false); code != 401 {
		t.Errorf("unsigned: %d", code)
	}
	for _, p := range []string{"/v1/logo/ZZ", "/v1/logo/XX", "/v1/logo/..", "/v1/logo/TAP", "/v1/logo/%2e%2e", "/v1/logo/T%2FP"} {
		if code, b := get(p, true); code == 200 || len(b) == logoBytes { // the mux may redirect a cleaned path; it must never serve one
			t.Errorf("%s: served a file (%d)", p, code)
		}
	}
	a.logoDir = ""
	if code, _ := get("/v1/logo/TP", true); code != 404 {
		t.Errorf("no logo folder: %d", code)
	}
}
