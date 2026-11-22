// A live plane, shaped like the example flights so the same drawing code serves both.
#pragma once
#include <stdio.h>

#include "live.h"
#include "sample_data.h"

struct PlaneView {
  SampleFlight f;
  char alt[12], spd[8], vr[12];
};

inline void planeToView(const LivePlane &p, PlaneView &v) {
  snprintf(v.alt, sizeof(v.alt), "%.1fkft", p.alt / 1000.0);
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
  v.f.logo = (LogoKind)((p.ai[0] + p.ai[1]) % 4);  // placeholder art until real logos exist
}
