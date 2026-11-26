package main

import (
	"context"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"sort"
	"strconv"
	"time"
)

// Weather turns a weather provider's open data into a ~150 byte answer for the display. Inside
// Portugal that is IPMA (this file); everywhere else it is Open-Meteo (worldwide.go). Which one
// answers a location is decided in coverage.go and Get below.
//
// IPMA:
//
//	forecast: https://api.ipma.pt/public-data/forecast/aggregate/{globalIdLocal}.json
//	          71 hourly entries (idPeriodo 1), then 3-hourly (3) and daily (24). About 36 KB.
//	current:  https://api.ipma.pt/open-data/observation/meteorology/stations/observations.json
//	          every station for the last 24 h, about 842 KB (74 KB gzipped).
//
// The data is IPMA's: it must be cited as the source wherever it is shown. Open-Meteo's must be
// cited too (CC BY 4.0), see service/README.md.

//go:embed data/distrits-islands.json
var placesJSON []byte

//go:embed data/stations.json
var stationsJSON []byte

type place struct {
	ID       int
	Name     string
	Lat, Lon float64
}

type station struct {
	ID       string
	Name     string
	Lat, Lon float64
}

type Weather struct {
	cache    *Cache
	http     *Upstream
	base     string // IPMA
	omBase   string // Open-Meteo
	places   []place
	stations []station
	now      func() time.Time
	// maxPlaceKm: a point inside Portugal's outline is still sent to Open-Meteo when no IPMA forecast
	// place is this close. It only guards against a wrong outline (the farthest Portuguese point
	// from a place is Barrancos, 79 km). A field, not a constant, so a test can lower it.
	maxPlaceKm float64
	// maxStationKm: an IPMA observation station farther than this is not used for the current temperature.
	maxStationKm float64
}

// errNoWeather is the only weather error that is not a 502: the provider answered, with no value at all.
var errNoWeather = errors.New("no weather data for this location")

func NewWeather(cache *Cache, up *Upstream) (*Weather, error) {
	w := &Weather{cache: cache, http: up, base: "https://api.ipma.pt", omBase: "https://api.open-meteo.com", now: time.Now,
		maxPlaceKm: 100, maxStationKm: 50}
	var pl struct {
		Data []struct {
			ID    int    `json:"globalIdLocal"`
			Local string `json:"local"`
			Lat   string `json:"latitude"`
			Lon   string `json:"longitude"`
		} `json:"data"`
	}
	if err := json.Unmarshal(placesJSON, &pl); err != nil {
		return nil, fmt.Errorf("places: %w", err)
	}
	for _, p := range pl.Data {
		lat, e1 := strconv.ParseFloat(p.Lat, 64)
		lon, e2 := strconv.ParseFloat(p.Lon, 64)
		if e1 != nil || e2 != nil {
			continue
		}
		w.places = append(w.places, place{p.ID, p.Local, lat, lon})
	}
	var st []struct {
		Geometry struct {
			Coordinates [2]float64 `json:"coordinates"`
		} `json:"geometry"`
		Properties struct {
			ID   int    `json:"idEstacao"`
			Name string `json:"localEstacao"`
		} `json:"properties"`
	}
	if err := json.Unmarshal(stationsJSON, &st); err != nil {
		return nil, fmt.Errorf("stations: %w", err)
	}
	for _, s := range st {
		w.stations = append(w.stations, station{strconv.Itoa(s.Properties.ID), s.Properties.Name, s.Geometry.Coordinates[1], s.Geometry.Coordinates[0]})
	}
	if len(w.places) == 0 || len(w.stations) == 0 {
		return nil, errors.New("embedded IPMA tables are empty")
	}
	return w, nil
}

func (w *Weather) nearestPlace(lat, lon float64) (place, float64) {
	best, bd := w.places[0], math.MaxFloat64
	for _, p := range w.places {
		if d := haversineKm(lat, lon, p.Lat, p.Lon); d < bd {
			best, bd = p, d
		}
	}
	return best, bd
}

// nearestStations returns the n closest observation stations, nearest first.
func (w *Weather) nearestStations(lat, lon float64, n int) []station {
	type sd struct {
		s station
		d float64
	}
	all := make([]sd, len(w.stations))
	for i, s := range w.stations {
		all[i] = sd{s, haversineKm(lat, lon, s.Lat, s.Lon)}
	}
	sort.Slice(all, func(i, j int) bool { return all[i].d < all[j].d })
	if n > len(all) {
		n = len(all)
	}
	out := make([]station, n)
	for i := 0; i < n; i++ {
		out[i] = all[i].s
	}
	return out
}

