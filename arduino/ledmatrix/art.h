// Placeholder 16x16 logos and weather icons, drawn with pixels. Real logos arrive in a later slice.
#pragma once
#include <stdint.h>

enum class LogoKind : uint8_t { Roundel, Stripes, Orb, Globe };
enum class WxKind : uint8_t { Sun, PartCloud, Cloud, Rain };

void drawLogo(int x, int y, LogoKind kind);
void drawWxIcon(int x, int y, WxKind kind, int size);
