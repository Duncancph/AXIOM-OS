/* AXIOM H.264 decoder: public API, NAL unit handling (Annex B / avcC), picture buffers and output order. */
#include "h264_int.h"

void h264_set_error(h264dec *d, const char *msg) {
    int i = 0;
    for (; msg[i] && i < (int)sizeof(d->err) - 1; i++) d->err[i] = msg[i];
    d->err[i] = 0;
}

/* ---------------------------------------------------------------- picture buffers */
static int pic_layout(h264dec *d, struct h264_pic *p) {
    uint8_t *m = p->mem;
    int sy = d->stride[0], sc = d->stride[1];
    int hy = d->mb_h * 16 + 2 * H264_PAD, hc = d->mb_h * 8 + 2 * H264_PADC;
    p->plane[0] = m + (size_t)sy * H264_PAD + H264_PAD;
    m += (size_t)sy * hy;
    p->plane[1] = m + (size_t)sc * H264_PADC + H264_PADC;
    m += (size_t)sc * hc;
    p->plane[2] = m + (size_t)sc * H264_PADC + H264_PADC;
    m += (size_t)sc * hc;
    m += 16;
    size_t n4 = (size_t)d->mb_w * 4 * d->mb_h * 4, n8 = (size_t)d->mb_w * 2 * d->mb_h * 2;
    for (int li = 0; li < 2; li++) { p->mv[li] = (int16_t (*)[2])(void *)m; m += n4 * 4; }
    for (int li = 0; li < 2; li++) { p->refuid[li] = (int32_t *)(void *)m; m += n8 * 4; }
    for (int li = 0; li < 2; li++) { p->refidx[li] = (int8_t *)m; m += n8; }
    p->alloc_w = d->mb_w;
    p->alloc_h = d->mb_h;
    p->stride_y = sy;
    p->stride_c = sc;
    return 0;
}

static size_t pic_bytes(h264dec *d) {
    int sy = d->stride[0], sc = d->stride[1];
    size_t img = (size_t)sy * (d->mb_h * 16 + 2 * H264_PAD) + 2 * (size_t)sc * (d->mb_h * 8 + 2 * H264_PADC);
    size_t n4 = (size_t)d->mb_w * 4 * d->mb_h * 4, n8 = (size_t)d->mb_w * 2 * d->mb_h * 2;
    d->img_size = img;
    return img + 16 + 2 * n4 * 4 + 2 * n8 * 4 + 2 * n8 + 64;
}

static void fill_gray(h264dec *d, struct h264_pic *p) {
    memset(p->mem, 128, d->img_size);
    size_t n4 = (size_t)d->mb_w * 4 * d->mb_h * 4, n8 = (size_t)d->mb_w * 2 * d->mb_h * 2;
    for (int li = 0; li < 2; li++) {
        memset(p->mv[li], 0, n4 * 4);
        memset(p->refidx[li], -1, n8);
        for (size_t i = 0; i < n8; i++) p->refuid[li][i] = -1;
    }
}

/* called after a geometry change: prepares the grey "missing reference" picture */
int h264_alloc_frame_data(h264dec *d) {
    d->pic_size = pic_bytes(d);
    struct h264_pic *m = d->missing;
    m->mem = (uint8_t *)malloc(d->pic_size);
    if (!m->mem) return -1;
    pic_layout(d, m);
    fill_gray(d, m);
    m->uid = ++d->uid_counter;
    m->ref = 1;
    m->poc = 0;
    return 0;
}

static int pic_in_use(const struct h264_pic *p) { return p->ref || p->waiting || p->held || p->busy; }