// ---- IPMA payloads --------------------------------------------------------------------------------------

type ipmaEntry struct {
	DataPrev string `json:"dataPrev"`
	Period   int    `json:"idPeriodo"`
	Type     int    `json:"idTipoTempo"`
	TMed     string `json:"tMed"`
	TMin     string `json:"tMin"`
	TMax     string `json:"tMax"`
	Prob     string `json:"probabilidadePrecipita"`
}

type reading struct {
	Hour string // "2026-11-21T22:00"
	Temp float64
}

// ipmaNumber parses IPMA's numbers-as-strings. IPMA uses -99 for "no value".
func ipmaNumber(s string) (float64, bool) {
	v, err := strconv.ParseFloat(s, 64)
	if err != nil || v <= -90 {
		return 0, false
	}
	return v, true
}

func (w *Weather) forecast(ctx context.Context, id int) ([]ipmaEntry, error) {
	v, err := w.cache.Get(fmt.Sprintf("ipma-fc|%d", id), 30*time.Minute, 6*time.Hour, func() (any, error) {
		var entries []ipmaEntry
		if err := w.http.GetJSON(ctx, fmt.Sprintf("%s/public-data/forecast/aggregate/%d.json", w.base, id), &entries, 2<<20); err != nil {
			return nil, err
		}
		if len(entries) == 0 {
			return nil, errors.New("IPMA forecast is empty")
		}
		return entries, nil
	})
	if err != nil {
		return nil, err
	}
	return v.([]ipmaEntry), nil
}

// observations returns, per station, up to three recent valid temperatures, newest first.
func (w *Weather) observations(ctx context.Context) (map[string][]reading, error) {
	v, err := w.cache.Get("ipma-obs", 20*time.Minute, 3*time.Hour, func() (any, error) {
		var raw map[string]map[string]*struct {
			Temp *float64 `json:"temperatura"`
		}
		if err := w.http.GetJSON(ctx, w.base+"/open-data/observation/meteorology/stations/observations.json", &raw, 4<<20); err != nil {
			return nil, err
		}
		hours := make([]string, 0, len(raw))
		for h := range raw {
			hours = append(hours, h)
		}
		sort.Sort(sort.Reverse(sort.StringSlice(hours)))
		out := map[string][]reading{}
		for _, h := range hours {
			for sid, o := range raw[h] {
				if o == nil || o.Temp == nil || *o.Temp <= -90 || len(out[sid]) >= 3 {
					continue
				}
				out[sid] = append(out[sid], reading{h, *o.Temp})
			}
		}
		return out, nil
	})
	if err != nil {
		return nil, err
	}
	return v.(map[string][]reading), nil
}

// ---- answer ------------------------------------------------------------------------------------------------

type Slot struct {
	T int    `json:"t"`
	I string `json:"i"`
}

type WeatherOut struct {
	Loc  string `json:"loc"`
	T    *int   `json:"t,omitempty"`   // current temperature, °C
	Hi   *int   `json:"hi,omitempty"`  // today's high
	Lo   *int   `json:"lo,omitempty"`  // today's low
	Pop  *int   `json:"pop,omitempty"` // chance of rain today, %
	Icon string `json:"i"`             // sun | part | cloud | rain
	Code int    `json:"code"`          // the provider's own code for the current hour (IPMA idTipoTempo, WMO code for Open-Meteo)
	H3   *Slot  `json:"h3,omitempty"`  // in three hours
	H6   *Slot  `json:"h6,omitempty"`  // in six hours
	Src  string `json:"src"`
	AsOf string `json:"asof,omitempty"`
}

// iconFor maps IPMA's weather types onto the display's four icons. The display has no snow, fog or
// storm icons, so those fall to cloud (fog, mist) or rain (everything wet or stormy).
func iconFor(id int) string {
	switch id {
	case 1, 22:
		return "sun"
	case 2, 3, 5, 25:
		return "part"
	case 0, -99, 4, 16, 17, 24, 26, 27:
		return "cloud"
	default:
		return "rain"
	}
}

func zoneFor(lat, lon float64) *time.Location {
	name := "Europe/Lisbon"
	switch {
	case lon < -24.5:
		name = "Atlantic/Azores"
	case lat < 33.5 && lon < -16:
		name = "Atlantic/Madeira"
	}
	loc, err := time.LoadLocation(name)
	if err != nil {
		return time.UTC
	}
	return loc
}

