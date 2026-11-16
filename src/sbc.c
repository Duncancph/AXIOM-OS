/* SBC encoder (the audio format every Bluetooth speaker and headphone accepts, A2DP's mandatory codec), in
 * whole-number arithmetic for the kernel: 8 subbands, 16 blocks, stereo (two independent channels), loudness bit
 * allocation, at 44.1 or 48 kHz. Checked against ffmpeg's SBC decoder: 60-72 dB signal-to-noise at every test
 * frequency (tests/test_sbc.c). */
#include "sbc.h"
#include "sbc_tables.h"

#define F 4                                     /* fractional bits kept in the subband samples */

void sbc_init(struct sbc_enc *e, int rate, int bitpool) {
    for (int c = 0; c < 2; c++) for (int i = 0; i < 80; i++) e->x[c][i] = 0;
    e->rate48 = rate == 48000;
    e->bitpool = bitpool < 2 ? 2 : bitpool > 53 ? 53 : bitpool;
}
int sbc_frame_len(const struct sbc_enc *e) { return 4 + 8 + (16 * e->bitpool + 7) / 8; }   /* header, scale factors, samples */

static void analyze(struct sbc_enc *e, int ch, const int16_t *pcm, int64_t *s) {   /* 8 new samples (stride 2) -> 8 subbands */
    int32_t *x = e->x[ch];
    for (int i = 79; i >= 8; i--) x[i] = x[i - 8];
    for (int i = 7; i >= 0; i--) x[i] = pcm[(7 - i) * 2];
    int64_t y[16];
    for (int i = 0; i < 16; i++) {
        int64_t a = 0;
        for (int j = 0; j < 5; j++) a += (int64_t)x[i + 16 * j] * sbc_win8[i + 16 * j];
        y[i] = a >> 10;                         /* Q20 */
    }
    for (int k = 0; k < 8; k++) {
        int64_t a = 0;
        for (int i = 0; i < 16; i++) a += (int64_t)sbc_cos8[k][i] * y[i];
        s[k] = a >> (20 + 14 - F);              /* the subband sample, with F fractional bits */
    }
}

static const int8_t offset8[2][8] = { { -4, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 } };   /* 44.1, 48 kHz */

struct bw { uint8_t *p; int bit; };
static void put(struct bw *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (!w->bit) *w->p = 0;
        if ((v >> i) & 1) *w->p |= 0x80 >> w->bit;
        if (++w->bit == 8) { w->bit = 0; w->p++; }
    }
}
static uint8_t crc_bits(uint8_t crc, uint32_t v, int n) {        /* CRC-8, x^8 + x^4 + x^3 + x^2 + 1, MSB first */
    for (int i = n - 1; i >= 0; i--) {
        int top = (crc >> 7) & 1; crc = (uint8_t)(crc << 1);
        if (top ^ ((v >> i) & 1)) crc ^= 0x1D;
    }
    return crc;
}

/* 128 stereo samples (interleaved L R) -> one frame; returns its length */
int sbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out) {
    static int64_t sb[2][16][8];
    for (int b = 0; b < 16; b++)
        for (int c = 0; c < 2; c++) analyze(e, c, pcm + b * 16 + c, sb[c][b]);
    int sf[2][8], need[2][8], bits[2][8];
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 8; k++) {
            int64_t mx = 0;
            for (int b = 0; b < 16; b++) { int64_t v = sb[c][b][k] < 0 ? -sb[c][b][k] : sb[c][b][k]; if (v > mx) mx = v; }
            int f = 0;
            while (f < 15 && mx > ((int64_t)2 << f) << F) f++;
            sf[c][k] = f;
            if (!f) need[c][k] = -5;
            else { int l = f - offset8[e->rate48][k]; need[c][k] = l > 0 ? l / 2 : l; }
        }
    /* bit allocation over both channels (stereo), as the decoder repeats it */
    int maxneed = -100;
    for (int c = 0; c < 2; c++) for (int k = 0; k < 8; k++) if (need[c][k] > maxneed) maxneed = need[c][k];
    int bitcount = 0, slicecount = 0, bitslice = maxneed + 1;
    do {
        bitslice--; bitcount += slicecount; slicecount = 0;
        for (int c = 0; c < 2; c++) for (int k = 0; k < 8; k++) {
            if (need[c][k] > bitslice + 1 && need[c][k] < bitslice + 16) slicecount++;
            else if (need[c][k] == bitslice + 1) slicecount += 2;
        }
    } while (bitcount + slicecount < e->bitpool);
    if (bitcount + slicecount == e->bitpool) { bitcount += slicecount; bitslice--; }
    for (int c = 0; c < 2; c++) for (int k = 0; k < 8; k++) {
        int v = need[c][k] < bitslice + 2 ? 0 : need[c][k] - bitslice;
        bits[c][k] = v > 16 ? 16 : v;
    }
    int c = 0, k = 0;
    while (bitcount < e->bitpool && k < 8) {
        if (bits[c][k] >= 2 && bits[c][k] < 16) { bits[c][k]++; bitcount++; }
        else if (need[c][k] == bitslice + 1 && e->bitpool > bitcount + 1) { bits[c][k] = 2; bitcount += 2; }
        if (c == 1) { c = 0; k++; } else c = 1;
    }
    c = 0; k = 0;
    while (bitcount < e->bitpool && k < 8) {
        if (bits[c][k] < 16) { bits[c][k]++; bitcount++; }
        if (c == 1) { c = 0; k++; } else c = 1;
    }
    /* the frame */
    uint8_t h1 = (uint8_t)((e->rate48 ? 3 : 2) << 6 | 3 << 4 | 2 << 2 | 0 << 1 | 1);   /* rate, 16 blocks, stereo, loudness, 8 subbands */
    struct bw w = { out, 0 };
    put(&w, 0x9C, 8); put(&w, h1, 8); put(&w, (uint32_t)e->bitpool, 8); put(&w, 0, 8);
    uint8_t crc = 0x0F;
    crc = crc_bits(crc, h1, 8); crc = crc_bits(crc, (uint32_t)e->bitpool, 8);
    for (int ch = 0; ch < 2; ch++) for (int kk = 0; kk < 8; kk++) { put(&w, (uint32_t)sf[ch][kk], 4); crc = crc_bits(crc, (uint32_t)sf[ch][kk], 4); }
    out[3] = crc;
    for (int b = 0; b < 16; b++)
        for (int ch = 0; ch < 2; ch++)
            for (int kk = 0; kk < 8; kk++) {
                int n = bits[ch][kk];
                if (!n) continue;
                int64_t lv = ((int64_t)1 << n) - 1, scale = ((int64_t)2 << sf[ch][kk]) << F;
                int64_t q = ((sb[ch][b][kk] + scale) * lv) / (2 * scale);
                if (q < 0) q = 0;
                if (q > lv) q = lv;
                put(&w, (uint32_t)q, n);
            }
    return (int)(w.p - out) + (w.bit ? 1 : 0);
}
