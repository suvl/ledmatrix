package main

import (
	"context"
	"encoding/xml"
	"errors"
	"fmt"
	"net/url"
	"sort"
	"strings"
	"time"
	"unicode"
)

// Headlines shrinks a Google News RSS search (about 120 KB per 100 items) to a few headlines.
//
// The feed is unofficial and carries a "personal, non-commercial use" notice; one hobby display
// polling every 30 minutes fits that. If it ever blocks us the answer is an error and the display
// keeps its last headline.
type Headlines struct {
	cache *Cache
	http  *Upstream
	base  string
}

func NewHeadlines(cache *Cache, up *Upstream) *Headlines {
	return &Headlines{cache: cache, http: up, base: "https://news.google.com"}
}

type HeadlineQuery struct {
	Q, HL, GL, CEID string
	N               int
}

type Headline struct {
	T  string `json:"t"`  // title without the trailing " - Source"
	S  string `json:"s"`  // source name
	TS int64  `json:"ts"` // publication time, unix seconds
}

type HeadlinesOut struct {
	Q     string     `json:"q"`
	Items []Headline `json:"items"`
	Src   string     `json:"src"`
}

type rssDoc struct {
	XMLName xml.Name `xml:"rss"` // anything else (a consent or block page) is an error, not "no news"
	Channel struct {
		Items []struct {
			Title   string `xml:"title"`
			PubDate string `xml:"pubDate"`
			Source  string `xml:"source"`
		} `xml:"item"`
	} `xml:"channel"`
}

func normalise(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if unicode.IsLetter(r) || unicode.IsDigit(r) {
			b.WriteRune(r)
		}
	}
	n := b.String()
	if len(n) > 48 {
		n = n[:48]
	}
	return n
}

// parseHeadlines reads the RSS, newest first, without duplicates.
func parseHeadlines(b []byte, n int) ([]Headline, error) {
	var doc rssDoc
	if err := xml.Unmarshal(b, &doc); err != nil {
		return nil, errors.New("news feed: unexpected response")
	}
	var out []Headline
	seen := map[string]bool{}
	for _, it := range doc.Channel.Items {
		title := strings.TrimSpace(it.Title)
		src := strings.TrimSpace(it.Source)
		if src != "" {
			title = strings.TrimSuffix(title, " - "+src)
		}
		t, err := time.Parse(time.RFC1123, strings.TrimSpace(it.PubDate))
		if title == "" || err != nil {
			continue
		}
		key := normalise(title)
		if seen[key] {
			continue
		}
		seen[key] = true
		out = append(out, Headline{T: title, S: src, TS: t.Unix()})
	}
	sort.SliceStable(out, func(i, j int) bool { return out[i].TS > out[j].TS })
	if len(out) > n {
		out = out[:n]
	}
	return out, nil
}

func (h *Headlines) Get(ctx context.Context, q HeadlineQuery) (*HeadlinesOut, error) {
	key := "news|" + q.Q + "|" + q.HL + "|" + q.GL + "|" + q.CEID
	v, err := h.cache.Get(key, 15*time.Minute, 6*time.Hour, func() (any, error) {
		u := fmt.Sprintf("%s/rss/search?q=%s&hl=%s&gl=%s&ceid=%s", h.base,
			url.QueryEscape(q.Q+" when:1d"), url.QueryEscape(q.HL), url.QueryEscape(q.GL), url.QueryEscape(q.CEID))
		b, err := h.http.GetBytes(ctx, u, 600<<10)
		if err != nil {
			return nil, err
		}
		return parseHeadlines(b, 10)
	})
	if err != nil {
		return nil, err
	}
	items := v.([]Headline)
	if len(items) > q.N {
		items = items[:q.N]
	}
	if items == nil {
		items = []Headline{}
	}
	return &HeadlinesOut{Q: q.Q, Items: items, Src: "Google News"}, nil
}
