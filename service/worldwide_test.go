package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"net/http/httptest"
	"net/url"
	"os"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"
)

// Tests of the worldwide weather: Open-Meteo outside Portugal, and the rule that chooses between it and IPMA.
// The recorded answers are real (testdata/openmeteo_*.json, see testdata/README.md).

// clock0 is the service clock of the recorded answers' expected JSON: 2026-11-26 01:20:00 UTC.
func clock0() time.Time { return time.Unix(1795656000, 0).UTC() }

func omFixture(t *testing.T, name string) omResponse {
	t.Helper()
	var r omResponse
	if err := json.Unmarshal([]byte(read(t, name)), &r); err != nil {
		t.Fatal(err)
	}
	return r
}

func mustZone(t *testing.T, name string) *time.Location {
	t.Helper()
	loc, err := time.LoadLocation(name)
	if err != nil {
		t.Fatal(err)
	}
	return loc
}

func marshal(t *testing.T, v any) string {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func getWeather(t *testing.T, a *app, lat, lon float64) *WeatherOut {
	t.Helper()
	out, err := a.weather.Get(context.Background(), lat, lon)
	if err != nil {
		t.Fatalf("weather at %.4f,%.4f: %v", lat, lon, err)
	}
	return out
}

func TestClock0(t *testing.T) {
	if got := time.Date(2026, 11, 26, 1, 20, 0, 0, time.UTC); !got.Equal(clock0()) {
		t.Fatalf("clock0 is %v", clock0())
	}
}

// ---- the recorded answers ---------------------------------------------------------------------------------

func TestOpenMeteoRecordedAnswers(t *testing.T) {
	cases := []struct {
		name, file string
		lat, lon   float64
		want       string
	}{
		{"Madrid", "openmeteo_madrid.json", 40.4168, -3.7038,
			`{"loc":"","t":6,"hi":11,"lo":3,"pop":0,"i":"sun","code":1,"h3":{"t":5,"i":"sun"},"h6":{"t":3,"i":"sun"},"src":"Open-Meteo","asof":"2026-11-26T02:00"}`},
		// -3.5 in the +3 h slot is rounded away from zero to -4; snow (71) shows as rain
		{"Moscow", "openmeteo_moscow.json", 55.7558, 37.6173,
			`{"loc":"","t":-4,"hi":-1,"lo":-5,"pop":90,"i":"cloud","code":3,"h3":{"t":-4,"i":"rain"},"h6":{"t":-2,"i":"rain"},"src":"Open-Meteo","asof":"2026-11-26T04:00"}`},
		// +5:45: the entries are 15 minutes past a UTC hour; the base is the 07:00 local entry (index 1)
		{"Kathmandu", "openmeteo_kathmandu.json", 27.7172, 85.3240,
			`{"loc":"","t":8,"hi":21,"lo":7,"pop":0,"i":"cloud","code":45,"h3":{"t":19,"i":"sun"},"h6":{"t":21,"i":"sun"},"src":"Open-Meteo","asof":"2026-11-26T06:45"}`},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			s := newStub(t)
			s.omBody.Store(read(t, c.file))
			a := s.app(t, clock0())
			got := marshal(t, getWeather(t, a, c.lat, c.lon))
			if got != c.want {
				t.Fatalf("got  %s\nwant %s", got, c.want)
			}
			if len(got) >= 220 {
				t.Fatalf("answer is %d bytes, the display wants a small payload", len(got))
			}
		})
	}
}

func TestOpenMeteoRequestShape(t *testing.T) {
	s := newStub(t)
	a := s.app(t, clock0())
	getWeather(t, a, 40.4168, -3.7038)
	queries, agents := s.omQueryLog()
	if len(queries) != 1 {
		t.Fatalf("%d requests", len(queries))
	}
	q, err := url.ParseQuery(queries[0])
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for k, v := range q {
		if len(v) != 1 {
			t.Errorf("parameter %s sent %d times", k, len(v))
		}
		got[k] = v[0]
	}
	want := map[string]string{
		"latitude": "40.4", "longitude": "-3.7", // ONE decimal, about 11 km: Open-Meteo keeps its logs for 90 days
		"current":       "temperature_2m,weather_code",
		"hourly":        "temperature_2m,weather_code",
		"daily":         "temperature_2m_max,temperature_2m_min,precipitation_probability_max",
		"forecast_days": "1", "forecast_hours": "12", // 12, not 7: the cached answer is used for hours after the fetch
		"timezone": "auto", "timeformat": "unixtime", // exact instants; the zone is resolved by Open-Meteo
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("query\n got %v\nwant %v", got, want) // also proves: no apikey, no other coordinates, nothing else
	}
	if strings.Contains(queries[0], "4168") || strings.Contains(queries[0], "7038") {
		t.Fatalf("the exact coordinates went out: %s", queries[0])
	}
	// the user agent stays the generic one: no contact details are needed for Open-Meteo
	ua := NewUpstream().ua
	if len(agents) != 1 || agents[0] != ua || strings.ContainsAny(ua, "@") || strings.Contains(ua, "http") {
		t.Fatalf("user agent %q (service default %q)", agents, ua)
	}
}

