package main

import (
	"bytes"
	"context"
	"fmt"
	"log"
	"math"
	"os"
	"strings"
	"testing"
	"time"
)

// Which provider answers which place (coverage.go), the outline (pt_outline.go) and the fall-through.

// points: real towns, chosen so that the lists do not depend on the outline's last kilometre, except the
// four border towns the lead named (Barrancos, Tui, Badajoz, Huelva), which are 0.5 to 7 km from the line.
type pin struct {
	name     string
	lat, lon float64
}

var ipmaPins = []pin{
	{"Lisbon", 38.7223, -9.1393}, {"Porto", 41.1496, -8.6109}, {"Faro", 37.0194, -7.9304}, {"Vila Real", 41.3006, -7.7441},
	{"Evora", 38.5714, -7.9135}, {"Coimbra", 40.2033, -8.4103}, {"Guarda", 40.5373, -7.2658}, {"Braganca", 41.8058, -6.7572},
	{"Viana do Castelo", 41.6918, -8.8344}, {"Lagos", 37.1028, -8.6730},
	{"Barrancos", 38.1333, -6.9792}, // 79 km from the nearest IPMA place, the farthest Portuguese town: why a distance rule fails
	{"Elvas", 38.8814, -7.1631}, {"Melgaco", 42.1140, -8.2580}, {"Alcoutim", 37.4717, -7.4750},
	// towns on the water front, which the 1:10m land polygon leaves in the water by a few hundred metres
	{"Viana do Castelo centre", 41.6918, -8.8344}, {"Figueira da Foz", 40.1508, -8.8618}, {"Cascais", 38.6979, -9.4215},
	{"Cais do Sodre", 38.7056, -9.1444}, {"Cacilhas", 38.6875, -9.1500}, {"Parque das Nacoes", 38.7680, -9.0950},
	{"Funchal", 32.6669, -16.9241}, {"Porto Santo", 33.0594, -16.3320},
	{"Ponta Delgada", 37.7412, -25.6756}, {"Angra do Heroismo", 38.6542, -27.2206}, {"Horta", 38.5333, -28.6333},
	{"Santa Cruz das Flores", 39.4546, -31.1291}, {"Vila do Porto", 36.9573, -25.1488},
}

var openMeteoPins = []pin{
	{"Vigo", 42.2406, -8.7207},
	{"Tui", 42.0467, -8.6444}, // 42 km from Viana do Castelo, but in Spain
	{"Ourense", 42.3358, -7.8639}, {"Salamanca", 40.9701, -5.6635}, {"Zamora", 41.5033, -5.7446}, {"Caceres", 39.4753, -6.3724},
	{"Ciudad Rodrigo", 40.5963, -6.5265}, {"A Guarda", 41.9020, -8.8740}, // on the Spanish coast, 3 km from the Minho mouth
	{"Badajoz", 38.8794, -6.9707}, {"Huelva", 37.2614, -6.9447}, {"Ayamonte", 37.2142, -7.4017},
	{"Seville", 37.3891, -5.9845}, {"Madrid", 40.4168, -3.7038}, {"Tangier", 35.7595, -5.8340}, {"Gibraltar", 36.14, -5.35},
	{"Tenerife", 28.4636, -16.2518}, {"Las Palmas", 28.1235, -15.4363},
	{"Selvagens", 30.14, -15.86}, // Portuguese, but no IPMA place and in no box
	{"Atlantic off Cascais", 38.7, -10.5}, {"Atlantic off Porto", 41.15, -8.75}, {"Atlantic west of Madeira", 32.7, -19.0},
	{"Tokyo", 35.6762, 139.6503}, {"Sao Paulo", -23.5505, -46.6333}, {"Moscow", 55.7558, 37.6173}, {"Kathmandu", 27.7172, 85.3240},
	{"Sydney", -33.8688, 151.2093}, {"Reykjavik", 64.1466, -21.9426}, {"mid-Atlantic", 0, -30},
	{"North Pole", 90, 0}, {"South Pole", -90, 0}, {"Kiritimati", 1.87, -157.4},
}

