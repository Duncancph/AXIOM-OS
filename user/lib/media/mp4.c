/* AXIOM MP4 / ISO BMFF (and QuickTime MOV) demuxer.
 * Freestanding C11: no libc. Works on the whole file in memory; builds one flat sample index per track
 * (regular sample tables and movie fragments). Every read is bounds-checked; damaged files give
 * fewer samples, never out-of-bounds reads. */
#include "media_port.h"
#include "mp4.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define MAX_TRACKS 64
#define MAX_SAMPLES (1L << 25)

struct smp {
    int64_t off;
    int64_t dts;
    uint32_t size;
    int32_t cts;     /* composition offset */
    uint32_t dur;
    uint32_t key;
};

struct trk {
    struct mp4_track pub;
    uint32_t id;
    uint32_t handler;
    int64_t shift;      /* added to dts and pts of samples from the moov sample tables (edit list) */
    int64_t shift_frag; /* same for samples from movie fragments */
    int64_t dts_shift;  /* subtracted from dts when ctts has negative offsets (so that dts <= pts) */
    long n_moov;        /* samples [0, n_moov) come from the moov sample tables */
    int64_t priming;
    int64_t mdhd_dur;
    struct smp *s;
    long n, cap;
    /* fragment state */
    int has_trex;
    uint32_t trex_dur, trex_size, trex_flags;
    int64_t frag_dts;
    /* edit list (raw) */
    int64_t elst_empty;   /* movie timescale */
    int64_t elst_mtime;   /* media timescale, -1: none */
    int64_t elst_dur;     /* movie timescale, duration of that edit (0: to the end) */
};

struct mp4 {
    const uint8_t *d;
    int64_t len;
    struct trk *t;
    int nt;
    uint32_t mvhd_ts;
    int64_t mvhd_dur;
};

/* ---------------------------------------------------------------- bounded reader */
struct rd { const uint8_t *d; int64_t pos, end; int err; };

static void rd_init(struct rd *r, const uint8_t *d, int64_t start, int64_t end) {
    r->d = d; r->pos = start; r->end = end; r->err = 0;
}
static int rd_has(struct rd *r, int64_t n) {
    if (n < 0 || r->pos > r->end || r->end - r->pos < n) { r->err = 1; r->pos = r->end; return 0; }
    return 1;
}
static uint32_t rd8(struct rd *r) { if (!rd_has(r, 1)) return 0; return r->d[r->pos++]; }
static uint32_t rd16(struct rd *r) {
    if (!rd_has(r, 2)) return 0;
    const uint8_t *p = r->d + r->pos; r->pos += 2;
    return (uint32_t)p[0] << 8 | p[1];
}
static uint32_t rd24(struct rd *r) {
    if (!rd_has(r, 3)) return 0;
    const uint8_t *p = r->d + r->pos; r->pos += 3;
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static uint32_t rd32(struct rd *r) {
    if (!rd_has(r, 4)) return 0;
    const uint8_t *p = r->d + r->pos; r->pos += 4;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t rd64(struct rd *r) {
    uint64_t h = rd32(r);
    return h << 32 | rd32(r);
}
static void rd_skip(struct rd *r, int64_t n) { if (rd_has(r, n)) r->pos += n; }

/* ---------------------------------------------------------------- boxes */
struct box { uint32_t type; int64_t start, body, end; };

/* reads the box header at pos (must lie in [pos, end)); returns 0 if there is no valid box */
static int box_at(const uint8_t *d, int64_t pos, int64_t end, struct box *b) {
    struct rd r;
    rd_init(&r, d, pos, end);
    uint64_t size = rd32(&r);
    uint32_t type = rd32(&r);
    if (r.err) return 0;
    if (size == 1) {
        size = rd64(&r);
        if (r.err) return 0;
    } else if (size == 0) {
        size = (uint64_t)(end - pos);
    }
    if (type == FOURCC('u', 'u', 'i', 'd')) rd_skip(&r, 16);
    if (r.err) return 0;
    int64_t hdr = r.pos - pos;
    if (size < (uint64_t)hdr) return 0;
    b->type = type;
    b->start = pos;
    b->body = r.pos;
    b->end = size > (uint64_t)(end - pos) ? end : pos + (int64_t)size;   /* truncated: clamp to parent */
    return 1;
}

/* finds the first child of [start, end) with that type */
static int find_box(const uint8_t *d, int64_t start, int64_t end, uint32_t type, struct box *out) {
    struct box b;
    for (int64_t p = start; p < end && box_at(d, p, end, &b); p = b.end) {
        if (b.type == type) { *out = b; return 1; }
        if (b.end <= p) break;
    }
    return 0;
}

/* ---------------------------------------------------------------- sample storage */
static int add_samples(struct trk *t, long n) {
    if (n <= 0) return 1;
    if (t->n + n > MAX_SAMPLES) return 0;
    if (t->n + n > t->cap) {
        long nc = t->cap ? t->cap : 256;
        while (nc < t->n + n) nc *= 2;
        struct smp *ns = realloc(t->s, (size_t)nc * sizeof *ns);
        if (!ns) return 0;
        t->s = ns;
        t->cap = nc;
    }
    memset(t->s + t->n, 0, (size_t)n * sizeof *t->s);
    return 1;
}

/* ---------------------------------------------------------------- AudioSpecificConfig (just the header) */
struct bits { const uint8_t *p; int len, pos; };
static uint32_t gb(struct bits *b, int n) {
    uint32_t v = 0;
    while (n-- > 0) {
        int bit = 0;
        if (b->pos < b->len * 8) bit = b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1;
        b->pos++;
        v = v << 1 | (uint32_t)bit;
    }
    return v;
}
static const int asc_rates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };

static void parse_asc(struct mp4_track *tr) {
    if (!tr->config || tr->config_len < 2) return;
    struct bits b = { tr->config, tr->config_len, 0 };
    int aot = (int)gb(&b, 5);
    if (aot == 31) aot = 32 + (int)gb(&b, 6);
    int sfi = (int)gb(&b, 4);
    int rate = sfi == 15 ? (int)gb(&b, 24) : asc_rates[sfi];
    int cc = (int)gb(&b, 4);
    int ps = 0;
    if (aot == 5 || aot == 29) {
        ps = aot == 29;
        int esfi = (int)gb(&b, 4);
        int erate = esfi == 15 ? (int)gb(&b, 24) : asc_rates[esfi];
        if (erate) rate = erate;
    }
    static const int cc_ch[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 24, 8, 0 };
    if (rate > 0) tr->sample_rate = rate;
    if (cc_ch[cc]) tr->channels = cc_ch[cc];
    if (ps && tr->channels == 1) tr->channels = 2;
}

/* ---------------------------------------------------------------- esds */
static int desc_len(struct rd *r) {
    int64_t len = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t c = rd8(r);
        len = len << 7 | (c & 0x7f);
        if (!(c & 0x80)) break;
    }
    return r->err ? -1 : (int)len;
}

