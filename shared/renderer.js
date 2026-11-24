const FONTS = __FONTS__;
const W = 64, H = 32, S = 8;

/* ---------- framebuffer and fonts ---------- */
class FB {
  constructor() { this.d = new Uint8Array(W * H * 3); }
  clear() { this.d.fill(0); }
  px(x, y, c) {
    x = Math.round(x); y = Math.round(y);
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const i = (y * W + x) * 3; this.d[i] = c[0]; this.d[i + 1] = c[1]; this.d[i + 2] = c[2];
  }
  rect(x, y, w, h, c) { for (let i = 0; i < w; i++) { this.px(x + i, y, c); this.px(x + i, y + h - 1, c); } for (let j = 0; j < h; j++) { this.px(x, y + j, c); this.px(x + w - 1, y + j, c); } }
  fill(x, y, w, h, c) { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) this.px(x + i, y + j, c); }
}
const WHITE = [255, 255, 255], AMBER = [255, 176, 0], BLUE = [40, 110, 255], GREEN = [0, 220, 90], RED = [255, 40, 40], GREY = [130, 130, 140], CYAN = [0, 200, 230];

function glcdCode(ch) { return ch === '°' ? 248 : ch.charCodeAt(0) & 255; }
function w6(s, size) { return s.length * 6 * (size || 1); }
function wTT(s) { let w = 0; for (const ch of s) { const g = FONTS.tt.glyphs[ch.charCodeAt(0)]; w += g ? g[3] : 4; } return w; }
function drawGlcd(fb, x, y, s, c, size) {
  size = size || 1;
  for (const ch of s) {
    const code = glcdCode(ch);
    for (let col = 0; col < 5; col++) {
      const b = FONTS.glcd[code * 5 + col];
      for (let row = 0; row < 8; row++) if (b & (1 << row)) fb.fill(x + col * size, y + row * size, size, size, c);
    }
    x += 6 * size;
  }
}
function drawTT(fb, x, base, s, c) {
  for (const ch of s) {
    const g = FONTS.tt.glyphs[ch.charCodeAt(0)];
    if (!g) { x += 4; continue; }
    const [off, w, h, adv, xo, yo] = g; let bit = 0;
    for (let yy = 0; yy < h; yy++) for (let xx = 0; xx < w; xx++) {
      if (FONTS.tt.bitmaps[off + (bit >> 3)] & (0x80 >> (bit & 7))) fb.px(x + xo + xx, base + yo + yy, c);
      bit++;
    }
    x += adv;
  }
}
// Draw text that must fit maxW; cut it like the panel would and report it.
function T6(fb, ctx, x, y, s, c, maxW, size) {
  size = size || 1; let t = s;
  while (t.length && w6(t, size) > maxW) t = t.slice(0, -1);
  if (t !== s) ctx.warn('"' + s + '" cut to "' + t + '"');
  drawGlcd(fb, x, y, t, c, size); return t;
}
function TT(fb, ctx, x, base, s, c, maxW) {
  let t = s;
  while (t.length && wTT(t) > maxW) t = t.slice(0, -1);
  if (t !== s) ctx.warn('"' + s + '" cut to "' + t + '"');
  drawTT(fb, x, base, t, c); return t;
}
function centerX(w) { return Math.max(0, Math.floor((W - w) / 2)); }