func TestWeatherChoosesProvider(t *testing.T) {
	s := newStub(t)
	for _, c := range []struct {
		src  string
		pins []pin
	}{{"IPMA", ipmaPins}, {"Open-Meteo", openMeteoPins}} {
		for _, p := range c.pins {
			s.aggHits.Store(0)
			s.omHits.Store(0)
			a := s.app(t, lisbonNoon(t)) // a new cache for every pin: the hit counters say who was asked
			out, err := a.weather.Get(context.Background(), p.lat, p.lon)
			if err != nil {
				t.Errorf("%s: %v", p.name, err)
				continue
			}
			ipma, om := s.aggHits.Load(), s.omHits.Load()
			switch c.src {
			case "IPMA":
				if out.Src != "IPMA" || out.Loc == "" || ipma == 0 || om != 0 {
					t.Errorf("%s: want IPMA, got src %q loc %q (IPMA hits %d, Open-Meteo hits %d)", p.name, out.Src, out.Loc, ipma, om)
				}
			default:
				if out.Src != "Open-Meteo" || out.Loc != "" || ipma != 0 || om != 1 {
					t.Errorf("%s: want Open-Meteo, got src %q loc %q (IPMA hits %d, Open-Meteo hits %d)", p.name, out.Src, out.Loc, ipma, om)
				}
			}
		}
	}
}

// The 100 km guard cannot be reached by a real Portuguese point, so the limit is a field: lowered to 10 km,
// Barrancos (79 km from Beja) goes to Open-Meteo.
func TestWeatherMaxPlaceGuard(t *testing.T) {
	s := newStub(t)
	a := s.app(t, lisbonNoon(t))
	if a.weather.maxPlaceKm != 100 || a.weather.maxStationKm != 50 {
		t.Fatalf("defaults: %v km from a place, %v km from a station", a.weather.maxPlaceKm, a.weather.maxStationKm)
	}
	pl, d := a.weather.nearestPlace(38.1333, -6.9792)
	if pl.Name != "Beja" || d < 70 || d > 90 {
		t.Fatalf("Barrancos is %.0f km from %s, the research says 79 km from Beja", d, pl.Name)
	}
	if !a.weather.ipmaCovers(38.1333, -6.9792) {
		t.Fatal("Barrancos is Portuguese and within 100 km of a place")
	}
	a.weather.maxPlaceKm = 10
	if a.weather.ipmaCovers(38.1333, -6.9792) || getWeather(t, a, 38.1333, -6.9792).Src != "Open-Meteo" {
		t.Fatal("with a 10 km guard Barrancos goes to Open-Meteo")
	}
	// the guard only ever takes places away: a point outside the outline is not covered however close the place
	a.weather.maxPlaceKm = 100000
	if a.weather.ipmaCovers(42.2406, -8.7207) {
		t.Fatal("Vigo is in Spain whatever the distance limit")
	}
}

func TestWeatherFallsThroughWhenIPMAFails(t *testing.T) {
	var logs bytes.Buffer
	log.SetOutput(&logs)
	defer log.SetOutput(os.Stderr)

	// IPMA down and nothing cached: Open-Meteo answers instead of a 502
	s := newStub(t)
	s.aggStatus.Store(500)
	a := s.app(t, lisbonNoon(t))
	out := getWeather(t, a, 38.7223, -9.1393)
	if out.Src != "Open-Meteo" || out.Loc != "" || s.aggHits.Load() == 0 || s.omHits.Load() != 1 {
		t.Fatalf("got %+v (IPMA hits %d, Open-Meteo hits %d)", out, s.aggHits.Load(), s.omHits.Load())
	}
	if !strings.Contains(logs.String(), "IPMA failed") || strings.Contains(logs.String(), "38.7") || strings.Contains(logs.String(), "-9.1") {
		t.Fatalf("log line wanted, without coordinates: %q", logs.String())
	}

	// IPMA answered once: a stale answer is kept for 6 hours, so IPMA still answers; after that Open-Meteo does
	s = newStub(t)
	a = s.app(t, lisbonNoon(t))
	clock := time.Unix(1795656000, 0)
	a.weather.cache.now = func() time.Time { return clock }
	if out = getWeather(t, a, 38.7223, -9.1393); out.Src != "IPMA" {
		t.Fatalf("first answer: %+v", out)
	}
	s.aggStatus.Store(500)
	clock = clock.Add(40 * time.Minute) // past the 30 minute TTL
	if out = getWeather(t, a, 38.7223, -9.1393); out.Src != "IPMA" || s.omHits.Load() != 0 {
		t.Fatalf("with a cached IPMA answer the display still gets IPMA: %+v (Open-Meteo hits %d)", out, s.omHits.Load())
	}
	clock = clock.Add(6 * time.Hour)
	if out = getWeather(t, a, 38.7223, -9.1393); out.Src != "Open-Meteo" {
		t.Fatalf("with IPMA's answer older than 6 hours: %+v", out)
	}

	// both down: an error, and not errNoWeather
	s = newStub(t)
	s.aggStatus.Store(500)
	s.omStatus.Store(500)
	a = s.app(t, lisbonNoon(t))
	if _, err := a.weather.Get(context.Background(), 38.7223, -9.1393); err == nil || err == errNoWeather {
		t.Fatalf("both providers down: %v", err)
	}
	// Open-Meteo has nothing for a pin outside Portugal and IPMA is not asked
	s = newStub(t)
	s.omStatus.Store(500)
	a = s.app(t, lisbonNoon(t))
	if _, err := a.weather.Get(context.Background(), 40.4168, -3.7038); err == nil || s.aggHits.Load() != 0 {
		t.Fatalf("Madrid with Open-Meteo down: %v, IPMA hits %d", err, s.aggHits.Load())
	}
}

