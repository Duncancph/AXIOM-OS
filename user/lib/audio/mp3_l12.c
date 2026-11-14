/* AXIOM MP3 decoder: Layer I and Layer II (MPEG-1 and MPEG-2 LSF). */
#include "mp3_int.h"

/* Layer II allocation tables (ISO 11172-3 B.2a-d, ISO 13818-3 B.1): for each subband, the number of
   allocation bits and the row of quantizer level counts selected by the allocation index (1..). */
static const uint16_t q_a0[] = { 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535 };
static const uint16_t q_a1[] = { 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535 };
static const uint16_t q_a2[] = { 3, 5, 7, 9, 15, 31, 65535 };
static const uint16_t q_a3[] = { 3, 5, 65535 };
static const uint16_t q_c0[] = { 3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767 };
static const uint16_t q_c1[] = { 3, 5, 9, 15, 31, 63, 127 };
static const uint16_t q_l0[] = { 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383 };
static const uint16_t q_l2[] = { 3, 5, 9 };

struct l2row { uint8_t nbal; const uint16_t *q; };
struct l2tab { int sblimit; struct l2row sb[32]; };

#define R4A0 { 4, q_a0 }
#define R4A1 { 4, q_a1 }
#define R3A2 { 3, q_a2 }
#define R2A3 { 2, q_a3 }
#define R4C0 { 4, q_c0 }
#define R3C1 { 3, q_c1 }
#define R4L0 { 4, q_l0 }
#define R3L1 { 3, q_c1 }
#define R2L2 { 2, q_l2 }
static const struct l2tab l2tabs[5] = {
    { 27, { R4A0, R4A0, R4A0, R4A1, R4A1, R4A1, R4A1, R4A1, R4A1, R4A1, R4A1, R3A2, R3A2, R3A2, R3A2, R3A2,
            R3A2, R3A2, R3A2, R3A2, R3A2, R3A2, R3A2, R2A3, R2A3, R2A3, R2A3 } },
    { 30, { R4A0, R4A0, R4A0, R4A1, R4A1, R4A1, R4A1, R4A1, R4A1, R4A1, R4A1, R3A2, R3A2, R3A2, R3A2, R3A2,
            R3A2, R3A2, R3A2, R3A2, R3A2, R3A2, R3A2, R2A3, R2A3, R2A3, R2A3, R2A3, R2A3, R2A3 } },
    { 8, { R4C0, R4C0, R3C1, R3C1, R3C1, R3C1, R3C1, R3C1 } },
    { 12, { R4C0, R4C0, R3C1, R3C1, R3C1, R3C1, R3C1, R3C1, R3C1, R3C1, R3C1, R3C1 } },
    { 30, { R4L0, R4L0, R4L0, R4L0, R3L1, R3L1, R3L1, R3L1, R3L1, R3L1, R3L1, R2L2, R2L2, R2L2, R2L2, R2L2,
            R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2, R2L2 } },
};

static float scf_tab[64];   /* 2^(1 - i/3); index 63 is invalid and maps to 0 */

void mp3_l12_init(void) {
    static const double cr[3] = { 1.0, 0.79370052598409973738, 0.62996052494743658238 };  /* 2^(-k/3) */
    double p = 2.0;
    for (int i = 0; i < 63; i += 3) {
        for (int k = 0; k < 3 && i + k < 63; k++) scf_tab[i + k] = (float)(p * cr[k]);
        p *= 0.5;
    }
    scf_tab[63] = 0;
}

/* Bounded bit read: past the end of the frame it returns 0 and marks the reader as overrun. */
static inline uint32_t rd(struct mp3_bits *b, int n) {
    if (b->pos + n > b->end) { b->pos = b->end + 1; return 0; }
    return mp3_getbits(b, n);
}

static inline int bits_for(int levels) {   /* bits of an ungrouped sample */
    int b = 0;
    while ((1 << b) < levels + 1) b++;
    return b;
}

static inline float deq(int s, int levels) { return (float)(2 * s - (levels - 1)) / (float)levels; }

static int layer1(mp3dec *d, const struct mp3_hdr *h, struct mp3_bits *b) {
    int nch = h->channels;
    int bound = h->mode == 1 ? (h->mode_ext + 1) * 4 : 32;
    if (nch == 1) bound = 32;
    uint8_t alloc[2][32], scf[2][32];
    for (int sb = 0; sb < 32; sb++)
        for (int ch = 0; ch < nch; ch++) {
            if (sb >= bound && ch == 1) { alloc[1][sb] = alloc[0][sb]; continue; }
            int a = (int)rd(b, 4);
            if (a == 15) return -1;
            alloc[ch][sb] = (uint8_t)a;
        }
    for (int sb = 0; sb < 32; sb++)
        for (int ch = 0; ch < nch; ch++)
            scf[ch][sb] = alloc[ch][sb] ? (uint8_t)rd(b, 6) : 0;
    for (int s = 0; s < 12; s++)
        for (int sb = 0; sb < 32; sb++)
            for (int ch = 0; ch < nch; ch++) {
                int a = alloc[ch][sb];
                if (!a) { d->l12.sb[ch][s][sb] = 0; continue; }
                int nb = a + 1, levels = (1 << nb) - 1;
                if (sb >= bound && ch == 1) continue;   /* shared sample, written with channel 0 */
                float v = deq((int)rd(b, nb), levels);
                d->l12.sb[ch][s][sb] = v * scf_tab[scf[ch][sb]];
                if (sb >= bound && nch == 2) d->l12.sb[1][s][sb] = v * scf_tab[scf[1][sb]];
            }
    return 12;
}

