package main

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// Flights answers "which planes are near me", merged from:
//
//	positions: adsb.fi open data (keyless, personal and non-commercial use, credit adsb.fi)
//	routes:    adsbdb.com, by callsign (community data: wrong or missing for charter, cargo,
//	           general aviation and military flights, so a missing route is simply left out)
//
// Coordinates are rounded to two decimals (about 1 km) before they leave for the upstream, which
// both limits what the upstream learns about where the display is and lets nearby requests share
// one cached answer.
type Flights struct {
	cache *Cache
	http  *Upstream
	adsb  string
	db    string
	now   func() time.Time
}

func NewFlights(cache *Cache, up *Upstream) *Flights {
	return &Flights{cache: cache, http: up, adsb: "https://opendata.adsb.fi", db: "https://api.adsbdb.com", now: time.Now}
}

type FlightQuery struct {
	Lat, Lon float64
	RadiusKm float64
	AltMin   int
	AltMax   int
	N        int
}

type Plane struct {
	ID    string  `json:"id"`
	CS    string  `json:"cs,omitempty"`  // callsign
	Reg   string  `json:"reg,omitempty"` // registration
	Air   string  `json:"air,omitempty"` // airline name
	AI    string  `json:"ai,omitempty"`  // airline IATA code (also the logo key)
	Route string  `json:"rt,omitempty"`  // origin-destination, IATA
	From  string  `json:"fr,omitempty"`  // origin city
	To    string  `json:"to,omitempty"`  // destination city
	Type  string  `json:"ty,omitempty"`  // short aircraft name
	TC    string  `json:"tc,omitempty"`  // ICAO type code
	Alt   int     `json:"alt"`           // feet
	Spd   int     `json:"spd"`           // knots
	Trk   int     `json:"trk"`           // degrees
	VR    int     `json:"vr"`            // feet per second
	Dst   float64 `json:"dst"`           // kilometres from the display
}

type FlightsOut struct {
	AsOf   int64   `json:"asof"`
	Planes []Plane `json:"planes"`
	Src    string  `json:"src"`
}

type adsbAircraft struct {
	Hex      string          `json:"hex"`
	Flight   string          `json:"flight"`
	Reg      string          `json:"r"`
	TypeCode string          `json:"t"`
	Desc     string          `json:"desc"`
	AltBaro  json.RawMessage `json:"alt_baro"`
	GS       float64         `json:"gs"`
	Track    float64         `json:"track"`
	BaroRate float64         `json:"baro_rate"`
	Lat      *float64        `json:"lat"`
	Lon      *float64        `json:"lon"`
}

type adsbReply struct {
	Aircraft []adsbAircraft `json:"ac"`
}

func (f *Flights) positions(ctx context.Context, q FlightQuery) ([]adsbAircraft, error) {
	nm := int(math.Ceil((q.RadiusKm + 1.5) / kmPerNM))
	if nm > 250 {
		nm = 250
	}
	lat, lon := math.Round(q.Lat*100)/100, math.Round(q.Lon*100)/100
	key := fmt.Sprintf("pos|%.2f|%.2f|%d", lat, lon, nm)
	v, err := f.cache.Get(key, 15*time.Second, 2*time.Minute, func() (any, error) {
		var r adsbReply
		if err := f.http.GetJSON(ctx, fmt.Sprintf("%s/api/v3/lat/%.2f/lon/%.2f/dist/%d", f.adsb, lat, lon, nm), &r, 1<<20); err != nil {
			return nil, err
		}
		return r.Aircraft, nil
	})
	if err != nil {
		return nil, err
	}
	return v.([]adsbAircraft), nil
}

var manufacturers = []string{"AIRBUS ", "BOEING ", "EMBRAER ", "BOMBARDIER ", "DE HAVILLAND ", "CESSNA ", "PIPER ", "ATR ", "SAAB ", "DORNIER ", "FOKKER ", "MCDONNELL DOUGLAS ", "GULFSTREAM AEROSPACE ", "DASSAULT ", "PILATUS ", "BEECH ", "LEARJET "}
var airbusDash = regexp.MustCompile(`\b([AB])-(\d)`)

// shortType turns "AIRBUS A-350-900" into "A350-900".
func shortType(desc string) string {
	s := strings.ToUpper(strings.TrimSpace(desc))
	for _, m := range manufacturers {
		if strings.HasPrefix(s, m) && len(s) > len(m) {
			s = s[len(m):]
			break
		}
	}
	return airbusDash.ReplaceAllString(s, "$1$2")
}