func tempStr(p *int) string {
	if p == nil {
		return "none"
	}
	return fmt.Sprint(*p)
}

// The current temperature comes from an observation station only when it is within 50 km.
func TestIPMAObservationStationCap(t *testing.T) {
	s := newStub(t)
	a := s.app(t, lisbonNoon(t))
	st := a.weather.nearestStations(38.7223, -9.1393, 1)[0]
	d := haversineKm(38.7223, -9.1393, st.Lat, st.Lon)
	s.obsBody.Store(fmt.Sprintf(`{"2026-11-21T11:00":{"%s":{"temperatura":7.4}}}`, st.ID)) // the forecast says 15 or so
	forecast := *roundInt(fixtureTemp(t, "2026-11-21T12:00:00"))
	if forecast == 7 {
		t.Fatal("the fixture's forecast and the reading must differ")
	}
	a.weather.maxStationKm = d + 1
	if out := getWeather(t, a, 38.7223, -9.1393); out.T == nil || *out.T != 7 {
		t.Fatalf("station %.1f km away, cap %.1f km: got %s, want the reading 7", d, a.weather.maxStationKm, tempStr(out.T))
	}
	a = s.app(t, lisbonNoon(t))
	a.weather.maxStationKm = d - 0.01
	if out := getWeather(t, a, 38.7223, -9.1393); out.T == nil || *out.T != forecast {
		t.Fatalf("station %.1f km away, cap %.2f km: got %s, want the forecast %d", d, a.weather.maxStationKm, tempStr(out.T), forecast)
	}
	// the stations are tried nearest first and the cap ends the search: a far station is never used, a near one still is
	a = s.app(t, lisbonNoon(t))
	if out := getWeather(t, a, 38.7223, -9.1393); out.T == nil || *out.T != 7 || d > 50 {
		t.Fatalf("default cap: %s (station %.1f km away)", tempStr(out.T), d)
	}
}

// ---- the outline ------------------------------------------------------------------------------------------

func TestPortugalOutlineData(t *testing.T) {
	src, err := os.ReadFile("pt_outline.go")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"Code generated by tools/make_pt_outline.py; DO NOT EDIT.", "Natural Earth 1:10m", "ADM0_A3 = PRT", "public domain", "Retrieved 20", "SHA-256"} {
		if !strings.Contains(string(src), want) {
			t.Errorf("pt_outline.go header lacks %q", want)
		}
	}
	n := len(ptMainland)
	if n < 100 || n > 600 { // a few hundred points: simplified, but not a 26 point sketch
		t.Fatalf("%d points in the mainland outline", n)
	}
	if ptMainland[0] == ptMainland[n-1] {
		t.Error("the ring must not repeat its first point at the end")
	}
	minLat, maxLat, minLon, maxLon := 90.0, -90.0, 180.0, -180.0
	for _, p := range ptMainland {
		minLat, maxLat = min(minLat, p.Lat), max(maxLat, p.Lat)
		minLon, maxLon = min(minLon, p.Lon), max(maxLon, p.Lon)
	}
	// Portugal's mainland: Cabo da Roca to Quinta (lon -9.5), Vila Nova de Cerveira to Vila Real de Santo Antonio (lat 42.2 to 36.96)
	if minLat < 36.9 || minLat > 37.1 || maxLat < 42.1 || maxLat > 42.2 || minLon < -9.6 || minLon > -9.4 || maxLon < -6.3 || maxLon > -6.1 {
		t.Errorf("the mainland spans lat %.2f..%.2f lon %.2f..%.2f", minLat, maxLat, minLon, maxLon)
	}
	if !simpleRing(ptMainland) {
		t.Error("the outline crosses itself")
	}
	if len(ptIslandBoxes) != 2 || ptIslandBoxes[0].Name != "Madeira" || ptIslandBoxes[1].Name != "Azores" {
		t.Fatalf("boxes %+v", ptIslandBoxes)
	}
	for _, b := range ptIslandBoxes {
		if b.LatMin >= b.LatMax || b.LonMin >= b.LonMax {
			t.Errorf("box %+v", b)
		}
	}
	// no box takes in another country's land: the Canaries, Morocco, Gibraltar, Cape Verde, Iceland
	for _, p := range []pin{{"Tenerife", 28.4636, -16.2518}, {"Las Palmas", 28.1235, -15.4363}, {"Casablanca", 33.5731, -7.5898}, {"Cape Verde", 14.93, -23.51}, {"Reykjavik", 64.1466, -21.9426}} {
		for _, b := range ptIslandBoxes {
			if b.contains(p.lat, p.lon) {
				t.Errorf("the %s box holds %s", b.Name, p.name)
			}
		}
	}
	// the Selvagens are Portuguese but in no box (no IPMA place there)
	if inPortugal(30.14, -15.86) {
		t.Error("the Selvagens are not covered")
	}
}

