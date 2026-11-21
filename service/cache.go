package main

import (
	"sync"
	"time"
)

// Cache is a small TTL cache. Concurrent callers asking for the same key share one load (the
// per-key lock serialises them), and a failed reload serves the last good value for a while, so a
// flaky upstream does not blank the display.
type Cache struct {
	mu    sync.Mutex
	items map[string]*cacheItem
	now   func() time.Time
}

type cacheItem struct {
	mu  sync.Mutex
	val any
	exp time.Time
	has bool
}

func NewCache() *Cache {
	return &Cache{items: map[string]*cacheItem{}, now: time.Now}
}

// Get returns the cached value for key, loading it when missing or older than ttl. When the load
// fails, a value that expired less than staleFor ago is returned instead of the error.
func (c *Cache) Get(key string, ttl, staleFor time.Duration, load func() (any, error)) (any, error) {
	c.mu.Lock()
	it := c.items[key]
	if it == nil {
		if len(c.items) > 2000 {
			c.sweep()
		}
		it = &cacheItem{}
		c.items[key] = it
	}
	c.mu.Unlock()

	it.mu.Lock()
	defer it.mu.Unlock()
	now := c.now()
	if it.has && now.Before(it.exp) {
		return it.val, nil
	}
	v, err := load()
	if err != nil {
		if it.has && now.Before(it.exp.Add(staleFor)) {
			return it.val, nil
		}
		return nil, err
	}
	it.val, it.exp, it.has = v, now.Add(ttl), true
	return v, nil
}

// sweep drops entries that expired more than a day ago. Callers hold c.mu.
func (c *Cache) sweep() {
	cutoff := c.now().Add(-24 * time.Hour)
	for k, it := range c.items {
		if it.mu.TryLock() {
			if !it.has || it.exp.Before(cutoff) {
				delete(c.items, k)
			}
			it.mu.Unlock()
		}
	}
}