struct h264_pic *h264_alloc_pic(h264dec *d) {
    struct h264_pic *p = 0;
    for (int i = 0; i < d->n_pics; i++) {
        struct h264_pic *q = &d->pics[i];
        if (pic_in_use(q)) continue;
        if (q->mem && (q->alloc_w != d->mb_w || q->alloc_h != d->mb_h)) { free(q->mem); q->mem = 0; }
        if (!p || (q->mem && !p->mem)) p = q;
    }
    if (!p && d->n_pics < H264_MAX_PICS) p = &d->pics[d->n_pics++];
    if (!p) {
        /* overflow (broken stream): drop the oldest picture waiting for output */
        for (int i = 0; i < d->n_pics; i++) {
            struct h264_pic *q = &d->pics[i];
            if (q->waiting && !q->ref && !q->held && !q->busy && (!p || q->epoch < p->epoch || (q->epoch == p->epoch && q->poc < p->poc))) p = q;
        }
        if (!p) return 0;
        p->waiting = 0;
        if (p->alloc_w != d->mb_w || p->alloc_h != d->mb_h) { free(p->mem); p->mem = 0; }
    }
    if (!p->mem) {
        p->mem = (uint8_t *)malloc(d->pic_size);
        if (!p->mem) return 0;
        pic_layout(d, p);
    }
    uint8_t *mem = p->mem;
    int aw = p->alloc_w, ah = p->alloc_h, ssy = p->stride_y, ssc = p->stride_c;
    uint8_t *pl[3] = { p->plane[0], p->plane[1], p->plane[2] };
    int16_t (*mv0)[2] = p->mv[0], (*mv1)[2] = p->mv[1];
    int8_t *r0 = p->refidx[0], *r1 = p->refidx[1];
    int32_t *u0 = p->refuid[0], *u1 = p->refuid[1];
    memset(p, 0, sizeof(*p));
    p->mem = mem; p->alloc_w = aw; p->alloc_h = ah; p->stride_y = ssy; p->stride_c = ssc;
    p->plane[0] = pl[0]; p->plane[1] = pl[1]; p->plane[2] = pl[2];
    p->mv[0] = mv0; p->mv[1] = mv1; p->refidx[0] = r0; p->refidx[1] = r1; p->refuid[0] = u0; p->refuid[1] = u1;
    p->uid = ++d->uid_counter;
    if (d->uid_counter > 0x3fffffff) d->uid_counter = 1;
    return p;
}

/* ---------------------------------------------------------------- NAL units */
static int unescape(h264dec *d, const uint8_t *p, int len, struct h264_br *b) {
    if ((size_t)len + 32 > d->rbsp_cap) {
        size_t nc = (size_t)len + 32 + ((size_t)len >> 1) + 1024;
        uint8_t *n = (uint8_t *)realloc(d->rbsp, nc);
        if (!n) return -1;
        d->rbsp = n;
        d->rbsp_cap = nc;
    }
    uint8_t *o = d->rbsp;
    int n = 0, zeros = 0;
    for (int i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (zeros >= 2 && c == 3) { zeros = 0; continue; }
        zeros = c == 0 ? zeros + 1 : 0;
        o[n++] = c;
    }
    while (n > 0 && o[n - 1] == 0) n--;
    memset(o + n, 0, 32);
    b->buf = o;
    b->pos = 0;
    b->lim = (uint32_t)n * 8;
    if (n == 0) { b->end = 0; return 0; }
    int last = o[n - 1], tz = __builtin_ctz((unsigned)last);
    b->end = (uint32_t)n * 8 - (uint32_t)tz - 1;   /* position of rbsp_stop_one_bit */
    return n;
}

/* SEI: only the recovery point (payload type 6) is used */
static void parse_sei(h264dec *d, struct h264_br *b) {
    while (b->pos + 16 <= b->end) {
        int type = 0, size = 0;
        uint32_t v;
        do { v = br_bits(b, 8); type += (int)v; } while (v == 255 && type < 100000);
        do { v = br_bits(b, 8); size += (int)v; } while (v == 255 && size < 1000000);
        uint32_t next = b->pos + (uint32_t)size * 8;
        if (type == 6) {
            uint32_t cnt = br_ue(b);
            if (cnt < 65536 && b->pos <= b->end) d->recovery_cnt = (int)cnt;
        }
        if (next > b->end || next < b->pos) break;
        b->pos = next;
    }
}

static int decode_nal(h264dec *d, const uint8_t *p, int len) {
    if (len < 1) return 0;
    int type = p[0] & 31, ref_idc = (p[0] >> 5) & 3;
    if (p[0] & 0x80) return 0;   /* forbidden_zero_bit */
    struct h264_br b;
    switch (type) {
    case 1: case 5: {
        if (unescape(d, p + 1, len - 1, &b) <= 0) return 0;
        int r = h264_slice_nal(d, &b, type, ref_idc);
        return r == -2 ? -1 : 0;
    }
    case 7:
        h264_finish_picture(d);
        if (unescape(d, p + 1, len - 1, &b) <= 0) return 0;
        if (h264_parse_sps(d, &b) < 0) h264_set_error(d, "bad SPS (ignored)");
        return 0;
    case 8:
        h264_finish_picture(d);
        if (unescape(d, p + 1, len - 1, &b) <= 0) return 0;
        if (h264_parse_pps(d, &b) < 0) h264_set_error(d, "bad PPS (ignored)");
        return 0;
    case 6:
        h264_finish_picture(d);
        if (unescape(d, p + 1, len - 1, &b) > 0) parse_sei(d, &b);
        return 0;
    case 9: case 10: case 11:
        h264_finish_picture(d);
        return 0;
    case 2: case 3: case 4:
        h264_set_error(d, "unsupported: data partitioning");
        return -1;
    default:
        return 0;
    }
}

