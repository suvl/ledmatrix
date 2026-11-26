// Host test for arduino/ledmatrix/devicename.h (the display name: DNS label rules, normalisation, the plain sentences; ticket 45).
// Run from the repo root:
//   c++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -Iarduino/ledmatrix tools/test_devicename.cpp -o <scratch>/test_devicename && <scratch>/test_devicename
// Names in here are made up.
#include <stdio.h>
#include <string.h>
#include <string>

#include "devicename.h"

using devicename::Error;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("    FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTION(name) printf("%s\n", name)

// Runs normalise on a C string and returns the error; `out` gets the result. A buffer of exactly MAX_LEN + 1 bytes with guard bytes
// after it: an overrun shows up as a changed guard (and the address sanitizer would catch it too).
struct Run {
  char out[devicename::MAX_LEN + 1];
  unsigned char guard[16];
  Error e;
  explicit Run(const std::string &in) {
    memset(out, 'Z', sizeof out);
    memset(guard, 0xA5, sizeof guard);
    e = devicename::normalise(in.data(), in.size(), out);
  }
  bool guardOk() const {
    for (unsigned char g : guard)
      if (g != 0xA5) return false;
    return true;
  }
};

static bool accepts(const std::string &in, const char *want) {
  Run r(in);
  return r.e == Error::None && strcmp(r.out, want) == 0 && r.guardOk();
}
static bool rejects(const std::string &in, Error want) {
  Run r(in);
  return r.e == want && r.out[0] == 0 && r.guardOk();  // an error leaves an empty name behind
}

int main() {
  SECTION("constants");
  CHECK(devicename::MAX_LEN == 24);
  CHECK(strcmp(devicename::DEFAULT_NAME, "ledmatrix") == 0);
  CHECK(devicename::storedOk(devicename::DEFAULT_NAME));  // the default is itself a valid name

  SECTION("good names pass unchanged");
  CHECK(accepts("ledmatrix", "ledmatrix"));
  CHECK(accepts("kitchen", "kitchen"));
  CHECK(accepts("a", "a"));                  // 1 character: the shortest
  CHECK(accepts("9", "9"));                  // a digit alone is a valid DNS label
  CHECK(accepts("living-room-2", "living-room-2"));
  CHECK(accepts("a-b", "a-b"));
  CHECK(accepts("a--b", "a--b"));            // doubled hyphens are allowed by the DNS rules
  CHECK(accepts("2nd-floor", "2nd-floor"));  // may start with a digit
  CHECK(accepts("ledmatrix-2", "ledmatrix-2"));

  SECTION("length: 1 to 24");
  CHECK(accepts(std::string(24, 'a'), std::string(24, 'a').c_str()));
  CHECK(rejects(std::string(25, 'a'), Error::TooLong));
  CHECK(rejects(std::string(200, 'a'), Error::TooLong));
  CHECK(accepts("a" + std::string(22, '-') + "b", ("a" + std::string(22, '-') + "b").c_str()));  // 24 with hyphens inside
  CHECK(rejects("", Error::Empty));

  SECTION("upper case becomes lower case");
  CHECK(accepts("Kitchen", "kitchen"));
  CHECK(accepts("KITCHEN", "kitchen"));
  CHECK(accepts("LedMatrix-2", "ledmatrix-2"));
  CHECK(accepts("A1B2", "a1b2"));
  CHECK(accepts("Z", "z"));
  CHECK(accepts("AZaz09-", "") == false);     // a trailing hyphen is still refused after the case rule
  CHECK(accepts("AZaz09", "azaz09"));         // the ends of both letter ranges and of the digits

  SECTION("spaces and tabs at the ends are dropped, inside they are refused");
  CHECK(accepts("  kitchen  ", "kitchen"));
  CHECK(accepts("\tkitchen\t", "kitchen"));
  CHECK(accepts(" \t Kitchen \t ", "kitchen"));
  CHECK(rejects("   ", Error::Empty));
  CHECK(rejects("\t", Error::Empty));
  CHECK(rejects("my kitchen", Error::BadChar));
  CHECK(rejects("kitchen\n", Error::BadChar));  // a line end is not a space to drop
  CHECK(rejects("\rkitchen", Error::BadChar));
  CHECK(accepts(std::string(24, 'a') + "   ", std::string(24, 'a').c_str()));  // the length counts the trimmed name
  CHECK(rejects(std::string(25, 'a') + " ", Error::TooLong));

  SECTION("hyphens at either end");
  CHECK(rejects("-kitchen", Error::Hyphen));
  CHECK(rejects("kitchen-", Error::Hyphen));
  CHECK(rejects("-", Error::Hyphen));
  CHECK(rejects("--", Error::Hyphen));
  CHECK(rejects("-a-", Error::Hyphen));
  CHECK(rejects("  -kitchen  ", Error::Hyphen));  // judged after trimming
  CHECK(rejects("KITCHEN-", Error::Hyphen));

  SECTION("anything else is refused");
  for (const char *bad : {"kitchen.local", "a.b", "kitchen_1", "kit/chen", "kit:chen", "kit@chen", "kit!chen", "kit\"chen", "kit'chen",
                          "kit\\chen", "kit<chen", "kit&chen", "kit%20chen", "kit*chen", "kit+chen", "kit=chen", "kit~chen", "k\x7f"})
    CHECK(rejects(bad, Error::BadChar));
  CHECK(rejects("caf\xc3\xa9", Error::BadChar));        // UTF-8 'é'
  CHECK(rejects("\xff", Error::BadChar));                // a byte with the top bit set: the char type may be signed
  CHECK(rejects("\x80" "abc", Error::BadChar));
  CHECK(rejects(std::string("kit\0chen", 8), Error::BadChar));  // an embedded NUL is not the end of the name
  CHECK(rejects(std::string("\0", 1), Error::BadChar));
  CHECK(rejects(std::string("abc\0", 4), Error::BadChar));
  for (int c = 0; c < 256; c++) {  // every byte alone: only letters, digits and (inside a longer name) the hyphen pass
    std::string one(1, (char)c);
    bool letterOrDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    Run r(one);
    if (letterOrDigit) {
      CHECK(r.e == Error::None);
    } else if (c == ' ' || c == '\t') {
      CHECK(r.e == Error::Empty);  // dropped by the trim
    } else if (c == '-') {
      CHECK(r.e == Error::Hyphen);
    } else {
      CHECK(r.e == Error::BadChar);
    }
    CHECK(r.guardOk());
    std::string mid = "a" + one + "b";  // and between two letters: only the space and the tab are refused as well, the hyphen passes
    Run m(mid);
    CHECK((letterOrDigit || c == '-') == (m.e == Error::None));
    CHECK(m.guardOk());
  }

  SECTION("which error comes first");
  CHECK(rejects(std::string(30, '_'), Error::TooLong));  // length before characters
  CHECK(rejects("-_-", Error::BadChar));                  // characters before the hyphen rule
  CHECK(rejects("", Error::Empty));

  SECTION("an error leaves an empty name, a good name is NUL-ended");
  {
    char buf[devicename::MAX_LEN + 1];
    memset(buf, 'Z', sizeof buf);
    CHECK(devicename::normalise("bad name", 8, buf) == Error::BadChar);
    CHECK(buf[0] == 0);
    memset(buf, 'Z', sizeof buf);
    CHECK(devicename::normalise(std::string(24, 'q').c_str(), 24, buf) == Error::None);
    CHECK(strlen(buf) == 24 && buf[24] == 0);
    memset(buf, 'Z', sizeof buf);
    CHECK(devicename::normalise("AbC", 3, buf) == Error::None);
    CHECK(strcmp(buf, "abc") == 0);
    // the length given is what counts, not a NUL: a longer string cut to 3 bytes
    memset(buf, 'Z', sizeof buf);
    CHECK(devicename::normalise("abcdef", 3, buf) == Error::None);
    CHECK(strcmp(buf, "abc") == 0);
    // idempotent: a normalised name normalises to itself
    char again[devicename::MAX_LEN + 1];
    CHECK(devicename::normalise(buf, strlen(buf), again) == Error::None && strcmp(again, buf) == 0);
  }

  SECTION("a stored name must already be in the normalised form");
  CHECK(devicename::storedOk("kitchen"));
  CHECK(devicename::storedOk("a-b-2"));
  CHECK(!devicename::storedOk(""));            // nothing stored yet: the default stays
  CHECK(!devicename::storedOk("Kitchen"));     // not what this module writes: damaged or hand-made, ignored
  CHECK(!devicename::storedOk(" kitchen"));
  CHECK(!devicename::storedOk("kitchen "));
  CHECK(!devicename::storedOk("-kitchen"));
  CHECK(!devicename::storedOk("kitchen-"));
  CHECK(!devicename::storedOk("kit.chen"));
  CHECK(!devicename::storedOk(std::string(25, 'a').c_str()));
  CHECK(devicename::storedOk(std::string(24, 'a').c_str()));

  SECTION("the sentences are plain and safe to put in a JSON string");
  for (Error e : {Error::NotText, Error::Empty, Error::TooLong, Error::BadChar, Error::Hyphen}) {
    const char *m = devicename::message(e);
    CHECK(m && strlen(m) > 20);
    CHECK(strncmp(m, "The display name ", 17) == 0);  // same voice as "The flight layout must be A, B or C."
    CHECK(m[strlen(m) - 1] == '.');
    for (const char *p = m; *p; p++) {
      CHECK((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f);  // no control character, plain ASCII
      CHECK(*p != '"' && *p != '\\');                                // would break the unescaped JSON in net.cpp
    }
  }
  CHECK(strlen(devicename::message(Error::None)) == 0);
  CHECK(strstr(devicename::message(Error::TooLong), "24") != nullptr);  // the sentence names the real limit

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