static int layer2(mp3dec *d, const struct mp3_hdr *h, struct mp3_bits *b) {
    int nch = h->channels, t;
    if (h->lsf) t = 4;
    else {
        int chbr = h->bitrate ? h->bitrate / nch : (h->size * h->sample_rate / 144000) / nch;
        if ((h->sample_rate == 48000 && chbr >= 56) || (chbr >= 56 && chbr <= 80)) t = 0;
        else if (h->sample_rate != 48000 && chbr >= 96) t = 1;
        else if (h->sample_rate != 32000 && chbr <= 48) t = 2;
        else t = 3;
    }
    const struct l2tab *tab = &l2tabs[t];
    int sbl = tab->sblimit;
    int bound = h->mode == 1 ? (h->mode_ext + 1) * 4 : sbl;
    if (bound > sbl || nch == 1) bound = sbl;
    uint8_t alloc[2][32], scfsi[2][32], scf[2][32][3];
    for (int sb = 0; sb < sbl; sb++) {
        int nbal = tab->sb[sb].nbal;
        if (sb < bound) {
            for (int ch = 0; ch < nch; ch++) alloc[ch][sb] = (uint8_t)rd(b, nbal);
        } else {
            alloc[0][sb] = alloc[1][sb] = (uint8_t)rd(b, nbal);
        }
    }
    for (int sb = 0; sb < sbl; sb++)
        for (int ch = 0; ch < nch; ch++)
            scfsi[ch][sb] = alloc[ch][sb] ? (uint8_t)rd(b, 2) : 0;
    for (int sb = 0; sb < sbl; sb++)
        for (int ch = 0; ch < nch; ch++) {
            uint8_t *s = scf[ch][sb];
            if (!alloc[ch][sb]) { s[0] = s[1] = s[2] = 63; continue; }
            switch (scfsi[ch][sb]) {
            case 0: s[0] = (uint8_t)rd(b, 6); s[1] = (uint8_t)rd(b, 6); s[2] = (uint8_t)rd(b, 6); break;
            case 1: s[0] = s[1] = (uint8_t)rd(b, 6); s[2] = (uint8_t)rd(b, 6); break;
            case 2: s[0] = s[1] = s[2] = (uint8_t)rd(b, 6); break;
            default: s[0] = (uint8_t)rd(b, 6); s[1] = s[2] = (uint8_t)rd(b, 6); break;
            }
        }
    for (int gr = 0; gr < 12; gr++) {
        int part = gr >> 2;
        for (int sb = 0; sb < 32; sb++) {
            for (int ch = 0; ch < nch; ch++) {
                float *o0 = &d->l12.sb[ch][gr * 3][sb];
                if (sb >= sbl || !alloc[ch][sb]) { o0[0] = o0[32] = o0[64] = 0; continue; }
                if (sb >= bound && ch == 1) continue;   /* filled together with channel 0 below */
                int levels = tab->sb[sb].q[alloc[ch][sb] - 1];
                int v[3];
                if (levels == 3 || levels == 5 || levels == 9) {
                    int nb = levels == 3 ? 5 : levels == 5 ? 7 : 10;
                    int c = (int)rd(b, nb);
                    for (int k = 0; k < 3; k++) { v[k] = c % levels; c /= levels; }
                } else {
                    int nb = bits_for(levels);
                    for (int k = 0; k < 3; k++) v[k] = (int)rd(b, nb);
                }
                int nchs = (sb >= bound) ? nch : 1;
                for (int c2 = 0; c2 < nchs; c2++) {
                    int cc = nchs == 1 ? ch : c2;
                    float f = scf_tab[scf[cc][sb][part]];
                    float *o = &d->l12.sb[cc][gr * 3][sb];
                    for (int k = 0; k < 3; k++) o[32 * k] = deq(v[k], levels) * f;
                }
            }
        }
    }
    return 36;
}

int mp3_l12_frame(mp3dec *d, const struct mp3_hdr *h, const uint8_t *p, int16_t *pcm, int out_ch) {
    /* copy the frame so that bit reads past its end stay inside a padded buffer */
    uint8_t buf[2048 + 16];
    int n = h->size < 2048 ? h->size : 2048;
    memcpy(buf, p, (size_t)n);
    memset(buf + n, 0, 16);
    struct mp3_bits b = { buf, (4 + (h->crc ? 2 : 0)) * 8, (long)n * 8 };
    int slots = h->layer == 1 ? layer1(d, h, &b) : layer2(d, h, &b);
    if (slots < 0 || b.pos > b.end) {   /* damaged: output silence of the right length */
        slots = h->layer == 1 ? 12 : 36;
        memset(d->l12.sb, 0, sizeof d->l12.sb);
    }
    for (int ch = 0; ch < h->channels; ch++)
        for (int s = 0; s < slots; s++)
            mp3_synth32(&d->synth, ch, d->l12.sb[ch][s], pcm + (long)s * 32 * out_ch + ch, out_ch);
    return slots * 32;
}