static int parse_avcc(h264dec *d, const uint8_t *e, int len) {
    if (len < 7 || e[0] != 1) return -1;
    d->nal_len_size = (e[4] & 3) + 1;
    int pos = 6, n = e[5] & 31;
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < n; i++) {
            if (pos + 2 > len) return -1;
            int l = e[pos] << 8 | e[pos + 1];
            pos += 2;
            if (pos + l > len) return -1;
            decode_nal(d, e + pos, l);
            pos += l;
        }
        if (k == 0) {
            if (pos >= len) return -1;
            n = e[pos++];
        }
    }
    return 0;
}

h264dec *h264_open(const uint8_t *extradata, int len) {
    h264dec *d = (h264dec *)calloc(1, sizeof(h264dec));
    if (!d) return 0;
    h264_tables_init();
    d->need_key = 1;
    d->dq_pps = -1;
    d->last_out_epoch = -1;
    d->recovery_cnt = -1;
    d->recover_until = -1;
    d->max_lt_idx = -1;
    if (extradata && len > 0) {
        d->avcc = 1;
        d->nal_len_size = 4;
        if (parse_avcc(d, extradata, len) < 0) {
            d->sticky_error = 1;
            h264_set_error(d, "invalid avcC extradata");
        }
    }
    if (!d->sticky_error) d->err[0] = 0;
    return d;
}

static void release_held(h264dec *d) {
    for (int i = 0; i < d->n_pics; i++) d->pics[i].held = 0;
}

/* Annex B: processes every complete NAL unit in the input buffer (all of it when final) */
static int process_annexb(h264dec *d, int final) {
    uint8_t *buf = d->in;
    size_t n = d->in_len, i = 0, start = (size_t)-1;
    int ret = 0;
    size_t consumed = 0;
    while (i + 2 < n) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            if (start != (size_t)-1) {
                size_t end = i;
                while (end > start && buf[end - 1] == 0) end--;
                /* pts of the chunk the NAL started in */
                for (int k = 0; k < d->n_in_pts; k++) if (d->in_pts[k].off <= start) d->nal_pts = d->in_pts[k].pts;
                if (decode_nal(d, buf + start, (int)(end - start)) < 0) ret = -1;
            }
            start = i + 3;
            consumed = i;
            i += 3;
            continue;
        }
        i++;
    }
    if (final) {
        if (start != (size_t)-1 && start <= n) {
            size_t end = n;
            while (end > start && buf[end - 1] == 0) end--;
            for (int k = 0; k < d->n_in_pts; k++) if (d->in_pts[k].off <= start) d->nal_pts = d->in_pts[k].pts;
            if (decode_nal(d, buf + start, (int)(end - start)) < 0) ret = -1;
        }
        d->in_len = 0;
        d->n_in_pts = 0;
        return ret;
    }
    if (start == (size_t)-1) {
        /* no start code yet: keep the last two bytes (a start code may straddle chunks) */
        consumed = n > 2 ? n - 2 : 0;
    }
    if (consumed) {
        memmove(buf, buf + consumed, n - consumed);
        d->in_len = n - consumed;
        int k = 0, keep = -1;
        for (int j = 0; j < d->n_in_pts; j++) if (d->in_pts[j].off <= consumed) keep = j;
        for (int j = 0; j < d->n_in_pts; j++) {
            if (j < keep) continue;
            d->in_pts[k].pts = d->in_pts[j].pts;
            d->in_pts[k].off = d->in_pts[j].off > consumed ? d->in_pts[j].off - consumed : 0;
            k++;
        }
        d->n_in_pts = k;
    }
    return ret;
}