static void parse_descs(const uint8_t *d, int64_t start, int64_t end, struct mp4_track *tr, int depth) {
    struct rd r;
    rd_init(&r, d, start, end);
    while (!r.err && r.pos + 2 <= r.end && depth < 8) {
        uint32_t tag = rd8(&r);
        int len = desc_len(&r);
        if (len < 0) return;
        int64_t ds = r.pos, de = r.pos + len;
        if (de > r.end) de = r.end;
        struct rd s;
        rd_init(&s, d, ds, de);
        if (tag == 3) {          /* ES_Descriptor */
            rd16(&s);
            uint32_t fl = rd8(&s);
            if (fl & 0x80) rd16(&s);
            if (fl & 0x40) rd_skip(&s, rd8(&s));
            if (fl & 0x20) rd16(&s);
            if (!s.err) parse_descs(d, s.pos, de, tr, depth + 1);
        } else if (tag == 4) {   /* DecoderConfigDescriptor */
            tr->object_type = (int)rd8(&s);
            rd_skip(&s, 12);
            if (!s.err) parse_descs(d, s.pos, de, tr, depth + 1);
        } else if (tag == 5) {   /* DecoderSpecificInfo */
            if (!tr->config) {
                tr->config = d + ds;
                tr->config_len = (int)(de - ds);
            }
        }
        r.pos = de;
    }
}

static void parse_esds(const uint8_t *d, struct box *b, struct mp4_track *tr) {
    if (b->end - b->body < 4) return;
    parse_descs(d, b->body + 4, b->end, tr, 0);
}

/* ---------------------------------------------------------------- stsd */
static void parse_entry_children(const uint8_t *d, int64_t start, int64_t end, struct mp4_track *tr, int depth) {
    struct box b;
    if (depth > 3) return;
    for (int64_t p = start; p < end && box_at(d, p, end, &b); p = b.end) {
        switch (b.type) {
        case FOURCC('e', 's', 'd', 's'):
            if (!tr->config) parse_esds(d, &b, tr);
            break;
        case FOURCC('w', 'a', 'v', 'e'):   /* QuickTime: wave { frma, mp4a, esds } */
            parse_entry_children(d, b.body, b.end, tr, depth + 1);
            break;
        case FOURCC('a', 'v', 'c', 'C'): case FOURCC('h', 'v', 'c', 'C'): case FOURCC('v', 'p', 'c', 'C'):
        case FOURCC('a', 'v', '1', 'C'): case FOURCC('d', 'O', 'p', 's'): case FOURCC('d', 'f', 'L', 'a'):
            if (!tr->config) {
                tr->config = d + b.body;
                tr->config_len = (int)(b.end - b.body);
            }
            break;
        }
        if (b.end <= p) break;
    }
}

