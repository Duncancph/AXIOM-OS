#!/usr/bin/env python3
# Encodes test tones with AXIOM's SBC encoder (build/test/test_sbc), decodes them with ffmpeg and checks the
# signal-to-noise ratio of both channels (best gain and delay fitted). Needs ffmpeg and numpy.
import subprocess, sys, numpy as np, os, tempfile
exe = sys.argv[1] if len(sys.argv) > 1 else 'build/test/test_sbc'
cases = [(500, 7000, 12000, 44100), (1000, 3000, 20000, 44100), (5000, 15000, 4000, 44100), (2000, 10000, 12000, 48000), (440, 19000, 30000, 44100)]
fails = 0
d = tempfile.mkdtemp()
for fl, fr, amp, rate in cases:
    sbc, raw = os.path.join(d, 't.sbc'), os.path.join(d, 't.raw')
    subprocess.run([exe, sbc, str(fl), str(fr), str(amp), str(rate)], check=True)
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'sbc', '-i', sbc, '-f', 's16le', '-ac', '2', raw], check=True)
    y = np.frombuffer(open(raw, 'rb').read(), dtype='<i2').reshape(-1, 2).astype(float)
    t = np.arange(len(y))
    for ch, fq in ((0, fl), (1, fr)):
        x = np.round(amp * np.sin(2 * np.pi * fq * t / rate))
        best = -99
        for dl in range(60, 90):
            a = x[4000:20000]; b = y[4000 + dl:20000 + dl, ch]
            g = np.dot(a, b) / np.dot(a, a); r = b - g * a
            best = max(best, 10 * np.log10(np.mean((g * a) ** 2) / np.mean(r ** 2)) if abs(g - 1) < 0.02 else -99)
        ok = best > 50
        fails += not ok
        print('%s %5d Hz ch%d amp %5d @%d: SNR %.1f dB' % ('PASS' if ok else 'FAIL', fq, ch, amp, rate, best))
print('SBC: %d failed' % fails)
sys.exit(1 if fails else 0)
