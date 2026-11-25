// Host stand-in for the few Arduino things screens.cpp and the Adafruit GFX library use (see tools/check_margins.py).
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <algorithm>
#define PROGMEM
class __FlashStringHelper;
#define F(x) (x)
#define radians(d) ((d) * 0.017453292519943295)

class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned int v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  unsigned int length() const { return (unsigned int)s.size(); }
  const char *c_str() const { return s.c_str(); }
  int indexOf(char c, unsigned int from = 0) const { size_t p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(unsigned int a) const { return String(s.substr(a).c_str()); }
  String substring(unsigned int a, unsigned int b) const { return String(s.substr(a, b - a).c_str()); }
  void remove(unsigned int idx) { s.erase(idx); }
  void toUpperCase() { for (auto &ch : s) ch = (char)toupper((unsigned char)ch); }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o) { s += o; return *this; }
  String &operator+=(char o) { s += o; return *this; }
  String operator+(const String &o) const { String r(*this); r.s += o.s; return r; }
  String operator+(const char *o) const { String r(*this); r.s += o; return r; }
  String operator+(char o) const { String r(*this); r.s += o; return r; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator!=(const String &o) const { return s != o.s; }
};
inline String operator+(const char *a, const String &b) { String r(a); r.s += b.s; return r; }

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *b, size_t n) { size_t k = 0; while (n--) k += write(*b++); return k; }
  size_t print(const char *c) { return write((const uint8_t *)c, strlen(c)); }
  size_t print(const String &s) { return write((const uint8_t *)s.c_str(), s.length()); }
  size_t print(char c) { return write((uint8_t)c); }
};

struct SerialStandIn {
  void printf(const char *fmt, ...) {}  // the TRUNC log lines are not wanted here
};
extern SerialStandIn Serial;

// Arduino defines these as macros; Adafruit_GFX.cpp relies on them.
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