/* ---------- placeholder art ---------- */
function logo(fb, x, y, kind) {
  for (let j = 0; j < 16; j++) for (let i = 0; i < 16; i++) {
    const dx = i - 7.5, dy = j - 7.5, r = Math.hypot(dx, dy);
    if (r > 7.6) continue;
    let c = null;
    if (kind === 'roundel') {
      const band = 7.5 + 3.2 * Math.sin((i - 7.5) / 7.5 * Math.PI * 0.9);
      c = Math.abs(j - band) < 1.6 ? [235, 235, 245] : (j < band ? [30, 90, 255] : [230, 60, 60]);
    } else if (kind === 'stripes') {
      const k = Math.floor((i + j) / 3) % 3; c = [[235, 60, 60], [235, 235, 245], [40, 90, 220]][k];
    } else if (kind === 'orb') {
      c = (Math.abs(r - 3) < 0.7 || Math.abs(r - 6) < 0.7 || j === 8 || i === 8) ? [190, 205, 255] : [30, 70, 220];
    } else {
      const mer = Math.abs(Math.abs(dx) - 4 * Math.sqrt(Math.max(0, 1 - (dy / 7.5) ** 2))) < 0.7;
      c = (r > 6.6 || mer || Math.abs(dy) < 0.6 || Math.abs(dy - 4) < 0.5 || Math.abs(dy + 4) < 0.5) ? [90, 160, 255] : [10, 40, 140];
    }
    fb.px(x + i, y + j, c);
  }
}
function wxIcon(fb, x, y, kind, s) {
  const k = s || 16, cx = x + k / 2 - 0.5, cy = y + k / 2 - 0.5;
  const sun = (r, col) => { for (let j = -k; j <= k; j++) for (let i = -k; i <= k; i++) if (Math.hypot(i, j) <= r) fb.px(cx + i, cy + j, col); };
  const cloud = (ox, oy, sc, col) => { [[-3, 1, 3], [0, -1, 4], [3, 1, 3], [0, 2, 3]].forEach(([a, b, r]) => { for (let j = -5; j <= 5; j++) for (let i = -6; i <= 6; i++) if (Math.hypot(i - a * sc, j - b * sc) <= r * sc) fb.px(cx + ox + i, cy + oy + j, col); }); };
  const sc = k / 16;
  if (kind === 'sun') {
    sun(3.2 * sc * 1.4, [255, 200, 0]);
    for (let a = 0; a < 8; a++) { const ang = a * Math.PI / 4; for (let r = 5.6 * sc; r < 7.6 * sc; r += 0.5) fb.px(cx + Math.cos(ang) * r, cy + Math.sin(ang) * r, [255, 170, 0]); }
  } else if (kind === 'part') {
    sun(2.6 * sc * 1.4, [255, 200, 0]); cloud(2 * sc, 2 * sc, 0.8 * sc, [200, 210, 225]);
  } else if (kind === 'cloud') {
    cloud(0, 0, 1 * sc, [190, 200, 215]);
  } else {
    cloud(0, -2 * sc, 0.9 * sc, [150, 160, 180]);
    for (let d = -1; d <= 1; d++) { fb.px(cx + d * 4 * sc, cy + 5 * sc, [60, 150, 255]); fb.px(cx + d * 4 * sc - 1, cy + 7 * sc, [60, 150, 255]); }
  }
}

/* ---------- example data ---------- */
const FLIGHTS = [
  { chip: 'Korean · LAX-ICN', air: 'Korean', route: 'LAX-ICN', type: '747-8', from: 'Los Angeles Intl', to: 'Seoul Incheon', alt: '34012ft', spd: '540mph', trk: '294°', vr: '+0ft/s', logo: 'roundel' },
  { chip: 'American · DCA-ORD', air: 'American', route: 'DCA-ORD', type: 'E75L', from: 'Washington Reagan', to: "Chicago O'Hare", alt: '9870ft', spd: '330kts', trk: '305°', vr: '+1ft/s', logo: 'stripes' },
  { chip: 'United · GUA-LAX', air: 'United', route: 'GUA-LAX', type: '737 MAX 8', from: 'Guatemala Aurora', to: 'Los Angeles Intl', alt: '9812ft', spd: '367mph', trk: '279°', vr: '-7ft/s', logo: 'orb' },
  { chip: 'Pan Am · JFK>SEA', air: 'PAN AM', route: 'JFK>SEA', type: 'B787-8', from: 'John F Kennedy Intl', to: 'Seattle Tacoma Intl', alt: '12240ft', spd: '380mph', trk: '301°', vr: '+9ft/s', logo: 'globe' },
];
const HEADLINES = ['City council approves new tram line for the riverside', 'Local bakery wins national bread award', 'Heavy rain expected across the north tonight'];
const state = { flight: 0, style: 'dots', bright: 1, paused: false };