// Every IPMA place is inside the outline or a box: a stale or broken outline fails here. The mainland places must be in
// the polygon itself (not in a box), the island places in the box of their group.
func TestEveryIPMAPlaceIsInsideTheOutline(t *testing.T) {
	a := newStub(t).app(t, lisbonNoon(t))
	var mainland, madeira, azores int
	for _, p := range a.weather.places {
		switch {
		case p.Lon < -24.5:
			azores++
			if !ptIslandBoxes[1].contains(p.Lat, p.Lon) || ptIslandBoxes[0].contains(p.Lat, p.Lon) {
				t.Errorf("%s (%.4f, %.4f) is not in the Azores box", p.Name, p.Lat, p.Lon)
			}
		case p.Lat < 33.5 && p.Lon < -16:
			madeira++
			if !ptIslandBoxes[0].contains(p.Lat, p.Lon) || ptIslandBoxes[1].contains(p.Lat, p.Lon) {
				t.Errorf("%s (%.4f, %.4f) is not in the Madeira box", p.Name, p.Lat, p.Lon)
			}
		default:
			mainland++
			if !inRing(ptMainland, p.Lat, p.Lon) && !nearCoast(ptMainland, p.Lat, p.Lon) {
				t.Errorf("%s (%.4f, %.4f) is not inside the mainland outline", p.Name, p.Lat, p.Lon)
			}
		}
		if !inPortugal(p.Lat, p.Lon) {
			t.Errorf("%s is not in Portugal", p.Name)
		}
	}
	if len(a.weather.places) != 35 || mainland < 20 || madeira < 1 || azores < 5 {
		t.Fatalf("%d places: %d mainland, %d Madeira, %d Azores", len(a.weather.places), mainland, madeira, azores)
	}
}

func TestInRingRayCast(t *testing.T) {
	// a concave "U" opening to the north, as (lat, lon): two arms (lon 0..3 and 6..9) on a base (lat 0..4);
	// the notch between the arms (lat 4..10, lon 3..6) is outside
	u := []outlinePoint{{0, 0, false}, {10, 0, false}, {10, 3, false}, {4, 3, false}, {4, 6, false}, {10, 6, false}, {10, 9, false}, {0, 9, false}}
	cases := []struct {
		lat, lon float64
		in       bool
	}{
		{5, 1, true}, {8, 1, true}, {8, 7, true}, {2, 4.5, true}, {1, 1, true}, // left arm, right arm, the base
		{5, 4.5, false}, {7, 4.5, false}, // the notch
		{5, -1, false}, {-1, 4, false}, {11, 4, false}, {20, 20, false},
	}
	for _, c := range cases {
		if got := inRing(u, c.lat, c.lon); got != c.in {
			t.Errorf("inRing(%v, %v) = %v, want %v", c.lat, c.lon, got, c.in)
		}
	}
	if inRing(nil, 1, 1) || inRing(u[:2], 1, 1) {
		t.Error("fewer than three points hold nothing")
	}
}

