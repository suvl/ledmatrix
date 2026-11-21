#include "art.h"

#include <math.h>

#include "display.h"

static void px(float x, float y, uint16_t c) {
  matrix.drawPixel((int16_t)lroundf(x), (int16_t)lroundf(y), c);
}

void drawLogo(int x, int y, LogoKind kind) {
  for (int j = 0; j < 16; j++) {
    for (int i = 0; i < 16; i++) {
      float dx = i - 7.5f, dy = j - 7.5f, r = hypotf(dx, dy);
      if (r > 7.6f) continue;
      uint16_t c = 0;
      switch (kind) {
        case LogoKind::Roundel: {
          float band = 7.5f + 3.2f * sinf((i - 7.5f) / 7.5f * (float)M_PI * 0.9f);
          if (fabsf(j - band) < 1.6f) c = rgb(235, 235, 245);
          else if (j < band) c = rgb(30, 90, 255);
          else c = rgb(230, 60, 60);
          break;
        }
        case LogoKind::Stripes: {
          int k = ((i + j) / 3) % 3;
          c = k == 0 ? rgb(235, 60, 60) : (k == 1 ? rgb(235, 235, 245) : rgb(40, 90, 220));
          break;
        }
        case LogoKind::Orb: {
          bool line = fabsf(r - 3) < 0.7f || fabsf(r - 6) < 0.7f || j == 8 || i == 8;
          c = line ? rgb(190, 205, 255) : rgb(30, 70, 220);
          break;
        }
        case LogoKind::Globe: {
          float s = 1.0f - (dy / 7.5f) * (dy / 7.5f);
          float mer = fabsf(fabsf(dx) - 4.0f * sqrtf(s > 0 ? s : 0));
          bool light = r > 6.6f || mer < 0.7f || fabsf(dy) < 0.6f || fabsf(dy - 4) < 0.5f || fabsf(dy + 4) < 0.5f;
          c = light ? rgb(90, 160, 255) : rgb(10, 40, 140);
          break;
        }
      }
      matrix.drawPixel(x + i, y + j, c);
    }
  }
}

static void disc(float cx, float cy, float r, uint16_t col, int span) {
  for (int j = -span; j <= span; j++)
    for (int i = -span; i <= span; i++)
      if (hypotf((float)i, (float)j) <= r) px(cx + i, cy + j, col);
}

static void cloud(float cx, float cy, float ox, float oy, float sc, uint16_t col) {
  static const int8_t blobs[4][3] = {{-3, 1, 3}, {0, -1, 4}, {3, 1, 3}, {0, 2, 3}};
  for (const auto &b : blobs)
    for (int j = -5; j <= 5; j++)
      for (int i = -6; i <= 6; i++)
        if (hypotf(i - b[0] * sc, j - b[1] * sc) <= b[2] * sc) px(cx + ox + i, cy + oy + j, col);
}

void drawWxIcon(int x, int y, WxKind kind, int size) {
  float k = (float)size, sc = k / 16.0f;
  float cx = x + k / 2 - 0.5f, cy = y + k / 2 - 0.5f;
  switch (kind) {
    case WxKind::Sun:
      disc(cx, cy, 3.2f * sc * 1.4f, rgb(255, 200, 0), size);
      for (int a = 0; a < 8; a++) {
        float ang = a * (float)M_PI / 4;
        for (float r = 5.6f * sc; r < 7.6f * sc; r += 0.5f) px(cx + cosf(ang) * r, cy + sinf(ang) * r, rgb(255, 170, 0));
      }
      break;
    case WxKind::PartCloud:
      disc(cx, cy, 2.6f * sc * 1.4f, rgb(255, 200, 0), size);
      cloud(cx, cy, 2 * sc, 2 * sc, 0.8f * sc, rgb(200, 210, 225));
      break;
    case WxKind::Cloud:
      cloud(cx, cy, 0, 0, sc, rgb(190, 200, 215));
      break;
    case WxKind::Rain:
      cloud(cx, cy, 0, -2 * sc, 0.9f * sc, rgb(150, 160, 180));
      for (int d = -1; d <= 1; d++) {
        px(cx + d * 4 * sc, cy + 5 * sc, rgb(60, 150, 255));
        px(cx + d * 4 * sc - 1, cy + 7 * sc, rgb(60, 150, 255));
      }
      break;
  }
}
