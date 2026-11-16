#!/usr/bin/env python3
# Decodes the SBC frames the Bluetooth test sent to its simulated speaker (build/test/bt_audio*.sbc) with ffmpeg
# and checks they carry the test tones: 1 kHz left, 3 kHz right.
import subprocess, sys, numpy as np, tempfile, os
fails = 0
for f in ('build/test/bt_audio1.sbc', 'build/test/bt_audio2.sbc'):
    raw = os.path.join(tempfile.mkdtemp(), 'o.raw')
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'sbc', '-i', f, '-f', 's16le', '-ac', '2', raw], check=True)
    y = np.frombuffer(open(raw, 'rb').read(), dtype='<i2').reshape(-1, 2).astype(float)
    seg = y[4000:24000]
    for ch, want, amp in ((0, 1000, 12000), (1, 3000, 8000)):
        F = np.abs(np.fft.rfft(seg[:, ch] * np.hanning(len(seg)))); fr = np.fft.rfftfreq(len(seg), 1 / 44100)
        peak = fr[np.argmax(F)]; level = np.sqrt(2 * np.mean(seg[:, ch] ** 2))
        ok = abs(peak - want) < 5 and abs(level - amp) / amp < 0.03
        fails += not ok
        print('%s %s ch%d: tone %.0f Hz, level %.0f (sent %d Hz at %d)' % ('PASS' if ok else 'FAIL', os.path.basename(f), ch, peak, level, want, amp))
sys.exit(1 if fails else 0)
