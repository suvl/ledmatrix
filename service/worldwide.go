package main

import (
	"context"
	"fmt"
	"math"
	"net/url"
	"time"
)

// Open-Meteo answers the weather outside Portugal (Get in weather.go chooses).
//
//	https://api.open-meteo.com/v1/forecast?latitude=&longitude=&current=temperature_2m,weather_code
//	    &hourly=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max
//	    &forecast_days=1&forecast_hours=12&timezone=auto&timeformat=unixtime
//
// One call of about 900 bytes gives every field the screens need. The data is Open-Meteo.com's, CC BY 4.0:
// it must be cited as the source wherever it is shown, and the free API is for non-commercial use
// only (service/README.md, wayfinder/research/worldwide-weather.md).
//
// Privacy: Open-Meteo keeps request logs with coordinates for up to 90 days, so the coordinates are
// rounded to ONE decimal (about 11 km, the forecast grid is of that order) before they are sent and
// before they become the cache key. Nothing that identifies the display is sent, and the
// User-Agent is the generic one.

// omResponse holds only the fields we use. Pointers, because Open-Meteo sends null for a value it
// does not have. With timeformat=unixtime every time is an exact instant (seconds since 1970).
type omResponse struct {
	Timezone  string `json:"timezone"`
	UTCOffset int    `json:"utc_offset_seconds"`
	Current   struct {
		Time int64    `json:"time"`
		Temp *float64 `json:"temperature_2m"`
		Code *int     `json:"weather_code"`
	} `json:"current"`
	Hourly struct {
		Time []int64    `json:"time"`
		Temp []*float64 `json:"temperature_2m"`
		Code []*int     `json:"weather_code"`
	} `json:"hourly"`
	Daily struct {
		Time []int64    `json:"time"` // the local midnight that starts the day
		Max  []*float64 `json:"temperature_2m_max"`
		Min  []*float64 `json:"temperature_2m_min"`
		Pop  []*float64 `json:"precipitation_probability_max"`
	} `json:"daily"`
}

// iconForWMO maps a WMO weather code onto the display's four icons: 0 and 1 sun, 2 part, 3 and the
// fog codes below 50 cloud, everything from 50 up rain. A range, not a table, so a code that a later
// version of the API adds lands in the right place by itself.
//
// There is no snow icon yet: snow (71-77, 85, 86) and sleet show as "Rain", and so do thunderstorms
// and hail. The light drizzle codes (51, 56) show as rain too, like IPMA's drizzle. A snow icon is
// a firmware change (art, screens, the renderer); the service starts sending "snow" only after it.
func iconForWMO(c int) string {
	switch {
	case c < 0:
		return "cloud" // not a WMO code: the caller's "missing" marker
	case c <= 1:
		return "sun" // 0 clear sky, 1 mainly clear
	case c == 2:
		return "part"
	case c < 50:
		return "cloud" // 3 overcast, 45 and 48 fog, anything unknown below 50
	default:
		return "rain" // 50-99: drizzle, rain, snow, showers, thunder
	}
}

// round1 rounds a coordinate to one decimal (about 11 km). Negative zero becomes zero, so the cache
// key of a point just south of the equator or west of Greenwich does not differ from its mirror.
func round1(v float64) float64 {
	v = math.Round(v*10) / 10
	if v == 0 {
		return 0
	}
	return v
}

// omQuery is the request for a rounded point (no key, no other parameter).
func omQuery(lat, lon float64) string {
	q := url.Values{}
	q.Set("latitude", fmt.Sprintf("%.1f", lat))
	q.Set("longitude", fmt.Sprintf("%.1f", lon))
	q.Set("current", "temperature_2m,weather_code")
	q.Set("hourly", "temperature_2m,weather_code")
	q.Set("daily", "temperature_2m_max,temperature_2m_min,precipitation_probability_max")
	// forecast_hours=12, not 7: the hourly list starts at the hour of the fetch, and a cached answer
	// is served for up to 15 minutes (3 hours when Open-Meteo fails), so by the time it is used the
	// current hour can be entry 1 to 4, and +6 h then needs entry 10 at most.
	q.Set("forecast_days", "1")
	q.Set("forecast_hours", "12")
	q.Set("timezone", "auto")
	q.Set("timeformat", "unixtime")
	return q.Encode()
}