static void parse_stsd(struct mp4 *m, struct trk *t, struct box *stsd) {
    const uint8_t *d = m->d;
    struct rd r;
    rd_init(&r, d, stsd->body, stsd->end);
    rd32(&r);
    uint32_t cnt = rd32(&r);
    if (r.err || cnt == 0) return;
    struct box e;
    if (!box_at(d, r.pos, stsd->end, &e)) return;
    struct mp4_track *tr = &t->pub;
    for (int i = 0; i < 4; i++) {
        char c = (char)(e.type >> (24 - 8 * i));
        tr->codec[i] = c ? c : ' ';
    }
    tr->codec[4] = 0;
    struct rd s;
    rd_init(&s, d, e.body, e.end);
    rd_skip(&s, 8);   /* reserved + data_reference_index */
    if (t->handler == FOURCC('v', 'i', 'd', 'e')) {
        rd_skip(&s, 16);
        tr->width = (int)rd16(&s);
        tr->height = (int)rd16(&s);
        rd_skip(&s, 50);
        if (!s.err) parse_entry_children(d, s.pos, e.end, tr, 0);
        tr->type = MP4_VIDEO;
    } else if (t->handler == FOURCC('s', 'o', 'u', 'n')) {
        uint32_t ver = rd16(&s);
        rd_skip(&s, 6);
        tr->channels = (int)rd16(&s);
        rd_skip(&s, 6);
        tr->sample_rate = (int)(rd32(&s) >> 16);
        if (ver == 1) rd_skip(&s, 16);
        else if (ver == 2) {
            rd_skip(&s, 4);
            union { uint64_t u; double f; } sr;
            sr.u = rd64(&s);
            uint32_t ch = rd32(&s);
            rd_skip(&s, 20);
            if (sr.f > 0 && sr.f < 1e7) tr->sample_rate = (int)sr.f;
            if (ch < 256) tr->channels = (int)ch;
        }
        if (!s.err) parse_entry_children(d, s.pos, e.end, tr, 0);
        tr->type = MP4_AUDIO;
        if (e.type == FOURCC('m', 'p', '4', 'a') && (tr->object_type == 0x40 || tr->object_type == 0x66 ||
                                                     tr->object_type == 0x67 || tr->object_type == 0x68))
            parse_asc(tr);
    }
}

/* ---------------------------------------------------------------- sample tables */
struct stbl { struct box stsz, stz2, stco, co64, stsc, stts, ctts, stss; unsigned have; };
enum { H_STSZ = 1, H_STZ2 = 2, H_STCO = 4, H_CO64 = 8, H_STSC = 16, H_STTS = 32, H_CTTS = 64, H_STSS = 128 };

