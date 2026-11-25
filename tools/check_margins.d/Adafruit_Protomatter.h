// A fake panel: a 64x32 RGB565 framebuffer behind the real Adafruit_GFX drawing code.
#pragma once
#include "Adafruit_GFX.h"

class Adafruit_Protomatter : public Adafruit_GFX {
 public:
  uint16_t fb[32][64];
  Adafruit_Protomatter() : Adafruit_GFX(64, 32) { clear(); }
  void clear() { for (auto &row : fb) for (auto &p : row) p = 0; }
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || y < 0 || x >= 64 || y >= 32) return;  // the panel clips like this
    fb[y][x] = c;
  }
  uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
};
