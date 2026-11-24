// A live plane, shaped like the example flights so the same drawing code serves both.
#pragma once
#include <stdio.h>

#include "live.h"
#include "sample_data.h"

struct PlaneView {
  SampleFlight f;
  char alt[12], spd[8], vr[12];
  uint16_t logoPx[256];
};

// The altitude is carried forward from the moment the position arrived using the vertical speed, one step per second.
inline void planeToView(const LivePlane &p, PlaneView &v, uint32_t nowMs) {
  uint32_t secs = (nowMs - p.at) / 1000;
  if (secs > 300) secs = 300;  // never carry a position further than five minutes
  long alt = p.alt + (long)p.vr * (long)secs;
  if (alt < 0) alt = 0;
  snprintf(v.alt, sizeof(v.alt), "%ldft", alt);
  snprintf(v.spd, sizeof(v.spd), "%d", p.spd);
  snprintf(v.vr, sizeof(v.vr), "%+dft/s", p.vr);
  v.f.airline = p.air[0] ? p.air : p.cs;
  v.f.route = p.route[0] ? p.route : "";
  v.f.type = p.type;
  v.f.originName = p.from;
  v.f.destName = p.to[0] ? p.to : p.cs;
  v.f.alt = v.alt;
  v.f.spdVal = v.spd;
  v.f.spdUnit = "kt";
  v.f.trackDeg = p.trk;
  v.f.vr = v.vr;
  v.f.logo = LogoKind::Globe;
  v.f.code = p.ai;  // non-null marks a live plane: with no logo the text uses the whole width
  v.f.logoPx = liveLogo(p.ai, v.logoPx) ? v.logoPx : nullptr;  // the data task fetched it before queueing the plane
}
