#!/usr/bin/env python3
"""Write ride.nmea: a made-up 3-minute loop near Milwaukee, as a u-blox would
report it at 1 Hz (RMC, GGA, GSA, GSV). For the PC demo (hbas-gpsd --replay).

    python3 make_ride.py > ride.nmea
"""
import math

START = (43.0389, -87.9065)          # downtown Milwaukee
SATS = [(12, 62, 45, 47), (5, 40, 120, 45), (25, 33, 300, 44), (2, 17, 308, 41),
        (29, 25, 200, 38), (15, 60, 120, 33), (18, 12, 80, 29), (21, 8, 20, 22),
        (31, 5, 250, 0)]


def nmea(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return '$%s*%02X' % (body, c)


def dm(v, deg_digits, pos, neg):
    a = abs(v)
    d = int(a)
    return '%0*d%07.4f' % (deg_digits, d, (a - d) * 60), pos if v >= 0 else neg


def main():
    lat, lon = START
    t0 = 20 * 3600 + 15 * 60
    for i in range(180):
        # speed: pull away, cruise, slow for a corner, every 45 s a quarter turn
        phase = i % 45
        kmh = min(90.0, 6.0 * phase) if phase < 38 else 90.0 - 10.0 * (phase - 37)
        course = (270 + 90 * (i // 45)) % 360
        d = kmh / 3.6                                   # metres this second
        lat += d * math.cos(math.radians(course)) / 111320
        lon += d * math.sin(math.radians(course)) / (111320 * math.cos(math.radians(lat)))
        t = t0 + i
        hh, mm, ss = t // 3600, t // 60 % 60, t % 60
        tm = '%02d%02d%02d.00' % (hh, mm, ss)
        la, ns = dm(lat, 2, 'N', 'S')
        lo, ew = dm(lon, 3, 'E', 'W')
        print(nmea('GNRMC,%s,A,%s,%s,%s,%s,%.1f,%.1f,011026,,,A'
                   % (tm, la, ns, lo, ew, kmh / 1.852, course)))
        print(nmea('GNGGA,%s,%s,%s,%s,%s,1,09,0.8,%.1f,M,-34.0,M,,'
                   % (tm, la, ns, lo, ew, 181.0 + 3 * math.sin(i / 20))))
        print(nmea('GNGSA,A,3,12,05,25,02,29,15,18,21,,,,,1.5,0.8,1.2'))
        sats = [(p, e, a, max(0, s - (i % 7))) if s else (p, e, a, 0) for p, e, a, s in SATS]
        groups = [sats[k:k + 4] for k in range(0, len(sats), 4)]
        for g, grp in enumerate(groups, 1):
            fields = ','.join('%02d,%02d,%03d,%s' % (p, e, a, '%02d' % s if s else '')
                              for p, e, a, s in grp)
            print(nmea('GPGSV,%d,%d,%02d,%s' % (len(groups), g, len(sats), fields)))


if __name__ == '__main__':
    main()