static void build_samples(struct mp4 *m, struct trk *t, struct stbl *st) {
    const uint8_t *d = m->d;
    struct rd z;
    long n = 0;
    uint32_t const_size = 0, field = 32;
    if (st->have & H_STSZ) {
        rd_init(&z, d, st->stsz.body, st->stsz.end);
        rd32(&z);
        const_size = rd32(&z);
        uint32_t cnt = rd32(&z);
        if (z.err) return;
        if (const_size == 0) {
            int64_t maxn = (z.end - z.pos) / 4;
            n = cnt > maxn ? (long)maxn : (long)cnt;
        } else {
            int64_t maxn = m->len / const_size + 1;
            n = cnt > maxn ? (long)maxn : (long)cnt;
        }
    } else if (st->have & H_STZ2) {
        rd_init(&z, d, st->stz2.body, st->stz2.end);
        rd32(&z);
        rd24(&z);
        field = rd8(&z);
        uint32_t cnt = rd32(&z);
        if (z.err || (field != 4 && field != 8 && field != 16)) return;
        int64_t maxn = (z.end - z.pos) * 8 / field;
        n = cnt > maxn ? (long)maxn : (long)cnt;
    } else return;
    if (n > MAX_SAMPLES) n = MAX_SAMPLES;
    if (n <= 0 || !add_samples(t, n)) return;
    struct smp *s = t->s;
    /* sizes */
    for (long i = 0; i < n; i++) {
        if (st->have & H_STSZ) s[i].size = const_size ? const_size : rd32(&z);
        else if (field == 16) s[i].size = rd16(&z);
        else if (field == 8) s[i].size = rd8(&z);
        else {
            uint32_t v = i & 1 ? (uint32_t)d[z.pos++] & 15 : (uint32_t)d[z.pos] >> 4;
            s[i].size = v;
        }
    }
    /* chunk offsets + stsc */
    struct rd co, sc;
    int big = 0;
    if (st->have & H_STCO) rd_init(&co, d, st->stco.body, st->stco.end);
    else if (st->have & H_CO64) { rd_init(&co, d, st->co64.body, st->co64.end); big = 1; }
    else { return; }
    rd32(&co);
    uint32_t nchunks = rd32(&co);
    int64_t maxc = (co.end - co.pos) / (big ? 8 : 4);
    if (co.err) return;
    if (nchunks > maxc) nchunks = (uint32_t)maxc;
    long si = 0;
    if ((st->have & H_STSC) && nchunks) {
        rd_init(&sc, d, st->stsc.body, st->stsc.end);
        rd32(&sc);
        uint32_t ne = rd32(&sc);
        int64_t maxe = (sc.end - sc.pos) / 12;
        if (ne > maxe) ne = (uint32_t)maxe;
        uint32_t chunk = 1;   /* next chunk to fill (1-based) */
        for (uint32_t e = 0; e < ne && si < n && !sc.err; e++) {
            uint32_t first = rd32(&sc), spc = rd32(&sc);
            rd32(&sc);
            uint32_t next = nchunks + 1;
            if (e + 1 < ne) {
                struct rd pk = sc;
                next = rd32(&pk);
                if (next > nchunks + 1) next = nchunks + 1;
            }
            if (first < chunk) first = chunk;
            for (uint32_t c = first; c < next && si < n; c++) {
                struct rd cr = co;
                cr.pos = co.pos + (int64_t)(c - 1) * (big ? 8 : 4);
                int64_t off = big ? (int64_t)rd64(&cr) : (int64_t)rd32(&cr);
                if (cr.err) break;
                for (uint32_t j = 0; j < spc && si < n; j++) {
                    s[si].off = off;
                    off += s[si].size;
                    si++;
                }
                chunk = c + 1;
            }
        }
    }
    n = si;
    /* stts */
    if (st->have & H_STTS) {
        struct rd r;
        rd_init(&r, d, st->stts.body, st->stts.end);
        rd32(&r);
        uint32_t ne = rd32(&r);
        int64_t dts = 0;
        long i = 0;
        uint32_t last = 0;
        for (uint32_t e = 0; e < ne && i < n && !r.err; e++) {
            uint32_t cnt = rd32(&r), delta = rd32(&r);
            if (r.err) break;
            last = delta;
            for (uint32_t j = 0; j < cnt && i < n; j++, i++) {
                s[i].dts = dts;
                s[i].dur = delta;
                dts += delta;
            }
        }
        for (; i < n; i++) { s[i].dts = dts; s[i].dur = last; dts += last; }
    }
    /* ctts */
    if (st->have & H_CTTS) {
        struct rd r;
        rd_init(&r, d, st->ctts.body, st->ctts.end);
        rd32(&r);
        uint32_t ne = rd32(&r);
        long i = 0;
        for (uint32_t e = 0; e < ne && i < n && !r.err; e++) {
            uint32_t cnt = rd32(&r);
            int32_t off = (int32_t)rd32(&r);
            if (r.err) break;
            for (uint32_t j = 0; j < cnt && i < n; j++, i++) s[i].cts = off;
        }
    }
    /* stss */
    int all_key = 1;
    if (st->have & H_STSS) {
        struct rd r;
        rd_init(&r, d, st->stss.body, st->stss.end);
        rd32(&r);
        uint32_t ne = rd32(&r);
        if (!r.err && ne) {
            all_key = 0;
            for (uint32_t e = 0; e < ne && !r.err; e++) {
                uint32_t k = rd32(&r);
                if (!r.err && k >= 1 && k <= (uint32_t)n) s[k - 1].key = 1;
            }
        }
    }
    if (all_key)
        for (long i = 0; i < n; i++) s[i].key = 1;
    t->n = n;
    t->n_moov = n;
}

/* ---------------------------------------------------------------- trak */
static void parse_elst(struct mp4 *m, struct trk *t, struct box *b) {
    struct rd r;
    rd_init(&r, m->d, b->body, b->end);
    uint32_t vf = rd32(&r);
    uint32_t ne = rd32(&r);
    int v1 = (vf >> 24) == 1;
    for (uint32_t e = 0; e < ne && !r.err; e++) {
        int64_t dur, mt;
        if (v1) { dur = (int64_t)rd64(&r); mt = (int64_t)rd64(&r); }
        else { dur = rd32(&r); mt = (int32_t)rd32(&r); }
        rd32(&r);
        if (r.err) break;
        if (mt == -1) { t->elst_empty += dur; continue; }
        t->elst_mtime = mt < 0 ? 0 : mt;
        t->elst_dur = dur;
        break;
    }
}

