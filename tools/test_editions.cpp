// Host test for the Google News editions list (arduino/ledmatrix/editions.h). No Arduino headers needed.
//   c++ -std=c++17 -Wall -Wextra -Iarduino/ledmatrix tools/test_editions.cpp -o test_editions && ./test_editions
// The portal's list is written from this one (tools/build_web.py) and checked by tools/test_editions_portal.py; the service
// checks every triple against its own validation (service/headlines_test.go).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>

#include "editions.h"

static int gFails = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      printf("  line %d: check failed: %s\n", __LINE__, #cond);    \
      gFails++;                                                    \
    }                                                              \
  } while (0)

static bool lower(char c) { return c >= 'a' && c <= 'z'; }
static bool upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool alnum(char c) { return lower(c) || upper(c) || (c >= '0' && c <= '9'); }

// A language tag such as pt-PT, es-419, en: 2 or 3 lower case letters, then up to two subtags of 2 to 8 letters or digits.
static bool languageTag(const char *s) {
  size_t i = 0;
  while (lower(s[i])) i++;
  if (i < 2 || i > 3) return false;
  for (int subtags = 0; s[i]; subtags++) {
    if (subtags == 2 || s[i] != '-') return false;
    i++;
    size_t n = 0;
    while (alnum(s[i + n])) n++;
    if (n < 2 || n > 8) return false;
    i += n;
  }
  return true;
}