func TestRound1(t *testing.T) {
	cases := []struct{ in, want float64 }{
		{40.4168, 40.4}, {-3.7038, -3.7}, {38.7223, 38.7}, {-9.1393, -9.1}, {27.7172, 27.7}, {85.3240, 85.3},
		{0.04, 0}, {-0.04, 0}, {math.Copysign(0, -1), 0}, {90, 90}, {-90, -90}, {180, 180}, {179.96, 180}, {-179.96, -180}, {89.96, 90},
		{0.05, 0.1}, {-0.05, -0.1}, {-33.8688, -33.9}, {151.2093, 151.2},
	}
	for _, c := range cases {
		if got := round1(c.in); got != c.want {
			t.Errorf("round1(%v) = %v, want %v", c.in, got, c.want)
		}
	}
	// no "-0.0" in the cache key or in the request
	if s := fmt.Sprintf("%.1f", round1(-0.04)); s != "0.0" || math.Signbit(round1(-0.04)) {
		t.Errorf("negative zero leaks: %s", s)
	}
}

// Every point of the world is answered, the rounding is applied to all of them and the poles and the date line are in range.
func TestOpenMeteoAnswersEveryPlace(t *testing.T) {
	places := []struct {
		name             string
		lat, lon         float64
		sentLat, sentLon string
	}{
		{"Madrid", 40.4168, -3.7038, "40.4", "-3.7"},
		{"Moscow", 55.7558, 37.6173, "55.8", "37.6"},
		{"Kathmandu", 27.7172, 85.3240, "27.7", "85.3"},
		{"Sydney", -33.8688, 151.2093, "-33.9", "151.2"},
		{"Reykjavik", 64.1466, -21.9426, "64.1", "-21.9"},
		{"mid-Atlantic", 0, -30, "0.0", "-30.0"},
		{"south of the equator, west of Greenwich", -0.04, -0.04, "0.0", "0.0"},
		{"north pole", 90, 0, "90.0", "0.0"},
		{"south pole", -90, 0, "-90.0", "0.0"},
		{"date line east", -17.7, 180, "-17.7", "180.0"},
		{"date line west", -17.7, -180, "-17.7", "-180.0"},
		{"Kiritimati", 1.87, -157.4, "1.9", "-157.4"},
	}
	for _, p := range places {
		t.Run(p.name, func(t *testing.T) {
			s := newStub(t)
			a := s.app(t, clock0())
			out := getWeather(t, a, p.lat, p.lon)
			if out.Src != "Open-Meteo" || out.Loc != "" {
				t.Fatalf("got %+v", out)
			}
			queries, _ := s.omQueryLog()
			q, _ := url.ParseQuery(queries[0])
			if q.Get("latitude") != p.sentLat || q.Get("longitude") != p.sentLon {
				t.Fatalf("sent %s,%s want %s,%s", q.Get("latitude"), q.Get("longitude"), p.sentLat, p.sentLon)
			}
			if !regexp.MustCompile(`^-?\d+\.\d$`).MatchString(q.Get("latitude")) || !regexp.MustCompile(`^-?\d+\.\d$`).MatchString(q.Get("longitude")) {
				t.Fatalf("more than one decimal sent: %s", queries[0])
			}
		})
	}
}

// ---- hours and days ---------------------------------------------------------------------------------------

func slotTemp(s *Slot) string {
	if s == nil {
		return "none"
	}
	return fmt.Sprintf("%d%s", s.T, s.I[:1])
}