static void parse_trak(struct mp4 *m, struct box *trak) {
    if (m->nt >= MAX_TRACKS) return;
    const uint8_t *d = m->d;
    struct trk *t = &m->t[m->nt];
    memset(t, 0, sizeof *t);
    t->elst_mtime = -1;
    struct box b, mdia, minf, stbl, x;
    if (find_box(d, trak->body, trak->end, FOURCC('t', 'k', 'h', 'd'), &b)) {
        struct rd r;
        rd_init(&r, d, b.body, b.end);
        uint32_t ver = rd32(&r) >> 24;
        rd_skip(&r, ver == 1 ? 16 : 8);
        t->id = rd32(&r);
        rd_skip(&r, 4 + (ver == 1 ? 8 : 4) + 8 + 4 + 4 + 36);
        uint32_t w = rd32(&r), h = rd32(&r);
        if (!r.err) { t->pub.disp_w = (int)(w >> 16); t->pub.disp_h = (int)(h >> 16); }
    }
    if (find_box(d, trak->body, trak->end, FOURCC('e', 'd', 't', 's'), &b) &&
        find_box(d, b.body, b.end, FOURCC('e', 'l', 's', 't'), &x))
        parse_elst(m, t, &x);
    if (!find_box(d, trak->body, trak->end, FOURCC('m', 'd', 'i', 'a'), &mdia)) return;
    if (find_box(d, mdia.body, mdia.end, FOURCC('m', 'd', 'h', 'd'), &b)) {
        struct rd r;
        rd_init(&r, d, b.body, b.end);
        uint32_t ver = rd32(&r) >> 24;
        if (ver == 1) {
            rd_skip(&r, 16);
            t->pub.timescale = rd32(&r);
            t->mdhd_dur = (int64_t)rd64(&r);
        } else {
            rd_skip(&r, 8);
            t->pub.timescale = rd32(&r);
            uint32_t du = rd32(&r);
            t->mdhd_dur = du == 0xffffffffu ? 0 : du;
        }
        if (r.err || t->mdhd_dur < 0) t->mdhd_dur = 0;
    }
    if (!t->pub.timescale) t->pub.timescale = 1000;
    if (find_box(d, mdia.body, mdia.end, FOURCC('h', 'd', 'l', 'r'), &b)) {
        struct rd r;
        rd_init(&r, d, b.body, b.end);
        rd32(&r);
        rd32(&r);
        t->handler = rd32(&r);
    }
    m->nt++;
    if (!find_box(d, mdia.body, mdia.end, FOURCC('m', 'i', 'n', 'f'), &minf)) return;
    if (!find_box(d, minf.body, minf.end, FOURCC('s', 't', 'b', 'l'), &stbl)) return;
    struct stbl st;
    memset(&st, 0, sizeof st);
    for (int64_t p = stbl.body; p < stbl.end && box_at(d, p, stbl.end, &b); p = b.end) {
        switch (b.type) {
        case FOURCC('s', 't', 's', 'd'): parse_stsd(m, t, &b); break;
        case FOURCC('s', 't', 's', 'z'): if (!(st.have & H_STSZ)) { st.stsz = b; st.have |= H_STSZ; } break;
        case FOURCC('s', 't', 'z', '2'): if (!(st.have & H_STZ2)) { st.stz2 = b; st.have |= H_STZ2; } break;
        case FOURCC('s', 't', 'c', 'o'): if (!(st.have & H_STCO)) { st.stco = b; st.have |= H_STCO; } break;
        case FOURCC('c', 'o', '6', '4'): if (!(st.have & H_CO64)) { st.co64 = b; st.have |= H_CO64; } break;
        case FOURCC('s', 't', 's', 'c'): if (!(st.have & H_STSC)) { st.stsc = b; st.have |= H_STSC; } break;
        case FOURCC('s', 't', 't', 's'): if (!(st.have & H_STTS)) { st.stts = b; st.have |= H_STTS; } break;
        case FOURCC('c', 't', 't', 's'): if (!(st.have & H_CTTS)) { st.ctts = b; st.have |= H_CTTS; } break;
        case FOURCC('s', 't', 's', 's'): if (!(st.have & H_STSS)) { st.stss = b; st.have |= H_STSS; } break;
        }
        if (b.end <= p) break;
    }
    build_samples(m, t, &st);
}

static struct trk *track_by_id(struct mp4 *m, uint32_t id) {
    for (int i = 0; i < m->nt; i++)
        if (m->t[i].id == id) return &m->t[i];
    return 0;
}