// The pinned list: the saved setting is an index, so this must only ever grow. Reordering or editing an entry fails here.
struct Pin { const char *label, *hl, *gl, *ceid; };
static const Pin PINNED[] = {
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
static const int PINNED_COUNT = (int)(sizeof(PINNED) / sizeof(PINNED[0]));

static std::string percentDecode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

static void entriesArePinned() {
  CHECK(editions::COUNT >= PINNED_COUNT);  // appended entries are allowed, removed ones are not
  for (int i = 0; i < PINNED_COUNT && i < editions::COUNT; i++) {
    const editions::Edition &e = editions::LIST[i];
    if (strcmp(e.label, PINNED[i].label) || strcmp(e.hl, PINNED[i].hl) || strcmp(e.gl, PINNED[i].gl) || strcmp(e.ceid, PINNED[i].ceid)) {
      printf("  entry %d is %s | %s | %s | %s, pinned %s | %s | %s | %s\n", i, e.label, e.hl, e.gl, e.ceid, PINNED[i].label, PINNED[i].hl,
             PINNED[i].gl, PINNED[i].ceid);
      gFails++;
    }
  }
  CHECK(editions::COUNT == PINNED_COUNT);  // when you append an edition, append it to PINNED too
}

static void defaultIsTheServicesOwn() {
  CHECK(editions::DEFAULT_INDEX == 0);
  const editions::Edition &d = editions::get(editions::DEFAULT_INDEX);
  CHECK(!strcmp(d.hl, "pt-PT") && !strcmp(d.gl, "PT") && !strcmp(d.ceid, "PT:pt-150"));  // what the service answers to when hl, gl and ceid are left out
}

static void everyEntryIsWellFormed() {
  std::set<std::string> labels, triples;
  for (int i = 0; i < editions::COUNT; i++) {
    const editions::Edition &e = editions::LIST[i];
    CHECK(e.label && e.hl && e.gl && e.ceid);
    CHECK(strlen(e.label) >= 3 && strlen(e.label) <= 40);
    for (const char *s = e.label; *s; s++) CHECK((unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7f && *s != '<' && *s != '>' && *s != '&' && *s != '"');  // goes into HTML
    CHECK(languageTag(e.hl));
    CHECK(strlen(e.gl) == 2 && upper(e.gl[0]) && upper(e.gl[1]));
    CHECK(strlen(e.ceid) >= 5 && strlen(e.ceid) <= 24);
    CHECK(!strncmp(e.ceid, e.gl, 2) && e.ceid[2] == ':' && languageTag(e.ceid + 3));  // ceid is country:language and the country is gl
    // the ceid language starts with the primary language of hl (pt-PT / PT:pt-150, es-419 / MX:es-419)
    const char *dash = strchr(e.hl, '-');
    size_t lang = dash ? (size_t)(dash - e.hl) : strlen(e.hl);
    CHECK(!strncmp(e.ceid + 3, e.hl, lang));
    CHECK(labels.insert(e.label).second);
    CHECK(triples.insert(std::string(e.hl) + "|" + e.gl + "|" + e.ceid).second);
  }
  CHECK(editions::COUNT >= 8 && editions::COUNT <= 40);  // a short list, and it fits a portal select
}

static void indexChecks() {
  CHECK(editions::valid(0) && editions::valid(editions::COUNT - 1));
  CHECK(!editions::valid(-1) && !editions::valid(editions::COUNT) && !editions::valid(255) && !editions::valid(1000000));
  // a stored index outside the list (a downgraded board) gives the default edition
  CHECK(&editions::get(-1) == &editions::LIST[0]);
  CHECK(&editions::get(editions::COUNT) == &editions::LIST[0]);
  CHECK(&editions::get(200) == &editions::LIST[0]);
  CHECK(&editions::get(3) == &editions::LIST[3]);
  // the byte that NVS gives back
  for (int raw = 0; raw < 256; raw++) CHECK(editions::valid(raw) == (raw < editions::COUNT));
}

static void queryStringIsEncoded() {
  char q[96];
  CHECK(editions::queryString(0, q, sizeof q) && !strcmp(q, "&hl=pt-PT&gl=PT&ceid=PT%3Apt-150"));
  CHECK(editions::queryString(7, q, sizeof q) && !strcmp(q, "&hl=es-419&gl=MX&ceid=MX%3Aes-419"));
  CHECK(editions::queryString(2, q, sizeof q) && !strcmp(q, "&hl=en-GB&gl=GB&ceid=GB%3Aen"));
  CHECK(editions::queryString(99, q, sizeof q) && !strcmp(q, "&hl=pt-PT&gl=PT&ceid=PT%3Apt-150"));  // out of the list: the default
  CHECK(editions::queryString(-5, q, sizeof q) && !strcmp(q, "&hl=pt-PT&gl=PT&ceid=PT%3Apt-150"));
  // for every entry: only unreserved characters, '&', '=' and '%XX'; decoding gives the triple back
  for (int i = 0; i < editions::COUNT; i++) {
    CHECK(editions::queryString(i, q, sizeof q));
    for (const char *s = q; *s; s++) CHECK(alnum(*s) || strchr("-_.~&=%", *s));
    CHECK(!strchr(q, ':'));
    std::string want = std::string("&hl=") + editions::LIST[i].hl + "&gl=" + editions::LIST[i].gl + "&ceid=" + editions::LIST[i].ceid;
    CHECK(percentDecode(q) == want);
    CHECK(strlen(q) < 60);
  }
}

static void queryStringNeverOverruns() {
  // the exact fit works, one byte less fails with an empty string, whatever the size; the bytes after the buffer are never touched
  char exact[96];
  CHECK(editions::queryString(0, exact, sizeof exact));
  size_t need = strlen(exact) + 1;
  for (size_t cap = 0; cap <= need + 4; cap++) {
    char buf[130];
    memset(buf, 0x5A, sizeof buf);
    bool ok = editions::queryString(0, buf, cap);
    CHECK(ok == (cap >= need));
    if (cap > 0 && !ok) CHECK(buf[0] == 0);
    if (ok) CHECK(!strcmp(buf, exact));
    for (size_t i = cap; i < sizeof buf; i++) CHECK((unsigned char)buf[i] == 0x5A);  // nothing written past the end
  }
  for (int i = 0; i < editions::COUNT; i++) {  // the longest entry too
    char buf[200];
    CHECK(editions::queryString(i, buf, sizeof buf));
    size_t n = strlen(buf) + 1;
    char tight[200];
    memset(tight, 0x5A, sizeof tight);
    CHECK(editions::queryString(i, tight, n));
    CHECK(!editions::queryString(i, tight, n - 1));
  }
}

int main() {
  struct { const char *name; void (*fn)(); } cases[] = {
      {"entries are pinned (append-only)", entriesArePinned},
      {"the default is the service's own", defaultIsTheServicesOwn},
      {"every entry is well formed", everyEntryIsWellFormed},
      {"index checks and the fallback", indexChecks},
      {"the query string is encoded", queryStringIsEncoded},
      {"the query string never overruns", queryStringNeverOverruns},
  };
  int failed = 0, total = (int)(sizeof cases / sizeof cases[0]);
  for (auto &c : cases) {
    gFails = 0;
    c.fn();
    printf("%s  %s\n", gFails ? "FAIL" : "PASS", c.name);
    if (gFails) failed++;
  }
  printf("\n%d of %d cases passed\n", total - failed, total);
  return failed ? 1 : 0;
}