func TestOpenMeteoHoursByTimeNotIndex(t *testing.T) {
	kath, moscow, madrid := omFixture(t, "openmeteo_kathmandu.json"), omFixture(t, "openmeteo_moscow.json"), omFixture(t, "openmeteo_madrid.json")
	at := func(r omResponse, unix int64) *WeatherOut {
		out, err := r.toOut(time.Unix(unix, 0))
		if err != nil {
			t.Fatal(err)
		}
		return out
	}
	// Kathmandu +5:45: the entries are on the local hour, 00:15 UTC, 01:15 UTC, ...
	// 06:50 local (01:05 UTC): base 1795652100 (06:00 local), +3 h is entry 3 (17.1), +6 h entry 6 (20.6)
	o := at(kath, 1795655100)
	if slotTemp(o.H3) != "17s" || slotTemp(o.H6) != "21s" {
		t.Errorf("Kathmandu 06:50: %s %s", slotTemp(o.H3), slotTemp(o.H6))
	}
	// 07:05 local (01:20 UTC): base 1795655700, +3 h is entry 4 (18.7)
	o = at(kath, 1795656000)
	if slotTemp(o.H3) != "19s" || slotTemp(o.H6) != "21s" {
		t.Errorf("Kathmandu 07:05: %s %s", slotTemp(o.H3), slotTemp(o.H6))
	}
	// 06:00 sharp is still the first entry; 05:59 is before the answer's first hour: no slots, no guessing
	if o = at(kath, 1795652100); slotTemp(o.H3) != "17s" {
		t.Errorf("exactly on the first entry: %s", slotTemp(o.H3))
	}
	if o = at(kath, 1795652099); o.H3 != nil || o.H6 != nil {
		t.Errorf("before the first entry: %+v %+v", o.H3, o.H6)
	}
	// Moscow at 02:05 UTC: the base is entry 1 (02:00 UTC), so +3 h is entry 4 (-3.2) and +6 h is entry 7 (-1.7); from index 0 it would be -3.5 and -2.2
	o = at(moscow, 1795654800+3600+300)
	if slotTemp(o.H3) != "-3r" || slotTemp(o.H6) != "-2r" {
		t.Errorf("Moscow 02:05: %s %s", slotTemp(o.H3), slotTemp(o.H6))
	}
	// the window of 12 entries: base 5 still has +6 h (entry 11), base 6 has +3 h but no +6 h
	o = at(madrid, 1795654800+5*3600+60)
	if o.H3 == nil || o.H6 == nil {
		t.Errorf("Madrid base 5: %v %v", o.H3, o.H6)
	}
	o = at(madrid, 1795654800+6*3600+60)
	if o.H3 == nil || o.H6 != nil {
		t.Errorf("Madrid base 6: %v %v", o.H3, o.H6)
	}
	// an entry that is missing in the list is a missing slot, not the neighbour: drop the entry at +3 h
	m := omFixture(t, "openmeteo_moscow.json")
	m.Hourly.Time = append(m.Hourly.Time[:3], m.Hourly.Time[4:]...)
	m.Hourly.Temp = append(m.Hourly.Temp[:3], m.Hourly.Temp[4:]...)
	m.Hourly.Code = append(m.Hourly.Code[:3], m.Hourly.Code[4:]...)
	o = at(m, 1795656000)
	if o.H3 != nil || slotTemp(o.H6) != "-2r" {
		t.Errorf("a gap at +3 h: %s %s", slotTemp(o.H3), slotTemp(o.H6))
	}
}