static void parse_mvex(struct mp4 *m, struct box *mvex) {
    struct box b;
    for (int64_t p = mvex->body; p < mvex->end && box_at(m->d, p, mvex->end, &b); p = b.end) {
        if (b.type == FOURCC('t', 'r', 'e', 'x')) {
            struct rd r;
            rd_init(&r, m->d, b.body, b.end);
            rd32(&r);
            uint32_t id = rd32(&r);
            rd32(&r);
            uint32_t dur = rd32(&r), size = rd32(&r), fl = rd32(&r);
            struct trk *t = track_by_id(m, id);
            if (t && !r.err) {
                t->has_trex = 1;
                t->trex_dur = dur;
                t->trex_size = size;
                t->trex_flags = fl;
            }
        }
        if (b.end <= p) break;
    }
}

static int parse_moov(struct mp4 *m, struct box *moov) {
    struct box b;
    for (int64_t p = moov->body; p < moov->end && box_at(m->d, p, moov->end, &b); p = b.end) {
        if (b.type == FOURCC('m', 'v', 'h', 'd')) {
            struct rd r;
            rd_init(&r, m->d, b.body, b.end);
            uint32_t ver = rd32(&r) >> 24;
            rd_skip(&r, ver == 1 ? 16 : 8);
            m->mvhd_ts = rd32(&r);
            m->mvhd_dur = ver == 1 ? (int64_t)rd64(&r) : (int64_t)rd32(&r);
            if (r.err) m->mvhd_ts = 0;
        } else if (b.type == FOURCC('t', 'r', 'a', 'k')) {
            parse_trak(m, &b);
        }
        if (b.end <= p) break;
    }
    if (find_box(m->d, moov->body, moov->end, FOURCC('m', 'v', 'e', 'x'), &b)) parse_mvex(m, &b);
    return m->nt;
}

/* ---------------------------------------------------------------- fragments */
static void parse_traf(struct mp4 *m, struct box *traf, int64_t moof_start, int64_t *implicit) {
    const uint8_t *d = m->d;
    struct box b;
    if (!find_box(d, traf->body, traf->end, FOURCC('t', 'f', 'h', 'd'), &b)) return;
    struct rd r;
    rd_init(&r, d, b.body, b.end);
    uint32_t fl = rd32(&r) & 0xffffff;
    struct trk *t = track_by_id(m, rd32(&r));
    if (!t || r.err) return;
    int64_t base;
    uint32_t ddur = t->trex_dur, dsize = t->trex_size, dflags = t->trex_flags;
    if (fl & 1) base = (int64_t)rd64(&r);
    else if (fl & 0x20000) base = moof_start;
    else base = *implicit;
    if (fl & 2) rd32(&r);
    if (fl & 8) ddur = rd32(&r);
    if (fl & 0x10) dsize = rd32(&r);
    if (fl & 0x20) dflags = rd32(&r);
    if (r.err) return;
    if (find_box(d, traf->body, traf->end, FOURCC('t', 'f', 'd', 't'), &b)) {
        struct rd q;
        rd_init(&q, d, b.body, b.end);
        uint32_t ver = rd32(&q) >> 24;
        int64_t v = ver == 1 ? (int64_t)rd64(&q) : (int64_t)rd32(&q);
        if (!q.err) t->frag_dts = v;
    }
    int64_t pos = base;
    for (int64_t p = traf->body; p < traf->end && box_at(d, p, traf->end, &b); p = b.end) {
        if (b.end <= p) break;
        if (b.type != FOURCC('t', 'r', 'u', 'n')) continue;
        struct rd q;
        rd_init(&q, d, b.body, b.end);
        uint32_t vf = rd32(&q);
        uint32_t tf = vf & 0xffffff;
        uint32_t cnt = rd32(&q);
        if (tf & 1) pos = base + (int32_t)rd32(&q);
        uint32_t first_flags = (tf & 4) ? rd32(&q) : 0;
        if (q.err) continue;
        int per = ((tf & 0x100) != 0) + ((tf & 0x200) != 0) + ((tf & 0x400) != 0) + ((tf & 0x800) != 0);
        int64_t maxn;
        if (per) maxn = (q.end - q.pos) / (4 * per);
        else maxn = dsize ? (m->len / dsize + 1) : (1 << 20);
        if (cnt > maxn) cnt = (uint32_t)maxn;
        if (!add_samples(t, (long)cnt)) return;
        struct smp *s = t->s + t->n;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t du = (tf & 0x100) ? rd32(&q) : ddur;
            uint32_t sz = (tf & 0x200) ? rd32(&q) : dsize;
            uint32_t sf = (tf & 0x400) ? rd32(&q) : (i == 0 && (tf & 4)) ? first_flags : dflags;
            int32_t ct = (tf & 0x800) ? (int32_t)rd32(&q) : 0;
            if (i == 0 && (tf & 4)) sf = first_flags;
            s[i].off = pos;
            s[i].size = sz;
            s[i].dts = t->frag_dts;
            s[i].dur = du;
            s[i].cts = ct;
            s[i].key = !(sf & 0x01010000u);
            pos += sz;
            t->frag_dts += du;
        }
        t->n += cnt;
    }
    *implicit = pos;
}

