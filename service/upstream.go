package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"time"
)

// Upstream is the HTTP client used for every outgoing request. It identifies itself generically
// and never puts a request URL (which can contain coordinates) into an error message.
type Upstream struct {
	client *http.Client
	ua     string
}

func NewUpstream() *Upstream {
	return &Upstream{client: &http.Client{Timeout: 10 * time.Second}, ua: "ledmatrix-api/0.1"}
}

type statusError struct {
	Code int
	Host string
}

func (e *statusError) Error() string { return fmt.Sprintf("%s answered HTTP %d", e.Host, e.Code) }

func asStatus(err error, target **statusError) bool { return errors.As(err, target) }

func (u *Upstream) GetBytes(ctx context.Context, rawURL string, max int64) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, fmt.Errorf("bad request")
	}
	req.Header.Set("User-Agent", u.ua)
	host := req.URL.Host
	resp, err := u.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("%s unreachable", host)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		io.Copy(io.Discard, io.LimitReader(resp.Body, 64<<10))
		return nil, &statusError{resp.StatusCode, host}
	}
	b, err := io.ReadAll(io.LimitReader(resp.Body, max+1))
	if err != nil {
		return nil, fmt.Errorf("%s: read failed", host)
	}
	if int64(len(b)) > max {
		return nil, fmt.Errorf("%s: response larger than %d bytes", host, max)
	}
	return b, nil
}

func (u *Upstream) GetJSON(ctx context.Context, rawURL string, v any, max int64) error {
	b, err := u.GetBytes(ctx, rawURL, max)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(b, v); err != nil {
		u2, _ := url.Parse(rawURL)
		return fmt.Errorf("%s: unexpected response", u2.Host)
	}
	return nil
}