func TestOpenMeteoDayWindowAndMidnight(t *testing.T) {
	has := func(r omResponse, unix int64) (bool, *WeatherOut) {
		out, err := r.toOut(time.Unix(unix, 0))
		if err != nil {
			t.Fatal(err)
		}
		if (out.Hi == nil) != (out.Lo == nil) || (out.Hi == nil) != (out.Pop == nil) {
			t.Fatalf("high, low and chance of rain must come and go together: %+v", out)
		}
		return out.Hi != nil, out
	}
	madrid := omFixture(t, "openmeteo_madrid.json")
	start := madrid.Daily.Time[0] // 2026-11-26 00:00 in Madrid
	if want := time.Date(2026, 11, 26, 0, 0, 0, 0, mustZone(t, "Europe/Madrid")).Unix(); start != want {
		t.Fatalf("fixture day starts at %d, want %d", start, want)
	}
	for _, c := range []struct {
		name string
		at   int64
		want bool
	}{
		{"a second before the day", start - 1, false}, {"the first second", start, true},
		{"the last second", start + 86400 - 1, true}, {"the next local midnight", start + 86400, false},
	} {
		if got, _ := has(madrid, c.at); got != c.want {
			t.Errorf("%s: day values present = %v, want %v", c.name, got, c.want)
		}
	}
	// an answer cached across the local midnight shows no high, low or chance of rain (the display prints "-"), not yesterday's;
	// the current temperature and the icon stay
	got, out := has(madrid, start+86400+60)
	if got || out.T == nil || out.Icon != "sun" {
		t.Errorf("after midnight: %+v", out)
	}

	// days of 23 and 25 hours: the window ends at the next local midnight, not 24 hours on
	day := func(zone string, y int, m time.Month, d int) omResponse {
		loc := mustZone(t, zone)
		hi, lo, pop := 20.0, 10.0, 30.0
		var r omResponse
		r.Timezone = zone
		r.Daily.Time = []int64{time.Date(y, m, d, 0, 0, 0, 0, loc).Unix()}
		r.Daily.Max, r.Daily.Min, r.Daily.Pop = []*float64{&hi}, []*float64{&lo}, []*float64{&pop}
		code := 1
		r.Current.Code = &code // something besides the day, so that outside the day the answer is not "no data"
		return r
	}
	spring := day("Europe/Madrid", 2026, 3, 29) // 23 hours: 2026-03-28 23:00 UTC to 2026-03-29 22:00 UTC
	if got, _ := has(spring, time.Date(2026, 3, 29, 21, 59, 59, 0, time.UTC).Unix()); !got {
		t.Error("spring day: the last second is in the day")
	}
	if got, _ := has(spring, time.Date(2026, 3, 29, 22, 30, 0, 0, time.UTC).Unix()); got {
		t.Error("spring day: 00:30 on the 30th is tomorrow (a 24 hour rule says it is today)")
	}
	autumn := day("Europe/Madrid", 2026, 10, 25) // 25 hours: 2026-10-24 22:00 UTC to 2026-10-25 23:00 UTC
	if got, _ := has(autumn, time.Date(2026, 10, 25, 22, 30, 0, 0, time.UTC).Unix()); !got {
		t.Error("autumn day: 23:30 on the 25th is still today (a 24 hour rule says it is over)")
	}
	if got, _ := has(autumn, time.Date(2026, 10, 25, 23, 0, 0, 0, time.UTC).Unix()); got {
		t.Error("autumn day: the next midnight is tomorrow")
	}
}

// omAt builds an answer the way Open-Meteo does for a zone at an instant: unix times on the local hour, the day
// starting at the local midnight, temperatures 10, 11, ... for the next 12 hours.
func omAt(t *testing.T, zone string, now time.Time) omResponse {
	loc := mustZone(t, zone)
	l := now.In(loc)
	hour := time.Date(l.Year(), l.Month(), l.Day(), l.Hour(), 0, 0, 0, loc)
	_, off := l.Zone()
	var r omResponse
	r.Timezone, r.UTCOffset = zone, off
	r.Current.Time = hour.Unix()
	cur, code := 10.0, 1
	r.Current.Temp, r.Current.Code = &cur, &code
	for i := 0; i < 12; i++ {
		v, c := float64(10+i), i*10 // the codes 0, 10, 20, 30 (cloud), ..., 60 (rain), ...
		r.Hourly.Time = append(r.Hourly.Time, hour.Add(time.Duration(i)*time.Hour).Unix())
		r.Hourly.Temp = append(r.Hourly.Temp, &v)
		r.Hourly.Code = append(r.Hourly.Code, &c)
	}
	hi, lo, pop := 25.4, 5.6, 40.0
	r.Daily.Time = []int64{time.Date(l.Year(), l.Month(), l.Day(), 0, 0, 0, 0, loc).Unix()}
	r.Daily.Max, r.Daily.Min, r.Daily.Pop = []*float64{&hi}, []*float64{&lo}, []*float64{&pop}
	return r
}

