#!/usr/bin/env python3
"""Generates shared/tz_zones.json, the table zone name -> POSIX TZ rule string that the portal page
embeds (build_web.py fills the placeholder __TZ_TABLE__ with it). The rule is the footer line of the
TZif (v2+) file of the zone in the tz database, e.g. "Europe/Lisbon": "WET0WEST,M3.5.0/1,M10.5.0".
The table is a plain name -> rule object, sorted by name, one entry per line. The tz database
version goes to shared/tz_zones.version.txt and is printed.

Zone names in the table: every name of the tz database's zone.tab (one or more zones per country, the
names people recognise, aliases such as Europe/Amsterdam included) plus Etc/UTC, Etc/GMT and Etc/GMT+-N.
These cover every name that the portal's offline lookup library (@photostructure/tz-lookup, see
THIRD_PARTY_NOTICES.md) can return. Verify that with --check (offline, reads the library file).

Run (one command, offline once the data is present; the tzdata package is the tz database as a pip
package, tzdata 2026.6 = tz database 2026f):
    python3 -m pip install tzdata && python3 tools/make_tz.py
Other source: python3 tools/make_tz.py ZONEINFO_DIR   (a directory of TZif files with zone.tab and
tzdata.zi; the default is the tzdata package if installed, else the operating system's zoneinfo)
Check against the lookup library: python3 tools/make_tz.py --check path/to/tz.js
Compatibility change: the firmware's libc (newlib) cannot parse a negative rule time, so the tz
database footers that contain one are changed to "/0" (see NEWLIB_NEGATIVE_TIME): this affects only
America/Nuuk and America/Scoresbysund, whose DST then starts one hour late (Sunday 00:00 instead of
Saturday 23:00 local time), once a year. The rule times above 24 hours (Cairo, Santiago, Gaza, Hebron,
Jerusalem) are valid for newlib and kept.
Note: the macOS system files are "rearguard" data (positive DST in Ireland, the tzdata package has the
main data with negative DST there); both are valid POSIX rules, but use the tzdata package so that the
result is reproducible.
The tz database is in the public domain."""
import json, os, re, sys, zoneinfo

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(root, 'shared/tz_zones.json')
OUT_VERSION = os.path.join(root, 'shared/tz_zones.version.txt')
ETC = ['Etc/UTC', 'Etc/GMT'] + ['Etc/GMT%+d' % n for n in list(range(-12, 0)) + list(range(1, 13))]


def zoneinfo_dir(arg):
    if arg:
        return arg
    try:
        import tzdata  # the tzdata package keeps its zoneinfo files in tzdata/zoneinfo
        return os.path.join(os.path.dirname(tzdata.__file__), 'zoneinfo')
    except ImportError:
        for p in zoneinfo.TZPATH:
            if os.path.isfile(os.path.join(p, 'zone.tab')):
                return p
    sys.exit('no tz database found: run  python3 -m pip install tzdata  or pass a zoneinfo directory')


def version(d):
    p = os.path.join(d, 'tzdata.zi')  # its first line is "# version 2026f"
    m = re.search(r'\b(\d{4}[a-z])\b', open(p).readline()) if os.path.isfile(p) else None
    return m.group(1) if m else sys.exit('cannot find the tz database version in ' + d)


def footer(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'TZif' and b[4:5] >= b'2', path + ': not a TZif v2+ file'
    rule = b.rstrip(b'\n').rsplit(b'\n', 1)[-1].decode('ascii')
    assert rule and not rule.startswith('TZif'), path + ': no POSIX footer'
    return rule


NEWLIB_NEGATIVE_TIME = re.compile(r'/-\d+(:\d+){0,2}')  # newlib reads "/-1" as 65535 hours


def names(d):
    out = set(ETC)
    for line in open(os.path.join(d, 'zone.tab')):
        if line.strip() and not line.startswith('#'):
            out.add(line.rstrip('\n').split('\t')[2])
    return out


def library_names(path):
    """All zone names the library can return: the array T in its minified source."""
    m = re.search(r'\bT=(\[(?:"[^"]+",?)+\])', open(path).read())
    assert m, 'cannot find the name list in ' + path
    return json.loads(m.group(1))


def check(js):
    table = json.load(open(OUT))
    lib = library_names(js)
    miss = [n for n in lib if n not in table]
    print('library names', len(lib), '- missing from the table:', miss or 'none')
    sys.exit(1 if miss else 0)


def main(argv):
    if argv[:1] == ['--check']:
        check(argv[1])
    d = zoneinfo_dir(argv[0] if argv else None)
    table = {n: footer(os.path.join(d, n)) for n in sorted(names(d))}
    for n, r in table.items():
        if NEWLIB_NEGATIVE_TIME.search(r):
            table[n] = NEWLIB_NEGATIVE_TIME.sub('/0', r)
            print('changed for newlib:', n, r, '->', table[n])
    longest = max(table, key=lambda n: len(table[n]))
    open(OUT, 'w').write(json.dumps(table, indent=1, ensure_ascii=True) + '\n')
    v = version(d)
    open(OUT_VERSION, 'w').write('tz database ' + v + '\n')
    print('tz database', v, 'from', d)
    print('shared/tz_zones.json:', len(table), 'entries,', os.path.getsize(OUT), 'bytes; minified',
          len(json.dumps(table, separators=(',', ':'))), 'bytes; longest rule', len(table[longest]),
          'chars:', longest, table[longest])


if __name__ == '__main__':
    main(sys.argv[1:])
