/* AXIOM MP3 decoder: stream handling (frame sync, tags, Xing/Info/VBRI, gapless, seeking) and the public API. */
#include "mp3_int.h"

static const uint16_t br_tab[2][3][15] = {
    { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
      { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
      { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
    { { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
      { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
      { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } },
};
static const int sr_base[3] = { 44100, 48000, 32000 };

#define MAX_FREE_SIZE 2900   /* free format: up to 640 kbps at 32 kHz (Layer III) */

int mp3_parse_header(const uint8_t *p, struct mp3_hdr *h, int free_size) {
    uint32_t r = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    if ((r & 0xffe00000u) != 0xffe00000u) return 0;
    int ver = (int)(r >> 19) & 3, lay = (int)(r >> 17) & 3;
    int bri = (int)(r >> 12) & 15, sri = (int)(r >> 10) & 3;
    if (ver == 1 || lay == 0 || bri == 15 || sri == 3) return 0;
    h->raw = r;
    h->lsf = ver != 3;
    h->mpeg25 = ver == 0;
    h->layer = 4 - lay;
    h->crc = !((r >> 16) & 1);
    h->br_index = bri;
    h->sr_index = sri;
    h->sr_table = (ver == 3 ? 0 : ver == 2 ? 3 : 6) + sri;
    h->padding = (int)(r >> 9) & 1;
    h->mode = (int)(r >> 6) & 3;
    h->mode_ext = (int)(r >> 4) & 3;
    h->channels = h->mode == 3 ? 1 : 2;
    h->sample_rate = sr_base[sri] >> (h->lsf + h->mpeg25);
    h->bitrate = br_tab[h->lsf][h->layer - 1][bri];
    h->spf = h->layer == 1 ? 384 : (h->layer == 3 && h->lsf) ? 576 : 1152;
    if (h->bitrate) {
        long br = h->bitrate * 1000L;
        if (h->layer == 1) h->size = (int)((12 * br / h->sample_rate + h->padding) * 4);
        else if (h->layer == 3 && h->lsf) h->size = (int)(72 * br / h->sample_rate + h->padding);
        else h->size = (int)(144 * br / h->sample_rate + h->padding);
    } else {
        h->size = free_size ? free_size + h->padding * (h->layer == 1 ? 4 : 1) : 0;
    }
    return 1;
}

/* ------------------------------------------------------------ frame location */
static int hdr_at(const mp3dec *d, long pos, struct mp3_hdr *h) {
    if (pos < 0 || pos + 4 > d->end) return 0;
    if (!mp3_parse_header(d->data + pos, h, d->free_size)) return 0;
    if ((h->raw ^ d->match_val) & d->match_mask) return 0;
    if (h->channels != d->info.channels) return 0;
    if ((h->bitrate == 0) != (d->free_size != 0)) return 0;
    return h->size >= 16;
}

/* Next frame at or after pos that fits in the data and is followed by another matching header (or the end).
   Returns its offset or -1. */
static long find_frame(const mp3dec *d, long pos, struct mp3_hdr *h) {
    for (long o = pos < 0 ? 0 : pos; o + 4 <= d->end; o++) {
        if (d->data[o] != 0xff) continue;
        if (!hdr_at(d, o, h) || o + h->size > d->end) continue;
        struct mp3_hdr n;
        long nx = o + h->size;
        if (nx + 4 > d->end || hdr_at(d, nx, &n)) return o;
    }
    return -1;
}

/* Next frame for sequential reading: the frame right at pos if it is valid, else resync. */
static long next_frame(const mp3dec *d, long pos, int insync, struct mp3_hdr *h) {
    if (insync && hdr_at(d, pos, h) && pos + h->size <= d->end) return pos;
    return find_frame(d, pos, h);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Free format: the frame size is the distance to the next header with identical fixed fields. */
static int detect_free_size(const uint8_t *data, long end, long o, const struct mp3_hdr *h) {
    for (long n = o + 16; n + 4 <= end && n - o <= MAX_FREE_SIZE + 4; n++) {
        if (data[n] != 0xff) continue;
        struct mp3_hdr x;
        if (!mp3_parse_header(data + n, &x, 0)) continue;
        if (((x.raw ^ h->raw) & 0xfffef000u) || x.channels != h->channels) continue;
        int s = (int)(n - o) - h->padding * (h->layer == 1 ? 4 : 1);
        if (s >= 16) return s;
    }
    return 0;
}

/* Looks for the first frame: a header followed by two more consistent frames (or the end of data). */
static long probe_first(mp3dec *d, long pos) {
    for (long o = pos; o + 4 <= d->end; o++) {
        struct mp3_hdr h;
        if (d->data[o] != 0xff || !mp3_parse_header(d->data + o, &h, 0)) continue;
        d->free_size = 0;
        if (!h.bitrate) {
            d->free_size = detect_free_size(d->data, d->end, o, &h);
            if (!d->free_size) continue;
        }
        d->match_mask = 0xfffe0c00u;
        d->match_val = h.raw;
        d->info.channels = h.channels;
        long p = o;
        int k, ok = 1;
        for (k = 0; k < 3; k++) {
            struct mp3_hdr x;
            if (p + 4 > d->end) break;
            if (!hdr_at(d, p, &x) || p + x.size > d->end) { ok = 0; break; }
            p += x.size;
        }
        if (ok && (k == 3 || p == d->end || p + 4 > d->end)) return o;
    }
    return -1;
}

static long syncsafe(const uint8_t *p) { return (long)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f); }

long mp3_skip_id3v2(const uint8_t *data, long len) {
    long pos = 0;
    while (pos + 10 <= len && !memcmp(data + pos, "ID3", 3) && data[pos + 3] < 0xff && data[pos + 4] < 0xff) {
        pos += 10 + syncsafe(data + pos + 6) + ((data[pos + 5] & 0x10) ? 10 : 0);
    }
    return pos > len ? len : pos;
}

static long strip_trailing_tags(const uint8_t *data, long end) {
    for (int guard = 0; guard < 4; guard++) {
        if (end >= 128 && !memcmp(data + end - 128, "TAG", 3)) {
            end -= 128;
            if (end >= 227 && !memcmp(data + end - 227, "TAG+", 4)) end -= 227;
            continue;
        }
        if (end >= 32 && !memcmp(data + end - 32, "APETAGEX", 8)) {
            const uint8_t *f = data + end - 32;
            long size = (long)f[12] | (long)f[13] << 8 | (long)f[14] << 16 | (long)f[15] << 24;
            long flags = (long)f[20] | (long)f[21] << 8 | (long)f[22] << 16 | (long)f[23] << 24;
            size += (flags & 0x80000000L) ? 32 : 0;
            if (size > end) size = end;
            end -= size;
            continue;
        }
        if (end >= 15 && !memcmp(data + end - 9, "LYRICS200", 9)) {
            long size = 0;
            for (int i = 0; i < 6; i++) size = size * 10 + (data[end - 15 + i] - '0');
            if (size + 15 > end || size < 0) break;
            end -= size + 15;
            continue;
        }
        break;
    }
    return end;
}

/* Reads an Xing/Info or VBRI header in the frame at o. Returns 1 if the frame is a tag frame (to be skipped). */
static int parse_vbr_tag(mp3dec *d, long o, const struct mp3_hdr *h) {
    const uint8_t *f = d->data + o;
    long fend = h->size;
    if (h->layer != 3) return 0;
    int side = h->lsf ? (h->channels == 1 ? 9 : 17) : (h->channels == 1 ? 17 : 32);
    for (int pass = 0; pass < 2; pass++) {
        long x = 4 + side + (pass ? 2 : 0);
        if (x + 8 > fend || (memcmp(f + x, "Xing", 4) && memcmp(f + x, "Info", 4))) continue;
        uint32_t flags = be32(f + x + 4);
        long p = x + 8, frames = 0, bytes = 0;
        if (flags & 1) { if (p + 4 > fend) return 0; frames = be32(f + p); p += 4; }
        if (flags & 2) { if (p + 4 > fend) return 0; bytes = be32(f + p); p += 4; }
        if (flags & 4) p += 100;
        if (flags & 8) p += 4;
        if (!frames && !bytes) return 0;
        d->xing_frames = frames;
        d->vbr = !memcmp(f + x, "Xing", 4);
        if (p + 24 <= fend && (!memcmp(f + p, "LAME", 4) || !memcmp(f + p, "Lavf", 4) || !memcmp(f + p, "Lavc", 4))) {
            d->enc_delay = f[p + 21] << 4 | f[p + 22] >> 4;
            d->enc_padding = (f[p + 22] & 15) << 8 | f[p + 23];
            d->gapless = 1;
        }
        return 1;
    }
    if (36 + 18 <= fend && !memcmp(f + 36, "VBRI", 4)) {
        d->xing_frames = be32(f + 36 + 14);
        d->vbr = 1;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ state */
static long start_skip(const mp3dec *d) { return d->gapless ? d->enc_delay + 529L : 0; }

static void reset_state(mp3dec *d) {
    mp3_synth_reset(&d->synth);
    mp3_l3_reset(d);
    d->insync = 1;
}

static int decode_at(mp3dec *d, long o, const struct mp3_hdr *h, int16_t *pcm, int decode) {
    if (h->layer == 3) return mp3_l3_frame(d, h, d->data + o, pcm, d->info.channels, decode);
    if (!decode) return h->spf;
    return mp3_l12_frame(d, h, d->data + o, pcm, d->info.channels);
}

mp3dec *mp3_open(const uint8_t *data, long len, struct mp3_info *info) {
    if (!data || len < 4) return 0;
    mp3_dsp_init();
    mp3dec *d = calloc(1, sizeof *d);
    if (!d) return 0;
    d->data = data;
    d->len = len;
    d->end = strip_trailing_tags(data, len);
    long pos = mp3_skip_id3v2(data, d->end);
    long o = probe_first(d, pos);
    if (o < 0) { free(d); return 0; }
    struct mp3_hdr h;
    hdr_at(d, o, &h);
    d->info.sample_rate = h.sample_rate;
    d->info.layer = h.layer;
    d->spf = h.spf;
    d->lsf = h.lsf;
    d->first = o;
    if (parse_vbr_tag(d, o, &h)) {
        d->first = o + h.size;
        struct mp3_hdr a;   /* the tag frame may use another bitrate than the audio (LAME: LSF CBR) */
        if (hdr_at(d, d->first, &a)) h = a;
    }

    /* count frames (cheap: header hops) unless the tag says */
    long frames = d->xing_frames;
    if (!frames) {
        long p = d->first;
        int sync = 1;
        struct mp3_hdr x;
        while ((p = next_frame(d, p, sync, &x)) >= 0) { frames++; p += x.size; }
    }
    d->info.frames_est = frames;
    long total = frames * d->spf;
    d->out_limit = -1;
    if (d->gapless) {
        if (d->xing_frames) {
            d->out_limit = total - d->enc_delay - d->enc_padding;
            if (d->out_limit < 0) d->out_limit = 0;
            total = d->out_limit;
        } else {
            total -= start_skip(d);
            if (total < 0) total = 0;
        }
    }
    d->info.duration_ms = (long)((int64_t)total * 1000 / h.sample_rate);
    long secs_den = frames * d->spf;
    if (h.bitrate && !d->vbr)
        d->info.bitrate_kbps = h.bitrate;
    else if (secs_den > 0)
        d->info.bitrate_kbps = (int)((int64_t)(d->end - d->first) * 8 * h.sample_rate / secs_den / 1000);
    reset_state(d);
    d->pos = d->first;
    d->skip = start_skip(d);
    if (info) *info = d->info;
    return d;
}

int mp3_decode(mp3dec *d, int16_t *pcm) {
    if (!d || !pcm) return -1;
    for (;;) {
        if (d->out_limit >= 0 && d->out_pos >= d->out_limit) return 0;
        struct mp3_hdr h;
        long o = next_frame(d, d->pos, d->insync, &h);
        if (o < 0) { d->pos = d->end; return 0; }
        if (o != d->pos) d->res_ok = 0;     /* resynced: the reservoir is not continuous */
        int n = decode_at(d, o, &h, pcm, 1);
        d->pos = o + h.size;
        d->insync = 1;
        d->frame_no++;
        if (n <= 0) continue;
        int off = 0;
        if (d->skip > 0) {
            off = d->skip < n ? (int)d->skip : n;
            d->skip -= off;
        }
        long cnt = n - off;
        if (d->out_limit >= 0 && d->out_pos + cnt > d->out_limit) cnt = d->out_limit - d->out_pos;
        if (cnt <= 0) continue;
        if (off) memmove(pcm, pcm + (long)off * d->info.channels, (size_t)cnt * d->info.channels * sizeof(int16_t));
        d->out_pos += cnt;
        return (int)cnt;
    }
}

#define SEEK_RING 128

long mp3_seek(mp3dec *d, long ms) {
    if (!d) return 0;
    if (ms < 0) ms = 0;
    int64_t target = (int64_t)ms * d->info.sample_rate / 1000;
    if (d->out_limit >= 0 && target > d->out_limit) target = d->out_limit;
    long k = (long)((target + start_skip(d)) / d->spf);   /* frame holding the target sample */

    /* walk frames from the start, remembering the most recent offsets */
    long offs[SEEK_RING];
    long p = d->first, i = 0;
    struct mp3_hdr h;
    int sync = 1;
    while (i <= k) {
        long o = next_frame(d, p, sync, &h);
        if (o < 0) break;
        offs[i % SEEK_RING] = o;
        p = o + h.size;
        i++;
    }
    if (i <= k) k = i;   /* past the end: park at the end */
    reset_state(d);
    /* pre-roll: decode the frames just before k (output discarded) so that the IMDCT overlap and the
       synthesis filter history (16 time slots) are right; one granule alone is not enough for that */
    int preroll = (d->info.layer == 1 || (d->info.layer == 3 && d->lsf)) ? 2 : 1;
    long first_dec = k - preroll >= 0 ? k - preroll : 0;
    long first_fed = first_dec;
    if (d->info.layer == 3) {
        /* before that, only feed the bit reservoir (main_data_begin reaches up to 511 bytes back) */
        long bytes = 0;
        while (first_fed > 0 && k - first_fed < SEEK_RING - 1 && bytes < 600) {
            first_fed--;
            struct mp3_hdr x;
            hdr_at(d, offs[first_fed % SEEK_RING], &x);
            bytes += x.size - 4 - (x.crc ? 2 : 0) - (x.lsf ? (x.channels == 1 ? 9 : 17) : (x.channels == 1 ? 17 : 32));
        }
    }
    int16_t *scratch = d->scratch;
    for (long f = first_fed; f < k; f++) {
        long o = offs[f % SEEK_RING];
        struct mp3_hdr x;
        hdr_at(d, o, &x);
        decode_at(d, o, &x, scratch, f >= first_dec);
    }
    d->insync = 1;
    if (k < i) d->pos = offs[k % SEEK_RING];
    else d->pos = d->end;
    d->frame_no = k;
    int64_t dec = (int64_t)k * d->spf - start_skip(d);
    d->skip = dec < 0 ? -dec : 0;
    d->out_pos = dec < 0 ? 0 : (long)dec;
    if (d->out_limit >= 0 && d->out_pos > d->out_limit) d->out_pos = d->out_limit;
    return mp3_position_ms(d);
}

long mp3_position_samples(mp3dec *d) { return d ? d->out_pos : 0; }

long mp3_position_ms(mp3dec *d) {
    if (!d || !d->info.sample_rate) return 0;
    return (long)((int64_t)d->out_pos * 1000 / d->info.sample_rate);
}

void mp3_gapless(mp3dec *d, int *enc_delay, int *enc_padding) {
    if (enc_delay) *enc_delay = d && d->gapless ? d->enc_delay : 0;
    if (enc_padding) *enc_padding = d && d->gapless ? d->enc_padding : 0;
}

void mp3_close(mp3dec *d) { free(d); }