// Whether the day's values are shown must agree with a plain comparison of local calendar dates, in every zone, also
// across the date line, half-hour zones and the days the clocks change.
func TestOpenMeteoLocalDayInManyZones(t *testing.T) {
	zones := []string{"Europe/Madrid", "Europe/Moscow", "Asia/Kathmandu", "Asia/Kolkata", "Australia/Lord_Howe", "Australia/Adelaide",
		"Pacific/Kiritimati", "Pacific/Pago_Pago", "Pacific/Auckland", "Pacific/Chatham", "Pacific/Tongatapu", "America/St_Johns", "America/Sao_Paulo", "UTC"}
	starts := []time.Time{
		time.Date(2026, 12, 31, 12, 0, 0, 0, time.UTC), // new year on the date line
		time.Date(2026, 3, 28, 20, 0, 0, 0, time.UTC),  // Europe changes its clocks on the 29th
		time.Date(2026, 10, 24, 20, 0, 0, 0, time.UTC), // and back on the 25th
		time.Date(2026, 4, 4, 10, 0, 0, 0, time.UTC),   // New Zealand and Australia change on the 5th
		time.Date(2026, 9, 26, 10, 0, 0, 0, time.UTC),  // and again
	}
	for _, zone := range zones {
		loc := mustZone(t, zone)
		for _, start := range starts {
			r := omAt(t, zone, start)
			for h := 0; h <= 60; h++ {
				now := start.Add(time.Duration(h)*time.Hour + 17*time.Minute)
				out, err := r.toOut(now)
				if err != nil {
					t.Fatal(err)
				}
				y1, m1, d1 := start.In(loc).Date()
				y2, m2, d2 := now.In(loc).Date()
				same := y1 == y2 && m1 == m2 && d1 == d2
				if (out.Hi != nil) != same {
					t.Fatalf("%s: answer of %s used at %s (%s): day values shown = %v, want %v", zone, start.In(loc).Format(time.RFC3339), now.In(loc).Format(time.RFC3339), now.UTC().Format(time.RFC3339), out.Hi != nil, same)
				}
				if same && (*out.Hi != 25 || *out.Lo != 6 || *out.Pop != 40) {
					t.Fatalf("%s: %d %d %d", zone, *out.Hi, *out.Lo, *out.Pop)
				}
			}
		}
	}
}

func TestOpenMeteoDateLineAndHalfHourSlots(t *testing.T) {
	now := time.Date(2026, 12, 31, 12, 0, 0, 0, time.UTC)
	cases := []struct{ zone, asof string }{ // asof: the local hour that contains the instant, in the place's own time
		{"Pacific/Kiritimati", "2027-01-01T02:00"},  // UTC+14, the first place to see the new year
		{"Pacific/Pago_Pago", "2026-12-31T01:00"},   // UTC-11, a day behind it
		{"Pacific/Auckland", "2027-01-01T01:00"},    // NZDT, +13
		{"Asia/Kathmandu", "2026-12-31T17:00"},      // +5:45
		{"Asia/Kolkata", "2026-12-31T17:00"},        // +5:30
		{"Australia/Lord_Howe", "2026-12-31T23:00"}, // +11 in summer, a half-hour DST step
	}
	for _, c := range cases {
		r := omAt(t, c.zone, now)
		out, err := r.toOut(now)
		if err != nil {
			t.Fatal(err)
		}
		if out.AsOf != c.asof {
			t.Errorf("%s: asof %s, want %s", c.zone, out.AsOf, c.asof)
		}
		if slotTemp(out.H3) != "13c" || slotTemp(out.H6) != "16r" || out.Hi == nil || *out.Hi != 25 {
			t.Errorf("%s: %s %s %v", c.zone, slotTemp(out.H3), slotTemp(out.H6), out.Hi)
		}
	}
	// the same instant, 22 hours later, is a new day at the date line's east side only
	later := now.Add(22 * time.Hour) // 2027-01-01 10:00 UTC
	for zone, wantToday := range map[string]bool{"Pacific/Kiritimati": false, "Pacific/Auckland": true, "Pacific/Pago_Pago": true, "Asia/Kathmandu": false} {
		r := omAt(t, zone, now)
		out, _ := r.toOut(later)
		if (out.Hi != nil) != wantToday {
			t.Errorf("%s: 22 hours later the day values shown = %v, want %v", zone, out.Hi != nil, wantToday)
		}
	}
}

func TestOpenMeteoZoneFallback(t *testing.T) {
	// the offset of the answer is used when the zone name is empty, "Local" or not in the tz database (never LoadLocation's UTC or the server's zone)
	for _, name := range []string{"", "Local", "Mars/Olympus_Mons", "../../etc/passwd"} {
		r := omFixture(t, "openmeteo_kathmandu.json")
		r.Timezone = name
		out, err := r.toOut(clock0())
		if err != nil {
			t.Fatal(err)
		}
		if out.AsOf != "2026-11-26T06:45" || out.Hi == nil || *out.Hi != 21 || slotTemp(out.H3) != "19s" {
			t.Errorf("zone %q: %+v", name, out)
		}
	}
}

