// The Google News editions the owner can pick for the headlines (portal: "Headlines language and region").
//
// No Arduino includes, so tools/test_editions.cpp tests it on the host. THIS LIST IS THE ONE PLACE: tools/build_web.py writes the
// portal's <option> list from it, tools/test_editions_portal.py checks the generated page against it, and the service's own tests
// (service/headlines_test.go) read it to check that the service accepts every triple.
//
// APPEND-ONLY. The saved setting is the INDEX in this list (settings.cpp, NVS key "hled"), so a saved setting stays valid only
// if no entry is ever moved, removed or edited into another edition. Add new editions at the end; a stored index that is not in the
// list (a board that was downgraded) falls back to the default. tools/test_editions.cpp pins every entry for that reason.
//
// Each triple was checked on 2026-11-26 against https://news.google.com/rss/search (the feed the service reads): HTTP 200, 100
// items, in the edition's language. hl is the interface language, gl the country, ceid country:language. Google redirects some
// spellings (hl=es-ES answered 302, hl=es answers 200), so a triple is only added after trying it.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace editions {

struct Edition {
  const char *label;  // shown in the portal
  const char *hl;
  const char *gl;
  const char *ceid;
};

// Index 0 is the service's own default (service/main.go: pt-PT, PT, PT:pt-150): a board that never saved this setting asks as before.
inline constexpr Edition LIST[] = {
    {"Portuguese (Portugal)", "pt-PT", "PT", "PT:pt-150"},
    {"Portuguese (Brazil)", "pt-BR", "BR", "BR:pt-419"},
    {"English (United Kingdom)", "en-GB", "GB", "GB:en"},
    {"English (United States)", "en-US", "US", "US:en"},
    {"English (Australia)", "en-AU", "AU", "AU:en"},
    {"English (Canada)", "en-CA", "CA", "CA:en"},
    {"Spanish (Spain)", "es", "ES", "ES:es"},
    {"Spanish (Mexico)", "es-419", "MX", "MX:es-419"},
    {"French (France)", "fr", "FR", "FR:fr"},
    {"German (Germany)", "de", "DE", "DE:de"},
    {"Italian (Italy)", "it", "IT", "IT:it"},
    {"Dutch (Netherlands)", "nl", "NL", "NL:nl"},
};
// Not in the list on purpose: Japanese (ja / JP / JP:ja works at Google, but the panel's fonts are ASCII only, so the headlines would
// be unreadable). Accented letters (é, ã, ü) come out as wrong characters on the panel today, Portuguese included.
inline constexpr int COUNT = (int)(sizeof(LIST) / sizeof(LIST[0]));
inline constexpr uint8_t DEFAULT_INDEX = 0;  // Portuguese (Portugal)

inline bool valid(int index) { return index >= 0 && index < COUNT; }

// The edition for a stored or received index; an index outside the list gives the default.
inline const Edition &get(int index) { return LIST[valid(index) ? index : DEFAULT_INDEX]; }

// Writes "&hl=pt-PT&gl=PT&ceid=PT%3Apt-150" for an edition (the default for an index outside the list) into `out`, ready to be
// appended to a query. Everything but A-Z a-z 0-9 - _ . ~ is percent-encoded. Returns false, with `out` empty, when it does not fit.
inline bool queryString(int index, char *out, size_t cap) {
  if (cap == 0) return false;
  const Edition &e = get(index);
  const char *parts[3][2] = {{"&hl=", e.hl}, {"&gl=", e.gl}, {"&ceid=", e.ceid}};
  size_t n = 0;
  for (auto &p : parts) {
    for (const char *s = p[0]; *s; s++) {
      if (n + 1 >= cap) { out[0] = 0; return false; }
      out[n++] = *s;
    }
    for (const char *s = p[1]; *s; s++) {
      uint8_t c = (uint8_t)*s;
      bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
      if (n + (plain ? 1 : 3) >= cap) { out[0] = 0; return false; }
      if (plain) {
        out[n++] = (char)c;
      } else {
        static const char hex[] = "0123456789ABCDEF";
        out[n++] = '%';
        out[n++] = hex[c >> 4];
        out[n++] = hex[c & 15];
      }
    }
  }
  out[n] = 0;
  return true;
}

}  // namespace editions