/* ---------- screen drawings ---------- */
const flightA = (fb, t, ctx) => {
  const f = FLIGHTS[state.flight];
  logo(fb, 1, 0, f.logo);
  T6(fb, ctx, 19, 1, f.air, WHITE, 44);
  T6(fb, ctx, 19, 9, f.route, CYAN, 44);
  T6(fb, ctx, 1, 17, f.type, AMBER, 62);
  TT(fb, ctx, 1, 30, f.alt + ' ' + f.spd, GREY, 62);
};
const flightB = (fb, t, ctx) => {
  const f = FLIGHTS[state.flight];
  logo(fb, 1, 0, f.logo);
  T6(fb, ctx, 19, 1, f.air, WHITE, 44);
  T6(fb, ctx, 19, 9, f.route, CYAN, 44);
  TT(fb, ctx, 1, 23, f.type, AMBER, 62);
  TT(fb, ctx, 1, 30, 'to ' + f.to, GREY, 62);
};
const flightC = (fb, t, ctx) => {
  const f = FLIGHTS[state.flight];
  T6(fb, ctx, 2, 1, f.air, WHITE, 60);
  T6(fb, ctx, 2, 9, f.route, CYAN, 60);
  TT(fb, ctx, 2, 21, f.type, AMBER, 60);
  TT(fb, ctx, 2, 27, f.alt + ' ' + f.spd, GREY, 60);
  TT(fb, ctx, 2, 32, f.trk + ' ' + f.vr, GREY, 60);
};
const wxA = (fb, t, ctx) => {
  wxIcon(fb, 1, 2, 'part', 16);
  T6(fb, ctx, 21, 3, '18', WHITE, 26, 2); drawGlcd(fb, 45, 3, '°', WHITE, 1); drawGlcd(fb, 51, 3, 'C', WHITE, 1);
  TT(fb, ctx, 2, 25, 'Partly cloudy', AMBER, 60);
  TT(fb, ctx, 2, 31, 'H21 L12 rain 10%', GREY, 60);
};
const wxB = (fb, t, ctx) => {
  TT(fb, ctx, 2, 7, 'HOME', AMBER, 60);
  T6(fb, ctx, centerX(w6('18°C', 2)), 10, '18°C', WHITE, 62, 2);
  TT(fb, ctx, centerX(wTT('Partly cloudy')), 31, 'Partly cloudy', GREY, 62);
};
const wxC = (fb, t, ctx) => {
  const cols = [['21h', 'part', '18'], ['00h', 'sun', '21'], ['03h', 'rain', '15']];
  cols.forEach(([lab, ic, tmp], i) => {
    const x0 = 2 + i * 21;
    TT(fb, ctx, x0 + Math.floor((20 - wTT(lab)) / 2), 7, lab, AMBER, 20);  // wTT counts the last gap, the panel's ttWidth does not
    wxIcon(fb, x0 + 3, 10, ic, 12);
    TT(fb, ctx, x0 + Math.floor((19 - wTT(tmp + '°')) / 2), 29, tmp + '°', WHITE, 20);
  });
};
const newsA = (fb, t, ctx) => {
  const h = HEADLINES[Math.floor(t / 14) % HEADLINES.length];
  fb.fill(0, 0, W, 8, [0, 40, 90]); TT(fb, ctx, 2, 6, 'NEWS', WHITE, 60);
  const wpx = w6(h, 1), x = W - ((t % 14) / 14) * (W + wpx);
  drawGlcd(fb, Math.round(x), 16, h, AMBER, 1);
  fb.fill(0, 0, 1, H, [0, 0, 0]);
};
const wrap = (s, maxChars) => { const out = []; let cur = ''; s.split(' ').forEach(w => { if ((cur + ' ' + w).trim().length > maxChars) { out.push(cur); cur = w; } else cur = (cur + ' ' + w).trim(); }); out.push(cur); return out; };
const newsB = (fb, t, ctx) => {
  const h = HEADLINES[Math.floor(t / 4) % HEADLINES.length];
  TT(fb, ctx, 2, 6, 'NEWS', BLUE, 60);
  wrap(h, 15).slice(0, 4).forEach((l, i) => TT(fb, ctx, 2, 13 + i * 6, l, WHITE, 62));
};
const idleA = (fb, t, ctx) => {
  const d = new Date(), hh = String(d.getHours()).padStart(2, '0'), mm = String(d.getMinutes()).padStart(2, '0');
  const colon = Math.floor(t * 2) % 2 === 0 ? ':' : ' ';
  const s = hh + colon + mm;
  T6(fb, ctx, centerX(w6(s, 2)), 4, s, WHITE, 64, 2);
  const date = d.toLocaleDateString('en-GB', { weekday: 'short', day: '2-digit', month: 'short' }).replace(',', '').toUpperCase();
  TT(fb, ctx, centerX(wTT(date)), 29, date, GREY, 62);
};
const idleB = (fb, t, ctx) => {
  TT(fb, ctx, centerX(wTT('NO FLIGHTS')), 6, 'NO FLIGHTS', GREY, 62);
  const cx = 31.5, cy = 20, R = 10, ang = (t * 1.4) % (Math.PI * 2);
  for (let a = 0; a < 40; a++) { const aa = ang - a * 0.05, br = Math.max(0, 1 - a / 40); for (let r = 1; r <= R; r++) fb.px(cx + Math.cos(aa) * r, cy + Math.sin(aa) * r, [0, 120 * br, 40 * br]); }
  for (let a = 0; a < 40; a++) fb.px(cx + Math.cos(a / 40 * 6.283) * R, cy + Math.sin(a / 40 * 6.283) * R, [20, 60, 30]);
  fb.px(cx, cy, GREEN);
};
const idleC = (fb, t) => {
  for (let i = 0; i < 3; i++) { const v = 0.35 + 0.65 * Math.max(0, Math.sin(t * 2.5 - i * 0.9)); fb.fill(24 + i * 8, 15, 3, 3, [255 * v, 176 * v, 0]); }
};
const setupA = (fb, t, ctx) => {
  TT(fb, ctx, 2, 6, 'SETUP MODE', AMBER, 62);
  TT(fb, ctx, 2, 14, 'WIFI ledmatrix', WHITE, 62);
  TT(fb, ctx, 2, 21, 'PASS 7K3PQ9XM', WHITE, 62);
  TT(fb, ctx, 2, 28, 'OPEN 192.168.4.1', CYAN, 62);
};
const setupB = (fb, t, ctx) => {
  T6(fb, ctx, 2, 0, 'ledmatrix', AMBER, 62);
  TT(fb, ctx, 2, 14, 'WIFI PASSWORD', GREY, 62);
  T6(fb, ctx, 2, 16, '7K3PQ9XM', WHITE, 62);
  TT(fb, ctx, 2, 30, '192.168.4.1', CYAN, 62);
};
const boot = (fb, t, ctx) => {
  T6(fb, ctx, centerX(w6('ledmatrix', 1)), 7, 'ledmatrix', WHITE, 62);
  const dots = '.'.repeat(1 + Math.floor(t * 2) % 3);
  TT(fb, ctx, 14, 24, 'connecting' + dots, GREY, 50);
};
const connected = (fb, t, ctx) => {
  TT(fb, ctx, 2, 8, 'WIFI OK', GREEN, 60); TT(fb, ctx, 2, 16, '192.168.1.100', WHITE, 62); TT(fb, ctx, 2, 24, 'ledmatrix.local', CYAN, 62);
};
const stale = (fb, t, ctx) => {
  wxIcon(fb, 4, 8, 'part', 16);
  T6(fb, ctx, 26, 10, '18', WHITE, 26, 2); drawGlcd(fb, 50, 10, '°', WHITE, 1);
  if (Math.floor(t * 1.5) % 2 === 0) fb.fill(60, 1, 3, 3, [255, 120, 20]);
  TT(fb, ctx, 2, 30, 'stale data dot', GREY, 62);
};
const nowifi = (fb, t, ctx) => {
  for (let i = 0; i < 12; i++) { fb.px(3 + i, 9 + i, RED); fb.px(14 - i, 9 + i, RED); fb.px(4 + i, 9 + i, RED); fb.px(13 - i, 9 + i, RED); }
  TT(fb, ctx, 20, 10, 'NO WIFI', RED, 44); TT(fb, ctx, 20, 17, 'hold UP 5s', WHITE, 44); TT(fb, ctx, 20, 24, 'to set up', GREY, 44);
};