// ---- gaps in the data -------------------------------------------------------------------------------------

func TestOpenMeteoNullsAndShortArrays(t *testing.T) {
	madridBody := read(t, "openmeteo_madrid.json")
	get := func(body string) (*WeatherOut, error) {
		s := newStub(t)
		s.omBody.Store(body)
		return s.app(t, clock0()).weather.Get(context.Background(), 40.4168, -3.7038)
	}
	// a null current temperature: no "t", the icon is still there
	body := strings.Replace(madridBody, `"temperature_2m":6.1,`, `"temperature_2m":null,`, 1)
	if body == madridBody {
		t.Fatal("the fixture changed: no current temperature to blank")
	}
	out, err := get(body)
	if err != nil || out.T != nil || out.Icon != "sun" || out.Hi == nil || out.H3 == nil {
		t.Fatalf("null current temperature: %+v %v", out, err)
	}
	if strings.Contains(marshal(t, out), `"t":6`) || !strings.Contains(marshal(t, out), `"h3":{"t":5,`) {
		t.Fatalf("json %s", marshal(t, out))
	}
	// a null current code: icon cloud, code -1
	body = strings.Replace(madridBody, `"weather_code":1}`, `"weather_code":null}`, 1)
	if out, err = get(body); err != nil || out.Icon != "cloud" || out.Code != -1 || out.T == nil {
		t.Fatalf("null current code: %+v %v", out, err)
	}

	toOut := func(r omResponse) *WeatherOut {
		out, err := r.toOut(clock0())
		if err != nil {
			t.Fatal(err)
		}
		return out
	}
	// only four hourly entries: +3 h is there, +6 h is not
	short := omFixture(t, "openmeteo_madrid.json")
	short.Hourly.Time, short.Hourly.Temp, short.Hourly.Code = short.Hourly.Time[:4], short.Hourly.Temp[:4], short.Hourly.Code[:4]
	if o := toOut(short); o.H3 == nil || o.H6 != nil {
		t.Errorf("4 entries: %v %v", o.H3, o.H6)
	}
	// temperatures shorter than the times: no slot rather than a panic
	short = omFixture(t, "openmeteo_madrid.json")
	short.Hourly.Temp, short.Hourly.Code = short.Hourly.Temp[:2], short.Hourly.Code[:2]
	if o := toOut(short); o.H3 != nil || o.H6 != nil {
		t.Errorf("short temperatures: %v %v", o.H3, o.H6)
	}
	// a null temperature in a slot: no slot; a null code in a slot: the slot with the cloud icon
	n := omFixture(t, "openmeteo_madrid.json")
	n.Hourly.Temp[3], n.Hourly.Code[6] = nil, nil
	if o := toOut(n); o.H3 != nil || slotTemp(o.H6) != "3c" {
		t.Errorf("null slot values: %s %s", slotTemp(o.H3), slotTemp(o.H6))
	}
	// no codes at all in the hourly list: cloud icons
	n = omFixture(t, "openmeteo_madrid.json")
	n.Hourly.Code = nil
	if o := toOut(n); slotTemp(o.H3) != "5c" {
		t.Errorf("no hourly codes: %s", slotTemp(o.H3))
	}
	// an empty daily block, or one with nulls
	e := omFixture(t, "openmeteo_madrid.json")
	e.Daily.Time = nil
	if o := toOut(e); o.Hi != nil || o.Lo != nil || o.Pop != nil || o.T == nil {
		t.Errorf("no daily: %+v", o)
	}
	e = omFixture(t, "openmeteo_madrid.json")
	e.Daily.Max, e.Daily.Min, e.Daily.Pop = []*float64{nil}, nil, []*float64{}
	if o := toOut(e); o.Hi != nil || o.Lo != nil || o.Pop != nil {
		t.Errorf("null and short daily values: %+v", o)
	}
	// no hourly block, no "current" time
	e = omFixture(t, "openmeteo_madrid.json")
	e.Hourly.Time, e.Current.Time = nil, 0
	if o := toOut(e); o.H3 != nil || o.H6 != nil || o.AsOf != "" || o.T == nil {
		t.Errorf("no hourly: %+v", o)
	}
}