// The coast margin: water close to a coast edge counts, a point close to a border edge does not, and the nearest edge decides.
func TestNearCoastMargin(t *testing.T) {
	// a square of 0.2 degrees at the equator (about 22 km), clockwise as (lat, lon); each point's flag is for the edge to the next
	ring := []outlinePoint{
		{0, 0, false},    // south-west corner, then the west edge: coast
		{0.2, 0, false},  // north-west corner, then the north edge: coast
		{0.2, 0.2, true}, // north-east corner, then the east edge: border
		{0, 0.2, false},  // south-east corner, then the south edge: coast
	}
	const km = 1 / 111.195 // degrees per kilometre, of latitude and, at the equator, of longitude
	cases := []struct {
		name     string
		lat, lon float64
		inside   bool // inRing
		coast    bool // nearCoast
	}{
		{"inside", 0.1, 0.1, true, false},
		{"1 km off the west coast", 0.1, -1 * km, false, true},
		{"1.4 km off the north coast", 0.2 + 1.4*km, 0.1, false, true},
		{"1.6 km off the south coast", -1.6 * km, 0.1, false, false},
		{"1 km off the border", 0.1, 0.2 + 1*km, false, false}, // Tui: close, but the nearest edge is the border
		{"0.1 km off the border", 0.1, 0.2 + 0.1*km, false, false},
		{"far away", 5, 5, false, false},
		// near the corner where the border meets the coast the nearer edge decides
		{"1 km off the border, 1.5 km from the coast", 0.19, 0.2 + 1*km, false, false},
		{"1 km off the coast, 1.5 km from the border", 0.2 + 1*km, 0.19, false, true},
	}
	for _, c := range cases {
		if got := inRing(ring, c.lat, c.lon); got != c.inside {
			t.Errorf("%s: inRing = %v", c.name, got)
		}
		if got := nearCoast(ring, c.lat, c.lon); got != c.coast {
			t.Errorf("%s: nearCoast = %v, want %v", c.name, got, c.coast)
		}
	}
	if d := segmentKm(0, 0.1, ring[0], ring[1]); math.Abs(d-0.1/km) > 0.01 {
		t.Errorf("segmentKm to the west edge = %v km, want %v", d, 0.1/km)
	}
	if d := segmentKm(0.5, 0.5, ring[0], ring[0]); math.Abs(d-math.Hypot(0.5, 0.5)/km) > 0.1 { // a zero-length segment is a point
		t.Errorf("segmentKm to a point = %v", d)
	}
}

// The border towns are on their own side of the outline, also the one 0.5 km from the line.
func TestBorderTownsAreOnTheirOwnSide(t *testing.T) {
	for _, p := range []pin{{"Tui", 42.0467, -8.6444}, {"Badajoz", 38.8794, -6.9707}, {"Vigo", 42.2406, -8.7207}, {"Huelva", 37.2614, -6.9447}, {"Ayamonte", 37.2142, -7.4017}} {
		if inPortugal(p.lat, p.lon) {
			t.Errorf("%s is in Spain", p.name)
		}
	}
	for _, p := range []pin{{"Barrancos", 38.1333, -6.9792}, {"Elvas", 38.8814, -7.1631}, {"Vila Real de Santo Antonio", 37.1944, -7.4156}, {"Alcoutim", 37.4717, -7.4750}} {
		if !inPortugal(p.lat, p.lon) {
			t.Errorf("%s is in Portugal", p.name)
		}
	}
}

// simpleRing: no two edges of the closed ring cross (touching neighbours at their shared point is fine).
func simpleRing(r []outlinePoint) bool {
	n := len(r)
	cross := func(a, b, c outlinePoint) float64 { return (b.Lon-a.Lon)*(c.Lat-a.Lat) - (b.Lat-a.Lat)*(c.Lon-a.Lon) }
	for i := 0; i < n; i++ {
		a, b := r[i], r[(i+1)%n]
		for j := i + 2; j < n; j++ {
			if i == 0 && j == n-1 {
				continue
			}
			c, d := r[j], r[(j+1)%n]
			d1, d2, d3, d4 := cross(a, b, c), cross(a, b, d), cross(c, d, a), cross(c, d, b)
			if (d1 > 0) != (d2 > 0) && (d3 > 0) != (d4 > 0) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0 {
				return false
			}
		}
	}
	return true
}

func TestSimpleRingDetectsABowTie(t *testing.T) {
	if !simpleRing([]outlinePoint{{0, 0, false}, {0, 2, false}, {2, 2, false}, {2, 0, false}}) || simpleRing([]outlinePoint{{0, 0, false}, {2, 2, false}, {0, 2, false}, {2, 0, false}}) {
		t.Fatal("the helper does not tell a square from a bow tie")
	}
}