func altitude(raw json.RawMessage) (int, bool) {
	var n float64
	if err := json.Unmarshal(raw, &n); err != nil {
		return 0, false // "ground" or missing
	}
	return int(n), true
}

type route struct {
	Airline, AirlineIATA, RouteCode, From, To string
}

type dbReply struct {
	Response json.RawMessage `json:"response"`
}

type dbFlightRoute struct {
	Flightroute struct {
		Airline struct {
			Name string `json:"name"`
			IATA string `json:"iata"`
		} `json:"airline"`
		Origin struct {
			IATA string `json:"iata_code"`
			City string `json:"municipality"`
		} `json:"origin"`
		Destination struct {
			IATA string `json:"iata_code"`
			City string `json:"municipality"`
		} `json:"destination"`
	} `json:"flightroute"`
}

var callsignShape = regexp.MustCompile(`^[A-Z]{3}[0-9][0-9A-Z]{0,3}$`)

// route looks a callsign up. Unknown callsigns are remembered for an hour so they are not asked again.
func (f *Flights) route(ctx context.Context, callsign string) *route {
	if !callsignShape.MatchString(callsign) {
		return nil
	}
	ttl := 12 * time.Hour
	v, err := f.cache.Get("route|"+callsign, ttl, 48*time.Hour, func() (any, error) {
		b, err := f.http.GetBytes(ctx, f.db+"/v0/callsign/"+callsign, 64<<10)
		if err != nil {
			var se *statusError
			if asStatus(err, &se) && se.Code == 404 {
				return (*route)(nil), nil
			}
			return nil, err
		}
		var rep dbReply
		if json.Unmarshal(b, &rep) != nil {
			return nil, fmt.Errorf("adsbdb: unexpected response")
		}
		var fr dbFlightRoute
		if json.Unmarshal(rep.Response, &fr) != nil || fr.Flightroute.Airline.Name == "" && fr.Flightroute.Origin.IATA == "" {
			return (*route)(nil), nil // "unknown callsign"
		}
		r := fr.Flightroute
		out := &route{Airline: r.Airline.Name, AirlineIATA: r.Airline.IATA, From: r.Origin.City, To: r.Destination.City}
		if r.Origin.IATA != "" && r.Destination.IATA != "" {
			out.RouteCode = r.Origin.IATA + "-" + r.Destination.IATA
		}
		return out, nil
	})
	if err != nil {
		return nil
	}
	return v.(*route)
}

func (f *Flights) Get(ctx context.Context, q FlightQuery) (*FlightsOut, error) {
	ac, err := f.positions(ctx, q)
	if err != nil {
		return nil, err
	}
	type cand struct {
		a    adsbAircraft
		alt  int
		dist float64
	}
	var cands []cand
	for _, a := range ac {
		if a.Lat == nil || a.Lon == nil {
			continue
		}
		alt, ok := altitude(a.AltBaro)
		if !ok || alt < q.AltMin || alt > q.AltMax {
			continue
		}
		d := haversineKm(q.Lat, q.Lon, *a.Lat, *a.Lon)
		if d > q.RadiusKm {
			continue
		}
		cands = append(cands, cand{a, alt, d})
	}
	sort.Slice(cands, func(i, j int) bool { return cands[i].dist < cands[j].dist })
	if len(cands) > q.N {
		cands = cands[:q.N]
	}

	planes := make([]Plane, len(cands))
	rctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	var wg sync.WaitGroup
	for i, c := range cands {
		cs := strings.ToUpper(strings.TrimSpace(c.a.Flight))
		planes[i] = Plane{
			ID: c.a.Hex, CS: cs, Reg: c.a.Reg, Type: shortType(c.a.Desc), TC: c.a.TypeCode,
			Alt: c.alt, Spd: int(math.Round(c.a.GS)), Trk: int(math.Round(c.a.Track)),
			VR: int(math.Round(c.a.BaroRate / 60)), Dst: math.Round(c.dist*10) / 10,
		}
		wg.Add(1)
		go func(i int, cs string) {
			defer wg.Done()
			if r := f.route(rctx, cs); r != nil {
				planes[i].Air, planes[i].AI, planes[i].Route, planes[i].From, planes[i].To = r.Airline, r.AirlineIATA, r.RouteCode, r.From, r.To
			}
		}(i, cs)
	}
	wg.Wait()
	return &FlightsOut{AsOf: f.now().Unix(), Planes: planes, Src: "adsb.fi, adsbdb"}, nil
}