func TestOpenMeteoNoDataAtAllIs422(t *testing.T) {
	for _, body := range []string{`{}`, `{"timezone":"Europe/Madrid","current":{},"hourly":{"time":[],"temperature_2m":[]},"daily":{}}`,
		`{"current":{"time":1795654800,"temperature_2m":null,"weather_code":null},"hourly":{"time":[1795654800],"temperature_2m":[null],"weather_code":[null]},"daily":{"time":[1795647600],"temperature_2m_max":[null],"temperature_2m_min":[null],"precipitation_probability_max":[null]}}`} {
		s := newStub(t)
		s.omBody.Store(body)
		a := s.app(t, clock0())
		if _, err := a.weather.Get(context.Background(), 40.4168, -3.7038); !errors.Is(err, errNoWeather) {
			t.Fatalf("%s: got %v, want errNoWeather", body, err)
		}
		h := a.routes()
		path := "/v1/weather?lat=40.4168&lon=-3.7038"
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, signedRequest(t, "GET", path, testNow, ""))
		if rec.Code != 422 || !strings.Contains(rec.Body.String(), "no weather data for this location") {
			t.Fatalf("HTTP layer: %d %s", rec.Code, rec.Body.String())
		}
	}
	// one value is enough: only a weather code still draws an icon
	s := newStub(t)
	s.omBody.Store(`{"current":{"time":1795654800,"weather_code":61}}`)
	out := getWeather(t, s.app(t, clock0()), 40.4168, -3.7038)
	if out.Icon != "rain" || out.Code != 61 || out.T != nil {
		t.Fatalf("%+v", out)
	}
}

func TestOpenMeteoErrorsAreBadGatewayWithoutDetail(t *testing.T) {
	var logs bytes.Buffer
	log.SetOutput(&logs)
	defer log.SetOutput(os.Stderr)
	for _, c := range []struct {
		name   string
		status int32
		body   string
	}{
		{"server error", 500, ""}, {"rate limit", 429, ""}, {"bad request", 400, ""},
		{"a page that is not JSON", 200, "<html>consent</html>"},
	} {
		s := newStub(t)
		s.omStatus.Store(c.status)
		if c.body != "" {
			s.omBody.Store(c.body)
		}
		a := s.app(t, clock0())
		if _, err := a.weather.Get(context.Background(), 40.4168, -3.7038); err == nil || errors.Is(err, errNoWeather) {
			t.Fatalf("%s: got %v, want a 502-type error", c.name, err)
		}
		rec := httptest.NewRecorder()
		a.routes().ServeHTTP(rec, signedRequest(t, "GET", "/v1/weather?lat=40.4168&lon=-3.7038", testNow, ""))
		if rec.Code != 502 || strings.Contains(rec.Body.String(), "127.0.0.1") || strings.Contains(rec.Body.String(), "40.4") {
			t.Fatalf("%s: %d %s", c.name, rec.Code, rec.Body.String())
		}
	}
	// the log has the host and the status, never a URL, a query or a coordinate
	for _, bad := range []string{"latitude", "longitude", "40.4", "-3.7", "?", "http://"} {
		if strings.Contains(logs.String(), bad) {
			t.Errorf("the log contains %q:\n%s", bad, logs.String())
		}
	}
	if !strings.Contains(logs.String(), "upstream error") {
		t.Errorf("nothing was logged:\n%s", logs.String())
	}
}

// ---- the icon table ---------------------------------------------------------------------------------------

func TestIconForWMO(t *testing.T) {
	want := map[string][]int{
		"sun":   {0, 1},
		"part":  {2},
		"cloud": {3, 45, 48},
		// drizzle, freezing drizzle, rain, freezing rain, showers, snow, snow grains, snow showers and thunderstorms
		// (there is no snow, storm or hail icon: they all show as rain)
		"rain": {51, 53, 55, 56, 57, 61, 63, 65, 66, 67, 71, 73, 75, 77, 80, 81, 82, 85, 86, 95, 96, 97, 99},
	}
	n := 0
	for icon, codes := range want {
		for _, c := range codes {
			n++
			if got := iconForWMO(c); got != icon {
				t.Errorf("iconForWMO(%d) = %s, want %s", c, got, icon)
			}
		}
	}
	if n != 29 {
		t.Errorf("%d documented codes in the table, want 29", n)
	}
	// codes the documentation does not list: haze and dust below 50, wet from 50, a negative number is "missing"
	for code, icon := range map[int]string{-1: "cloud", -99: "cloud", 4: "cloud", 10: "cloud", 44: "cloud", 49: "cloud", 50: "rain", 98: "rain", 100: "rain", 120: "rain"} {
		if got := iconForWMO(code); got != icon {
			t.Errorf("iconForWMO(%d) = %s, want %s", code, got, icon)
		}
	}
	// the icons are the four the display has
	for c := -5; c < 130; c++ {
		switch iconForWMO(c) {
		case "sun", "part", "cloud", "rain":
		default:
			t.Fatalf("code %d gives %q", c, iconForWMO(c))
		}
	}
}