// openMeteo returns Open-Meteo's answer for a point, cached per rounded point: fresh for 15
// minutes (the board asks every 15 minutes and Open-Meteo's current value has a 15 minute step), a
// stale one up to 3 hours old when Open-Meteo is not answering. Two displays in the same city
// share one call.
func (w *Weather) openMeteo(ctx context.Context, lat, lon float64) (*omResponse, error) {
	rlat, rlon := round1(lat), round1(lon)
	v, err := w.cache.Get(fmt.Sprintf("om|%.1f|%.1f", rlat, rlon), 15*time.Minute, 3*time.Hour, func() (any, error) {
		var r omResponse
		if err := w.http.GetJSON(ctx, w.omBase+"/v1/forecast?"+omQuery(rlat, rlon), &r, 64<<10); err != nil {
			return nil, err
		}
		return &r, nil
	})
	if err != nil {
		return nil, err
	}
	return v.(*omResponse), nil
}

func (w *Weather) getOpenMeteo(ctx context.Context, lat, lon float64) (*WeatherOut, error) {
	r, err := w.openMeteo(ctx, lat, lon)
	if err != nil {
		return nil, err
	}
	return r.toOut(w.now())
}

// zone is the location's time zone: its name (the distroless image has the time/tzdata package
// compiled in, see main.go), else the fixed offset of the answer.
func (r *omResponse) zone() *time.Location {
	// LoadLocation("") is UTC and LoadLocation("Local") is the server's zone: neither is the place's
	if r.Timezone != "" && r.Timezone != "Local" {
		if loc, err := time.LoadLocation(r.Timezone); err == nil {
			return loc
		}
	}
	return time.FixedZone("", r.UTCOffset)
}

// toOut turns an answer into the display's JSON at the moment now. The location name is empty:
// Open-Meteo has none, and the display shows "WEATHER" instead. The temperatures are whole degrees.
//
// The hours are found by time, never by index. Base is the last hourly entry at or before now (the
// cached answer may be a few hours old, and Kathmandu's entries are 15 minutes past a UTC hour);
// +3 h and +6 h are the entries whose instants are base+3 h and base+6 h. The day's high, low and
// chance of rain are used only while now is inside the answer's local day, so an answer cached
// across midnight shows none instead of yesterday's.
func (r *omResponse) toOut(now time.Time) (*WeatherOut, error) {
	loc := r.zone()
	out := &WeatherOut{Loc: "", Icon: "cloud", Code: -1, Src: "Open-Meteo"}
	if r.Current.Time != 0 {
		out.AsOf = time.Unix(r.Current.Time, 0).In(loc).Format("2006-01-02T15:04")
	}
	if r.Current.Temp != nil {
		out.T = roundInt(*r.Current.Temp)
	}
	if r.Current.Code != nil {
		out.Icon, out.Code = iconForWMO(*r.Current.Code), *r.Current.Code
	}

	if h := r.Hourly; len(h.Time) > 0 {
		base := -1
		for i, t := range h.Time {
			if t <= now.Unix() {
				base = i
			}
		}
		if base >= 0 {
			slot := func(hours int64) *Slot {
				want := h.Time[base] + hours*3600
				for i, t := range h.Time {
					if t != want {
						continue
					}
					if i >= len(h.Temp) || h.Temp[i] == nil {
						return nil
					}
					s := &Slot{T: *roundInt(*h.Temp[i]), I: "cloud"}
					if i < len(h.Code) && h.Code[i] != nil {
						s.I = iconForWMO(*h.Code[i])
					}
					return s
				}
				return nil
			}
			out.H3, out.H6 = slot(3), slot(6)
		}
	}

	if d := r.Daily; len(d.Time) > 0 {
		start := time.Unix(d.Time[0], 0).In(loc)
		if !now.Before(start) && now.Before(start.AddDate(0, 0, 1)) { // AddDate: the next local midnight, also on a DST day
			pick := func(v []*float64) *int {
				if len(v) == 0 || v[0] == nil {
					return nil
				}
				return roundInt(*v[0])
			}
			out.Hi, out.Lo, out.Pop = pick(d.Max), pick(d.Min), pick(d.Pop)
		}
	}

	if out.T == nil && out.Hi == nil && out.Lo == nil && out.Pop == nil && out.H3 == nil && out.H6 == nil && r.Current.Code == nil {
		return nil, errNoWeather
	}
	return out, nil
}
