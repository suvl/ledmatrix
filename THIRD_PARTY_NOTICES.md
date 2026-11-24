# Third-party notices

This project contains or links to the following third-party material.

## Adafruit GFX Library and Tom Thumb font (pixel font data)

`shared/fonts.json` holds the pixel data of the default 5x7 font of the Adafruit GFX Library and of the
Tom Thumb 3x5 font, extracted by `tools/extract_fonts.py`. The data is also embedded in the generated
files `preview/index.html` and `arduino/ledmatrix/portal_page.h`. The firmware is compiled against the
Adafruit GFX and Adafruit Protomatter libraries. The licence texts that apply to the font data follow.

### Adafruit GFX Library

```
Software License Agreement (BSD License)

Copyright (c) 2012 Adafruit Industries.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

### Tom Thumb 3x5 font (as included in the Adafruit GFX Library)

```
The original 3x5 font is licensed under the 3-clause BSD license:

Copyright 1999 Brian J. Swetland
Copyright 1999 Vassilii Khachaturov
Portions (of vt100.c/vt100.h) copyright Dan Marks

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions, and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions, and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Modifications to Tom Thumb for improved readability are from Robey Pointer,
see:
http://robey.lag.net/2010/01/23/tiny-monospace-font.html

The original author does not have any objection to relicensing of Robey
Pointer's modifications (in this file) in a more permissive license.  See
the discussion at the above blog, and also here:
http://opengameart.org/forumtopic/how-to-submit-art-using-the-3-clause-bsd-license

Feb 21, 2016: Conversion from Linux BDF --> Adafruit GFX font,
with the help of this Python script:
https://gist.github.com/skelliam/322d421f028545f16f6d
William Skellenger (williamj@skellenger.net)
Twitter: @skelliam

Jan 09, 2020: Bitmaps now compressed, to fix the bounding box problem,
because non-compressed the calculated text width were wrong.
Andreas Merkle (web@blue-andi.de)
```

## Libraries used but not included in this repository

- Adafruit Protomatter (BSD), ArduinoJson (MIT), arduino-esp32 (LGPL 2.1): fetched by `arduino-cli` when the firmware is built.
- Leaflet 1.9.4 (BSD-2-Clause): loaded by the settings page from unpkg in the visitor's browser, pinned with an integrity hash.
- @photostructure/tz-lookup 11.8.0 (CC0-1.0, https://github.com/photostructure/tz-lookup): turns the coordinates of the pin on the map into a time zone name, offline in the visitor's browser. Loaded by the settings page from jsDelivr (`https://cdn.jsdelivr.net/npm/@photostructure/tz-lookup@11.8.0/tz.js`), pinned with an integrity hash, not bundled. Its boundary data comes from timezone-boundary-builder (ODbL, built on OpenStreetMap data).
- Map tiles and place search: (c) OpenStreetMap contributors, loaded by the visitor's browser.

## Time zone data

`shared/tz_zones.json` holds the POSIX rule string of each time zone name, read from the tz database
(IANA time zone database, public domain; the version is in `shared/tz_zones.version.txt`) by
`tools/make_tz.py`. The data is also embedded in the generated file `arduino/ledmatrix/portal_page.h`.

## Data sources (see also the README)

IPMA (weather, must be cited as the source), adsb.fi open data (positions, personal non-commercial use),
adsbdb (routes), Google News (headlines, unofficial feed).

Airline logos are trademarks of their airlines. They are not included in this repository or the published image; the owner converts them privately for personal use.