// ---- the cache --------------------------------------------------------------------------------------------

func cacheKeys(c *Cache) []string {
	c.mu.Lock()
	defer c.mu.Unlock()
	var keys []string
	for k := range c.items {
		keys = append(keys, k)
	}
	return keys
}

func TestOpenMeteoCache(t *testing.T) {
	s := newStub(t)
	a := s.app(t, clock0())
	clock := time.Unix(1795656000, 0)
	a.weather.cache.now = func() time.Time { return clock }
	get := func(lat, lon float64) (*WeatherOut, error) { return a.weather.Get(context.Background(), lat, lon) }

	for i := 0; i < 3; i++ {
		if _, err := get(40.4168, -3.7038); err != nil {
			t.Fatal(err)
		}
	}
	if n := s.omHits.Load(); n != 1 {
		t.Fatalf("three calls for one place made %d upstream calls, want 1", n)
	}
	// two pins that round to the same place share the call (the key holds the ROUNDED coordinates)
	if _, err := get(40.37, -3.66); err != nil {
		t.Fatal(err)
	}
	if n := s.omHits.Load(); n != 1 {
		t.Fatalf("pins in the same rounded place made %d upstream calls, want 1", n)
	}
	if keys := cacheKeys(a.weather.cache); len(keys) != 1 || keys[0] != "om|40.4|-3.7" {
		t.Fatalf("cache keys %v, want [om|40.4|-3.7]", keys)
	}
	// another place is another call
	if _, err := get(41.0, -3.7); err != nil {
		t.Fatal(err)
	}
	if n := s.omHits.Load(); n != 2 {
		t.Fatalf("a second place made %d upstream calls in all, want 2", n)
	}
	// fresh for 15 minutes
	clock = clock.Add(14*time.Minute + 59*time.Second)
	get(40.4168, -3.7038)
	if n := s.omHits.Load(); n != 2 {
		t.Fatalf("an answer 14:59 old was fetched again (%d calls)", n)
	}
	clock = clock.Add(2 * time.Second)
	get(40.4168, -3.7038)
	if n := s.omHits.Load(); n != 3 {
		t.Fatalf("an answer over 15 minutes old was not fetched again (%d calls)", n)
	}
	// Open-Meteo goes down: the stale answer is served for 3 hours
	s.omStatus.Store(500)
	clock = clock.Add(16 * time.Minute)
	out, err := get(40.4168, -3.7038)
	if err != nil || out.Src != "Open-Meteo" || out.T == nil {
		t.Fatalf("stale answer: %+v %v", out, err)
	}
	expired := clock.Add(-time.Minute) // it expired a minute ago (16 minutes after its fetch, TTL 15)
	clock = expired.Add(3*time.Hour - time.Minute)
	if _, err := get(40.4168, -3.7038); err != nil {
		t.Fatalf("an answer that expired 2:59 ago is still served: %v", err)
	}
	clock = expired.Add(3*time.Hour + time.Minute)
	if _, err := get(40.4168, -3.7038); err == nil {
		t.Fatal("an answer that expired more than 3 hours ago must not be served")
	}
	// -0.04 and +0.04 both round to 0.0: one key, no "-0.0"
	s2 := newStub(t)
	a2 := s2.app(t, clock0())
	for _, ll := range [][2]float64{{0.04, 0.04}, {-0.04, -0.04}, {0.04, -0.04}} {
		getWeather(t, a2, ll[0], ll[1])
	}
	if keys := cacheKeys(a2.weather.cache); len(keys) != 1 || keys[0] != "om|0.0|0.0" || s2.omHits.Load() != 1 {
		t.Fatalf("keys %v, hits %d", keys, s2.omHits.Load())
	}
}
