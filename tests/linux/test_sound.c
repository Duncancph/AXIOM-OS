/* Build: gcc -O1 -o sndtest tests/linux/test_sound.c -lm -l:libasound.so.2 (no headers needed). Needs the
 * ALSA configuration in the Linux root: /usr/share/alsa (from libasound2-data) and /etc/asound.conf
 * (tests/linux/asound.conf). Run on AXIOM: 'run sndtest'. tests: 48 kHz stereo blocking, 22.05 kHz mono through
 * libasound's converter (mmap), and non-blocking with poll. */
/* Plays tones through libasound (ALSA) on AXIOM: blocking at 48 kHz stereo, through the plug converter at
 * 22.05 kHz mono, and non-blocking with poll (as Chromium does). Prints what happens. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <poll.h>
#include <errno.h>
typedef struct snd_pcm snd_pcm_t;
int snd_pcm_open(snd_pcm_t **, const char *, int, int);
int snd_pcm_set_params(snd_pcm_t *, int fmt, int access, unsigned ch, unsigned rate, int resample, unsigned latency_us);
long snd_pcm_writei(snd_pcm_t *, const void *, unsigned long);
int snd_pcm_drain(snd_pcm_t *);
int snd_pcm_close(snd_pcm_t *);
long snd_pcm_avail_update(snd_pcm_t *);
int snd_pcm_poll_descriptors_count(snd_pcm_t *);
int snd_pcm_poll_descriptors(snd_pcm_t *, struct pollfd *, unsigned);
int snd_pcm_recover(snd_pcm_t *, int, int);
int snd_pcm_delay(snd_pcm_t *, long *);
int snd_pcm_get_params(snd_pcm_t *, unsigned long *buffer, unsigned long *period);
const char *snd_strerror(int);
int snd_card_next(int *);
#define S16_LE 2
#define RW_INTERLEAVED 3
static int fails;
#define OK(what, r) do { long _r = (r); if (_r < 0) { printf("FAIL %s: %s (%ld)\n", what, snd_strerror((int)_r), _r); fails++; } else printf("ok   %s = %ld\n", what, _r); } while (0)

static void play(const char *name, unsigned rate, int ch, double hz, double secs, int nonblock) {
    snd_pcm_t *h;
    int r = snd_pcm_open(&h, name, 0, nonblock ? 1 : 0);   /* SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK */
    OK("open", r); if (r < 0) return;
    OK("set_params", snd_pcm_set_params(h, S16_LE, RW_INTERLEAVED, ch, rate, 1, 100000));
    unsigned long buf, per; OK("get_params", snd_pcm_get_params(h, &buf, &per));
    printf("     buffer %lu frames, period %lu frames\n", buf, per);
    long frames = (long)(rate * secs), done = 0, waits = 0;
    short *pcm = malloc(frames * ch * 2);
    for (long i = 0; i < frames; i++) for (int c = 0; c < ch; c++) pcm[i * ch + c] = (short)(8000 * sin(2 * M_PI * hz * i / rate));
    struct pollfd pf[4]; int np = snd_pcm_poll_descriptors_count(h); snd_pcm_poll_descriptors(h, pf, np);
    long it = 0;
    while (done < frames) {
        if (nonblock && ++it % 200 == 0) { long dd = 0; int r = snd_pcm_delay(h, &dd); printf("     [%ld] done %ld avail %ld delay %ld (r %d)\n", it, done, snd_pcm_avail_update(h), dd, r); if (it > 3000) break; }
        if (nonblock) {
            long a = snd_pcm_avail_update(h);
            if (a < 0) { OK("avail_update", a); snd_pcm_recover(h, (int)a, 1); continue; }
            if (a <= 0) { poll(pf, np, 100); waits++; continue; }
        }
        long n = snd_pcm_writei(h, pcm + done * ch, frames - done < 1024 ? frames - done : 1024);
        if (n == -EAGAIN) continue;
        if (n < 0) { OK("writei", n); if (snd_pcm_recover(h, (int)n, 1) < 0) break; continue; }
        done += n;
    }
    long d = 0; snd_pcm_delay(h, &d);
    printf("     wrote %ld frames (%d Hz %d ch, %.0f Hz tone)%s, delay %ld\n", done, rate, ch, hz, nonblock ? ", non-blocking" : "", d);
    if (nonblock) printf("     waited in poll %ld times\n", waits);
    if (nonblock) { /* drain needs blocking mode in a non-blocking stream: wait for the delay instead */ int k = 0; while (snd_pcm_delay(h, &d) == 0 && d > 0 && k++ < 300) poll(0, 0, 10); printf("     drained: delay %ld after %d waits\n", d, k); }
    else OK("drain", snd_pcm_drain(h));
    OK("close", snd_pcm_close(h));
    free(pcm);
}
int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    int card = -1; OK("card_next", snd_card_next(&card)); printf("     card %d\n", card);
    play("default", 48000, 2, 440, 2.0, 0);
    play("default", 22050, 1, 660, 1.5, 0);
    play("default", 48000, 2, 880, 1.5, 1);
    printf(fails ? "FAILED: %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