static void parse_moof(struct mp4 *m, struct box *moof) {
    struct box b;
    int64_t implicit = moof->start;
    for (int64_t p = moof->body; p < moof->end && box_at(m->d, p, moof->end, &b); p = b.end) {
        if (b.type == FOURCC('t', 'r', 'a', 'f')) parse_traf(m, &b, moof->start, &implicit);
        if (b.end <= p) break;
    }
}

/* ---------------------------------------------------------------- open */
static int64_t rescale(int64_t v, int64_t from, int64_t to) {   /* v * to / from, floor */
    if (from <= 0) return 0;
    if (v >= 0 && v < ((int64_t)1 << 40) && to < ((int64_t)1 << 22)) return v * to / from;
    int neg = v < 0;
    uint64_t a = neg ? (uint64_t)-v : (uint64_t)v;
    uint64_t q = a / (uint64_t)from, rmd = a % (uint64_t)from;
    uint64_t res = q * (uint64_t)to + rmd * (uint64_t)to / (uint64_t)from;
    if (neg) {
        if ((rmd * (uint64_t)to) % (uint64_t)from) res++;
        return -(int64_t)res;
    }
    return (int64_t)res;
}

mp4 *mp4_open(const uint8_t *data, int64_t len) {
    if (!data || len < 16) return 0;
    struct mp4 *m = calloc(1, sizeof *m);
    if (!m) return 0;
    m->d = data;
    m->len = len;
    m->t = calloc(MAX_TRACKS, sizeof *m->t);
    if (!m->t) { free(m); return 0; }
    struct box b;
    int have_moov = 0;
    for (int64_t p = 0; p < len && box_at(data, p, len, &b); p = b.end) {
        if (b.type == FOURCC('m', 'o', 'o', 'v') && !have_moov) {
            have_moov = 1;
            parse_moov(m, &b);
        } else if (b.type == FOURCC('m', 'o', 'o', 'f') && have_moov) {
            parse_moof(m, &b);
        }
        if (b.end <= p) break;
    }
    if (!have_moov || m->nt == 0) { mp4_close(m); return 0; }
    for (int i = 0; i < m->nt; i++) {
        struct trk *t = &m->t[i];
        t->pub.nsamples = t->n;
        int64_t dur = t->mdhd_dur;
        if (t->n) {
            int64_t sd = t->s[t->n - 1].dts + t->s[t->n - 1].dur - t->s[0].dts;
            if (dur <= 0) dur = sd;
        }
        t->pub.duration = dur > 0 ? dur : 0;
        int32_t mincts = 0;
        for (long k = 0; k < t->n_moov; k++)
            if (t->s[k].cts < mincts) mincts = t->s[k].cts;
        t->dts_shift = -(int64_t)mincts;
        if (t->elst_mtime >= 0 || t->elst_empty) {
            /* Edit list: an initial empty edit delays the track, the first real edit says which media time is
               shown first. Like ffmpeg (and players), video starts at the earliest frame that is inside the edit. */
            int64_t mts = m->mvhd_ts ? m->mvhd_ts : 1000;
            int64_t empty = rescale(t->elst_empty, mts, t->pub.timescale);
            int64_t mt = t->elst_mtime > 0 ? t->elst_mtime : 0;
            int64_t ed = t->elst_dur > 0 ? rescale(t->elst_dur, mts, t->pub.timescale) : 0;
            t->shift = t->shift_frag = empty - mt;
            if (t->pub.type != MP4_AUDIO) {
                int64_t first_dts = 0, minpts = 0;
                int found = 0;
                for (long k = 0; k < t->n_moov; k++) {
                    int64_t pts = t->s[k].dts + t->s[k].cts;
                    if (pts < mt || (ed > 0 && pts >= mt + ed)) continue;
                    if (!found) { first_dts = t->s[k].dts; minpts = pts; found = 1; }
                    else if (pts < minpts) minpts = pts;
                }
                if (found) t->shift = empty - (minpts > first_dts ? minpts : first_dts);
            }
            if (t->pub.type == MP4_AUDIO && mt > 0) t->priming = mt;
        }
        if (!t->pub.type && t->n == 0) t->pub.codec[0] = 0;
    }
    return m;
}