int h264_decode(h264dec *d, const uint8_t *data, int len, int64_t pts) {
    if (!d) return -1;
    release_held(d);
    d->flushing = 0;
    d->err[0] = 0;
    if (d->sticky_error) { h264_set_error(d, "invalid avcC extradata"); return -1; }
    if (!data || len <= 0) return 0;
    int ret = 0;
    if (d->avcc) {
        d->nal_pts = pts;
        int pos = 0, ls = d->nal_len_size;
        while (pos + ls <= len) {
            uint32_t l = 0;
            for (int i = 0; i < ls; i++) l = l << 8 | data[pos + i];
            pos += ls;
            if (l > (uint32_t)(len - pos)) l = (uint32_t)(len - pos);   /* truncated sample */
            if (decode_nal(d, data + pos, (int)l) < 0) ret = -1;
            pos += (int)l;
        }
        h264_finish_picture(d);
    } else {
        if (d->in_len + (size_t)len + 64 > d->in_cap) {
            size_t nc = (d->in_len + (size_t)len + 64) * 2;
            uint8_t *n = (uint8_t *)realloc(d->in, nc);
            if (!n) { h264_set_error(d, "out of memory"); return -1; }
            d->in = n;
            d->in_cap = nc;
        }
        if (d->n_in_pts == 64) {
            memmove(&d->in_pts[0], &d->in_pts[1], sizeof(d->in_pts[0]) * 63);
            d->n_in_pts--;
        }
        d->in_pts[d->n_in_pts].off = d->in_len;
        d->in_pts[d->n_in_pts].pts = pts;
        d->n_in_pts++;
        memcpy(d->in + d->in_len, data, (size_t)len);
        d->in_len += (size_t)len;
        ret = process_annexb(d, 0);
    }
    if (ret < 0 && !d->err[0]) h264_set_error(d, "unsupported stream");
    return ret;
}

int h264_get_frame(h264dec *d, struct h264_frame *f) {
    if (!d) return 0;
    struct h264_pic *best = 0;
    int n_cur = 0;
    for (int i = 0; i < d->n_pics; i++) {
        struct h264_pic *p = &d->pics[i];
        if (!p->waiting) continue;
        if (p->epoch == d->epoch) n_cur++;
        if (!best || p->epoch < best->epoch || (p->epoch == best->epoch && p->poc < best->poc)) best = p;
    }
    if (!best) return 0;
    if (!d->flushing && best->epoch == d->epoch && n_cur <= d->reorder) return 0;
    best->waiting = 0;
    best->held = 1;
    d->last_out_poc = best->poc;
    d->last_out_epoch = best->epoch;
    int sy = best->stride_y, sc = best->stride_c;
    f->stride_y = sy;
    f->stride_uv = sc;
    f->y = best->plane[0] + (size_t)best->crop_t * sy + best->crop_l;
    f->u = best->plane[1] + (size_t)(best->crop_t >> 1) * sc + (best->crop_l >> 1);
    f->v = best->plane[2] + (size_t)(best->crop_t >> 1) * sc + (best->crop_l >> 1);
    f->width = best->disp_w;
    f->height = best->disp_h;
    f->pts = best->pts;
    f->keyframe = best->idr;
    f->full_range = best->full_range;
    f->matrix = best->matrix;
    return 1;
}

void h264_flush(h264dec *d) {
    if (!d) return;
    release_held(d);
    if (!d->avcc) process_annexb(d, 1);
    h264_finish_picture(d);
    d->flushing = 1;
}

void h264_reset(h264dec *d) {
    if (!d) return;
    if (d->cur) { d->cur->busy = 0; d->cur->waiting = 0; d->cur = 0; }
    for (int i = 0; i < d->n_pics; i++) { d->pics[i].ref = 0; d->pics[i].waiting = 0; }
    d->in_len = 0;
    d->n_in_pts = 0;
    d->need_key = 1;
    d->have_prev_ref = 0;
    d->drop_leading = 0;
    d->prev_poc_msb = d->prev_poc_lsb = d->prev_frame_num = d->prev_frame_num_offset = d->prev_ref_frame_num = 0;
    d->prev_has_mmco5 = 0;
    d->max_lt_idx = -1;
    d->flushing = 0;
    d->skip_pic = 0;
    d->recovery_cnt = -1;
    d->recover_until = -1;
    d->epoch++;
}

void h264_close(h264dec *d) {
    if (!d) return;
    for (int i = 0; i < d->n_pics; i++) free(d->pics[i].mem);
    if (d->missing) { free(d->missing->mem); free(d->missing); }
    for (int i = 0; i < 32; i++) free(d->sps[i]);
    for (int i = 0; i < 256; i++) free(d->pps[i]);
    free(d->mbi);
    free(d->slices);
    free(d->in);
    free(d->rbsp);
    free(d);
}

const char *h264_error(h264dec *d) { return d ? d->err : "no decoder"; }

int h264_info(h264dec *d, int *width, int *height, int *reorder) {
    if (!d || !d->have_sps) return 0;
    const struct h264_sps *s = &d->cur_sps;
    if (width) *width = s->mb_w * 16 - s->crop_l - s->crop_r;
    if (height) *height = s->mb_h * 16 - s->crop_t - s->crop_b;
    if (reorder) *reorder = d->reorder;
    return 1;
}

long h264_concealed_mbs(h264dec *d) { return d ? d->concealed : 0; }
