package main

import (
	"net/http/httptest"
	"net/url"
	"os"
	"regexp"
	"strings"
	"testing"
	"time"
)

// The headline edition (hl, gl, ceid): validated, passed on to Google, part of the cache key, and every edition
// the board offers (arduino/ledmatrix/editions.h) is accepted.

type edition struct{ label, hl, gl, ceid string }

// boardEditions reads the list the board and the portal offer, from its one place.
func boardEditions(t *testing.T) []edition {
	t.Helper()
	src, err := os.ReadFile("../arduino/ledmatrix/editions.h")
	if err != nil {
		t.Fatal(err)
	}
	s := string(src)
	start := strings.Index(s, "LIST[] = {")
	end := strings.Index(s[start:], "\n};")
	if start < 0 || end < 0 {
		t.Fatal("editions.h: no LIST")
	}
	re := regexp.MustCompile(`\{"([^"]*)",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\}`)
	var out []edition
	for _, m := range re.FindAllStringSubmatch(s[start:start+end], -1) {
		out = append(out, edition{m[1], m[2], m[3], m[4]})
	}
	return out
}

func (s *stub) headlinesGet(t *testing.T, a *app, query string) (int, string) {
	t.Helper()
	path := "/v1/headlines?q=tram" + query
	rec := httptest.NewRecorder()
	a.routes().ServeHTTP(rec, signedRequest(t, "GET", path, testNow, ""))
	return rec.Code, rec.Body.String()
}

// what Google was asked, decoded
func (s *stub) newsAsked(t *testing.T) url.Values {
	t.Helper()
	raw, _ := s.newsQuery.Load().(string)
	v, err := url.ParseQuery(raw)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func TestEveryBoardEditionIsAccepted(t *testing.T) {
	list := boardEditions(t)
	if len(list) < 8 {
		t.Fatalf("%d editions read from editions.h", len(list))
	}
	for i, e := range list {
		q := HeadlineQuery{Q: "x", HL: e.hl, GL: e.gl, CEID: e.ceid}
		if err := q.validateEdition(); err != nil {
			t.Errorf("edition %d (%s): %v", i, e.label, err)
		}
	}
	// index 0 is the service's own default: a board that sends nothing and one that sends edition 0 ask for the same
	s := newStub(t)
	a := s.app(t, time.Now())
	if code, body := s.headlinesGet(t, a, ""); code != 200 {
		t.Fatalf("no edition: %d %s", code, body)
	}
	got := s.newsAsked(t)
	if got.Get("hl") != list[0].hl || got.Get("gl") != list[0].gl || got.Get("ceid") != list[0].ceid {
		t.Fatalf("the service's defaults %v are not edition 0 %+v", got, list[0])
	}
}

// The board sends hl, gl and ceid percent-encoded ("&hl=en-GB&gl=GB&ceid=GB%3Aen"); Google must get them decoded, as the board meant them.
func TestHeadlineEditionReachesGoogleAndTheCache(t *testing.T) {
	s := newStub(t)
	a := s.app(t, time.Now())
	if code, body := s.headlinesGet(t, a, "&hl=en-GB&gl=GB&ceid=GB%3Aen"); code != 200 {
		t.Fatalf("%d %s", code, body)
	}
	got := s.newsAsked(t)
	if got.Get("hl") != "en-GB" || got.Get("gl") != "GB" || got.Get("ceid") != "GB:en" || !strings.HasPrefix(got.Get("q"), "tram") {
		t.Fatalf("Google was asked %v", got)
	}
	if n := s.newsHits.Load(); n != 1 {
		t.Fatalf("%d upstream calls", n)
	}
	// the same edition is cached, another one is another call (the key holds hl, gl and ceid)
	s.headlinesGet(t, a, "&hl=en-GB&gl=GB&ceid=GB%3Aen")
	if n := s.newsHits.Load(); n != 1 {
		t.Fatalf("the same edition was fetched again: %d calls", n)
	}
	s.headlinesGet(t, a, "&hl=en-US&gl=US&ceid=US%3Aen")
	if n := s.newsHits.Load(); n != 2 {
		t.Fatalf("another edition was not fetched: %d calls", n)
	}
	s.headlinesGet(t, a, "")
	if n := s.newsHits.Load(); n != 3 {
		t.Fatalf("the default edition shares the cache with another: %d calls", n)
	}
	// the unencoded colon works as well (a hand-made request)
	s.headlinesGet(t, a, "&hl=en-GB&gl=GB&ceid=GB:en")
	if n := s.newsHits.Load(); n != 3 {
		t.Fatalf("GB:en and GB%%3Aen are the same edition: %d calls", n)
	}
}

func TestHeadlineEditionValidation(t *testing.T) {
	good := []string{"&hl=pt-PT&gl=PT&ceid=PT%3Apt-150", "&hl=es-419&gl=MX&ceid=MX%3Aes-419", "&hl=en&gl=US&ceid=US%3Aen", "&hl=zh-Hant&gl=TW&ceid=TW%3Azh-Hant",
		"&hl=", "&gl=", "&ceid=", "&hl=de"} // empty means "not given": the default
	for _, q := range good {
		s := newStub(t)
		if code, body := s.headlinesGet(t, s.app(t, time.Now()), q); code != 200 {
			t.Errorf("%s: %d %s", q, code, body)
		}
	}
	bad := map[string]string{
		"&hl=pt-PT%26x%3D1":                     "hl", // a parameter smuggled into the URL to Google
		"&hl=pt%20PT":                           "hl",
		"&hl=pt_PT":                             "hl",
		"&hl=PT":                                "hl", // a language is lower case
		"&hl=p":                                 "hl",
		"&hl=pt-":                               "hl",
		"&hl=pt-PT-x-y-z":                       "hl", // more than two subtags
		"&hl=" + strings.Repeat("a", 30):        "hl",
		"&hl=pt-P%0AT":                          "hl", // a newline
		"&hl=p%C3%A3":                           "hl", // not ASCII
		"&hl=%00":                               "hl",
		"&gl=pt":                                "gl",
		"&gl=PRT":                               "gl",
		"&gl=P":                                 "gl",
		"&gl=P1":                                "gl",
		"&gl=PT%2C":                             "gl",
		"&ceid=PT":                              "ceid",
		"&ceid=PT%3A":                           "ceid",
		"&ceid=pt%3Apt-150":                     "ceid",
		"&ceid=PT%3APT":                         "ceid",
		"&ceid=PT%3Apt-150%26hl%3Dx":            "ceid",
		"&ceid=PT%3A" + strings.Repeat("a", 30): "ceid",
		"&ceid=PT%3Apt-150%0D%0A":               "ceid",
		"&ceid=%3Apt":                           "ceid",
	}
	for q, which := range bad {
		s := newStub(t)
		code, body := s.headlinesGet(t, s.app(t, time.Now()), q)
		if code != 400 || !strings.Contains(body, which) || s.newsHits.Load() != 0 {
			t.Errorf("%s: want 400 naming %s and no call to Google, got %d %s (calls %d)", q, which, code, body, s.newsHits.Load())
		}
	}
}