func roundInt(v float64) *int { r := int(math.Round(v)); return &r }

// Get answers the weather at a point: from IPMA inside Portugal (ipmaCovers), otherwise from
// Open-Meteo. When IPMA fails and has nothing cached, not even a stale answer, Open-Meteo answers
// instead of a 502: the temperature may differ by a degree or two and the place name is empty,
// which the display handles.
func (w *Weather) Get(ctx context.Context, lat, lon float64) (*WeatherOut, error) {
	if w.ipmaCovers(lat, lon) {
		out, err := w.getIPMA(ctx, lat, lon)
		if err == nil {
			return out, nil
		}
		// the error names a host and a status at most, never a URL or coordinates
		log.Printf("weather: IPMA failed (%v), using Open-Meteo", err)
	}
	return w.getOpenMeteo(ctx, lat, lon)
}

func (w *Weather) getIPMA(ctx context.Context, lat, lon float64) (*WeatherOut, error) {
	pl, _ := w.nearestPlace(lat, lon)
	entries, err := w.forecast(ctx, pl.ID)
	if err != nil {
		return nil, err
	}
	loc := zoneFor(pl.Lat, pl.Lon)
	now := w.now().In(loc)
	hourNow := time.Date(now.Year(), now.Month(), now.Day(), now.Hour(), 0, 0, 0, loc)

	byTime := map[time.Time]ipmaEntry{} // hourly and 3-hourly entries
	var daily []ipmaEntry
	var firstHour time.Time
	for _, e := range entries {
		t, perr := time.ParseInLocation("2006-01-02T15:04:05", e.DataPrev, loc)
		if perr != nil {
			continue
		}
		switch e.Period {
		case 1, 3:
			byTime[t] = e
			if firstHour.IsZero() || t.Before(firstHour) {
				firstHour = t
			}
		case 24:
			daily = append(daily, e)
		}
	}
	cur, ok := byTime[hourNow]
	if !ok {
		// outside the hourly window: use the nearest entry we have
		var best time.Time
		for t := range byTime {
			if best.IsZero() || absDur(t.Sub(hourNow)) < absDur(best.Sub(hourNow)) {
				best = t
			}
		}
		if best.IsZero() {
			return nil, errors.New("IPMA forecast has no hourly entries")
		}
		cur, hourNow = byTime[best], best
	}

	out := &WeatherOut{Loc: pl.Name, Icon: iconFor(cur.Type), Code: cur.Type, Src: "IPMA"}
	slot := func(h int) *Slot {
		e, found := byTime[hourNow.Add(time.Duration(h)*time.Hour)]
		if !found {
			return nil
		}
		t, okT := ipmaNumber(e.TMed)
		if !okT {
			return nil
		}
		return &Slot{T: *roundInt(t), I: iconFor(e.Type)}
	}
	out.H3, out.H6 = slot(3), slot(6)

	today := now.Format("2006-01-02")
	for _, d := range daily {
		if len(d.DataPrev) >= 10 && d.DataPrev[:10] == today {
			if v, okv := ipmaNumber(d.TMax); okv {
				out.Hi = roundInt(v)
			}
			if v, okv := ipmaNumber(d.TMin); okv {
				out.Lo = roundInt(v)
			}
			if v, okv := ipmaNumber(d.Prob); okv {
				out.Pop = roundInt(v)
			}
		}
	}

	// current temperature: the nearest station (within maxStationKm) with a recent valid reading,
	// else the forecast for this hour
	if obs, oerr := w.observations(ctx); oerr == nil {
		for _, s := range w.nearestStations(lat, lon, 5) {
			if haversineKm(lat, lon, s.Lat, s.Lon) > w.maxStationKm {
				break // nearest first: the rest are farther
			}
			if rs := obs[s.ID]; len(rs) > 0 {
				out.T, out.AsOf = roundInt(rs[0].Temp), rs[0].Hour
				break
			}
		}
	}
	if out.T == nil {
		if v, okv := ipmaNumber(cur.TMed); okv {
			out.T, out.AsOf = roundInt(v), hourNow.Format("2006-01-02T15:04")
		}
	}
	return out, nil
}

func absDur(d time.Duration) time.Duration {
	if d < 0 {
		return -d
	}
	return d
}