int mp4_ntracks(mp4 *m) { return m ? m->nt : 0; }

const struct mp4_track *mp4_track(mp4 *m, int i) {
    if (!m || i < 0 || i >= m->nt) return 0;
    return &m->t[i].pub;
}

static int decodable(const struct mp4_track *t) {
    if (t->type == MP4_VIDEO) return !memcmp(t->codec, "avc1", 4) || !memcmp(t->codec, "avc3", 4);
    if (t->type == MP4_AUDIO) return !memcmp(t->codec, "mp4a", 4) && (t->object_type == 0x40 || t->object_type == 0x67 || t->object_type == 0x66 || t->object_type == 0x68);
    return 0;
}

int mp4_find(mp4 *m, int type) {
    if (!m) return -1;
    int any = -1;
    for (int i = 0; i < m->nt; i++) {
        const struct mp4_track *t = &m->t[i].pub;
        if (t->type != type || m->t[i].n == 0) continue;
        if (decodable(t)) return i;
        if (any < 0) any = i;
    }
    if (any < 0)
        for (int i = 0; i < m->nt; i++)
            if (m->t[i].pub.type == type) return i;
    return any;
}

static void sample_times(const struct trk *t, long i, int64_t *dts, int64_t *pts) {
    const struct smp *p = &t->s[i];
    if (i < t->n_moov) {
        *pts = p->dts + p->cts + t->shift;
        *dts = p->dts + t->shift - t->dts_shift;
    } else {
        *dts = p->dts + t->shift_frag;
        *pts = *dts + p->cts;
    }
}

int mp4_sample(mp4 *m, int track, long i, struct mp4_sample *s) {
    if (!m || track < 0 || track >= m->nt) return 0;
    struct trk *t = &m->t[track];
    if (i < 0 || i >= t->n) return 0;
    const struct smp *p = &t->s[i];
    sample_times(t, i, &s->dts, &s->pts);
    s->offset = p->off;
    s->size = p->size;
    if (p->off < 0 || p->off > m->len) { s->offset = 0; s->size = 0; }   /* damaged: never point outside the file */
    else if ((int64_t)p->size > m->len - p->off) s->size = (uint32_t)(m->len - p->off);
    s->keyframe = (int)p->key;
    return 1;
}

int64_t mp4_ms(mp4 *m, int track, int64_t t) {
    if (!m || track < 0 || track >= m->nt) return 0;
    return rescale(t, m->t[track].pub.timescale, 1000);
}

long mp4_sample_at(mp4 *m, int track, int64_t pts_ms, int keyframe_only) {
    if (!m || track < 0 || track >= m->nt) return -1;
    struct trk *t = &m->t[track];
    if (t->n == 0) return -1;
    int64_t target = rescale(pts_ms, 1000, t->pub.timescale);
    long best = -1, first = -1;
    int64_t best_pts = 0;
    for (long i = 0; i < t->n; i++) {
        const struct smp *p = &t->s[i];
        if (keyframe_only && !p->key) continue;
        if (first < 0) first = i;
        int64_t pts, dts;
        sample_times(t, i, &dts, &pts);
        if (pts <= target && (best < 0 || pts > best_pts)) { best = i; best_pts = pts; }
    }
    if (best < 0) best = first >= 0 ? first : 0;
    return best;
}

long mp4_duration_ms(mp4 *m) {
    if (!m) return 0;
    int64_t best = 0;
    for (int i = 0; i < m->nt; i++) {
        struct trk *t = &m->t[i];
        if (!t->pub.type) continue;
        int64_t ms = rescale(t->pub.duration, t->pub.timescale, 1000);
        if (ms > best) best = ms;
    }
    if (best == 0 && m->mvhd_ts) best = rescale(m->mvhd_dur, m->mvhd_ts, 1000);
    return (long)best;
}

void mp4_close(mp4 *m) {
    if (!m) return;
    if (m->t)
        for (int i = 0; i < m->nt; i++) free(m->t[i].s);
    free(m->t);
    free(m);
}

uint32_t mp4_track_id(mp4 *m, int track) {
    return m && track >= 0 && track < m->nt ? m->t[track].id : 0;
}

int64_t mp4_track_shift(mp4 *m, int track) {
    return m && track >= 0 && track < m->nt ? m->t[track].shift : 0;
}

int64_t mp4_sample_duration(mp4 *m, int track, long i) {
    if (!m || track < 0 || track >= m->nt || i < 0 || i >= m->t[track].n) return 0;
    return m->t[track].s[i].dur;
}

int64_t mp4_priming(mp4 *m, int track) {
    return m && track >= 0 && track < m->nt ? m->t[track].priming : 0;
}
