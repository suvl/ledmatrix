// Shared display object and geometry. The Protomatter matrix is created in ledmatrix.ino.
#pragma once
#include <Adafruit_Protomatter.h>

extern Adafruit_Protomatter matrix;

static const int PANEL_W = 64;
static const int PANEL_H = 32;

// Colours used by the screens (same values as the preview page).
inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return matrix.color565(r, g, b);
}
