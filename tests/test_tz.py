# Compares DuncOS time zone offsets (src/tz.c) with Python's zoneinfo (IANA tz database), hourly 2024-2030.
import subprocess, sys
from zoneinfo import ZoneInfo
from datetime import datetime, timezone
out = subprocess.run([sys.argv[1]], capture_output=True, text=True).stdout.split("\n")
bad = {}; n = 0
for line in out:
    if not line: continue
    z, t, off = line.split(); t = int(t); off = int(off)
    want = int(datetime.fromtimestamp(t, timezone.utc).astimezone(ZoneInfo(z)).utcoffset().total_seconds() // 60)
    n += 1
    if want != off: bad.setdefault(z, []).append((datetime.fromtimestamp(t, timezone.utc).isoformat(), off, want))
for z, l in bad.items(): print("FAIL %s: %d hours wrong, e.g. %s" % (z, len(l), l[0]))
print("tz: %d zone-hours checked, %d zones wrong" % (n, len(bad)))
sys.exit(1 if bad else 0)
