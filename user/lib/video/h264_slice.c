/* AXIOM H.264 decoder: slice header (7.3.3), picture start/finish, POC (8.2.1), reference lists (8.2.4),
   reference marking (8.2.5) and the slice data loop (7.3.4). */
#include "h264_int.h"

static int level_max_dpb_mbs(int level, int constraint) {
    switch (level) {
    case 9: return 396;
    case 10: return 396;
    case 11: return (constraint & 0x10) ? 396 : 900;
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51: case 52: return 184320;
    default: return 696320;
    }
}

/* (re)allocates the per-picture-size state; called when the active SPS changes geometry */
static int setup_geometry(h264dec *d, const struct h264_sps *s) {
    /* output everything that was waiting, drop references */
    d->epoch++;
    for (int i = 0; i < d->n_pics; i++) d->pics[i].ref = 0;
    d->mb_w = s->mb_w;
    d->mb_h = s->mb_h;
    d->mb_count = s->mb_w * s->mb_h;
    d->width = s->mb_w * 16;
    d->height = s->mb_h * 16;
    d->stride[0] = (d->width + 2 * H264_PAD + 63) & ~63;
    d->stride[1] = d->stride[2] = (d->width / 2 + 2 * H264_PADC + 63) & ~63;
    free(d->mbi);
    d->mbi = (struct h264_mbinfo *)calloc((size_t)d->mb_count, sizeof(struct h264_mbinfo));
    if (!d->mbi) return -1;
    if (d->missing) { free(d->missing->mem); free(d->missing); d->missing = 0; }
    d->missing = (struct h264_pic *)calloc(1, sizeof(struct h264_pic));
    if (!d->missing) return -1;
    if (h264_alloc_frame_data(d) < 0) return -1;
    return 0;
}

static int same_geometry(const struct h264_sps *a, const struct h264_sps *b) {
    return a->mb_w == b->mb_w && a->mb_h == b->mb_h;
}

/* ---------------------------------------------------------------- slice header */
static int parse_header(h264dec *d, struct h264_br *b, struct h264_slice *s, int nal_type, int nal_ref_idc) {
    s->nal_type = nal_type;
    s->nal_ref_idc = nal_ref_idc;
    s->idr = nal_type == 5;
    uint32_t first = br_ue(b), st = br_ue(b), ppsid = br_ue(b);
    if (st > 9 || ppsid > 255) { h264_set_error(d, "bad slice header"); return -1; }
    const struct h264_pps *p = d->pps[ppsid];
    if (!p || !p->valid) { h264_set_error(d, "slice refers to a missing PPS"); return -1; }
    const struct h264_sps *sps = d->sps[p->sps_id];
    if (!sps || !sps->valid) { h264_set_error(d, "slice refers to a missing SPS"); return -1; }
    if (sps->chroma_format_idc != 1) { h264_set_error(d, "unsupported: only 4:2:0 chroma"); return -2; }
    if (sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8) { h264_set_error(d, "unsupported: bit depth > 8"); return -2; }
    if (sps->mbaff) { h264_set_error(d, "unsupported: interlaced (MBAFF)"); return -2; }
    if (p->num_slice_groups > 1) { h264_set_error(d, "unsupported: slice groups (FMO)"); return -2; }
    int type = (int)st % 5;
    if (type > 2) { h264_set_error(d, "unsupported: SP/SI slices"); return -2; }
    s->type = type == 0 ? 0 : type == 1 ? 1 : 2;
    s->first_mb = (int)first;
    if (first >= (uint32_t)(sps->mb_w * sps->mb_h)) { h264_set_error(d, "bad first_mb_in_slice"); return -1; }
    s->pps_id = (int)ppsid;
    s->frame_num = (int)br_bits(b, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only) {
        if (br_bit(b)) { h264_set_error(d, "unsupported: interlaced (field pictures)"); return -2; }
    }
    s->idr_pic_id = s->idr ? (int)br_ue(b) : 0;
    s->poc_lsb = 0; s->delta_poc_bottom = 0; s->delta_poc[0] = s->delta_poc[1] = 0;
    if (sps->poc_type == 0) {
        s->poc_lsb = (int)br_bits(b, sps->log2_max_poc_lsb);
        if (p->bottom_field_pic_order) s->delta_poc_bottom = br_se(b);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        s->delta_poc[0] = br_se(b);
        if (p->bottom_field_pic_order) s->delta_poc[1] = br_se(b);
    }
    s->redundant_cnt = p->redundant_pic_cnt_present ? (int)br_ue(b) : 0;
    s->direct_spatial = s->type == 1 ? (int)br_bit(b) : 0;
    s->num_ref[0] = s->num_ref[1] = 0;
    if (s->type != 2) {
        s->num_ref[0] = p->num_ref_idx[0];
        s->num_ref[1] = s->type == 1 ? p->num_ref_idx[1] : 0;
        if (br_bit(b)) {
            uint32_t a = br_ue(b), c = s->type == 1 ? br_ue(b) : 0;
            if (a > 31 || c > 31) { h264_set_error(d, "bad num_ref_idx"); return -1; }
            s->num_ref[0] = (int)a + 1;
            if (s->type == 1) s->num_ref[1] = (int)c + 1;
        }
    }
    s->n_mod[0] = s->n_mod[1] = 0;
    for (int li = 0; li < (s->type == 2 ? 0 : s->type == 1 ? 2 : 1); li++) {
        if (br_bit(b)) {
            for (;;) {
                uint32_t idc = br_ue(b);
                if (idc == 3) break;
                if (idc > 2 || s->n_mod[li] >= 33) { h264_set_error(d, "bad ref_pic_list_modification"); return -1; }
                s->mod_idc[li][s->n_mod[li]] = (int)idc;
                uint32_t v = br_ue(b);
                if (v > 0x7fffffff) return -1;
                s->mod_val[li][s->n_mod[li]++] = (int)v;
            }
        }
    }
    s->wp_mode = 0;
    if ((p->weighted_pred && s->type == 0) || (p->weighted_bipred == 1 && s->type == 1)) {
        s->wp_mode = 1;
        uint32_t lw = br_ue(b), cw = br_ue(b);
        if (lw > 7 || cw > 7) { h264_set_error(d, "bad pred_weight_table"); return -1; }
        s->luma_log2_wd = (int)lw;
        s->chroma_log2_wd = (int)cw;
        for (int li = 0; li < (s->type == 1 ? 2 : 1); li++)
            for (int i = 0; i < s->num_ref[li]; i++) {
                int f = 0;
                s->lw[li][i] = (int16_t)(1 << lw); s->lo[li][i] = 0;
                if (br_bit(b)) {
                    int w = br_se(b), o = br_se(b);
                    if (w < -128 || w > 127 || o < -128 || o > 127) { h264_set_error(d, "bad weight"); return -1; }
                    s->lw[li][i] = (int16_t)w; s->lo[li][i] = (int16_t)o;
                    f = 1;
                }
                for (int j = 0; j < 2; j++) { s->cw[li][i][j] = (int16_t)(1 << cw); s->co[li][i][j] = 0; }
                if (br_bit(b)) {
                    for (int j = 0; j < 2; j++) {
                        int w = br_se(b), o = br_se(b);
                        if (w < -128 || w > 127 || o < -128 || o > 127) { h264_set_error(d, "bad weight"); return -1; }
                        s->cw[li][i][j] = (int16_t)w; s->co[li][i][j] = (int16_t)o;
                    }
                    f = 1;
                }
                s->wflag[li][i] = (uint8_t)f;
            }
    } else if (p->weighted_bipred == 2 && s->type == 1) s->wp_mode = 2;
    s->no_output_prior = s->long_term_ref = s->adaptive_marking = s->n_mmco = 0;
    if (nal_ref_idc) {
        if (s->idr) {
            s->no_output_prior = (int)br_bit(b);
            s->long_term_ref = (int)br_bit(b);
        } else if ((s->adaptive_marking = (int)br_bit(b))) {
            for (;;) {
                uint32_t op = br_ue(b);
                if (op == 0) break;
                if (op > 6 || s->n_mmco >= 66) { h264_set_error(d, "bad dec_ref_pic_marking"); return -1; }
                struct h264_mmco *m = &s->mmco[s->n_mmco++];
                m->op = (int)op; m->a = m->b = 0;
                if (op == 1 || op == 3) m->a = (int)br_ue(b);
                if (op == 2) m->a = (int)br_ue(b);
                if (op == 3 || op == 6) m->b = (int)br_ue(b);
                if (op == 4) m->a = (int)br_ue(b);
                if (br_overrun(b)) return -1;
            }
        }
    }
    s->cabac_init_idc = 0;
    if (p->cabac && s->type != 2) {
        uint32_t v = br_ue(b);
        if (v > 2) { h264_set_error(d, "bad cabac_init_idc"); return -1; }
        s->cabac_init_idc = (int)v;
    }
    int qd = br_se(b);
    s->qp = p->init_qp + qd;
    if (s->qp < 0 || s->qp > 51) { h264_set_error(d, "bad slice_qp_delta"); return -1; }
    s->deblock_idc = 0; s->alpha_off = s->beta_off = 0;
    if (p->deblock_ctrl) {
        uint32_t v = br_ue(b);
        if (v > 2) { h264_set_error(d, "bad disable_deblocking_filter_idc"); return -1; }
        s->deblock_idc = (int)v;
        if (v != 1) {
            int a = br_se(b), c = br_se(b);
            if (a < -6 || a > 6 || c < -6 || c > 6) { h264_set_error(d, "bad deblocking offsets"); return -1; }
            s->alpha_off = a * 2; s->beta_off = c * 2;
        }
    }
    if (br_overrun(b)) { h264_set_error(d, "truncated slice header"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------- reference marking */
static int frame_num_wrap(const h264dec *d, const struct h264_pic *p) {
    int max = 1 << d->act_sps->log2_max_frame_num;
    return p->frame_num > d->cur_frame_num ? p->frame_num - max : p->frame_num;
}

static void sliding_window(h264dec *d, struct h264_pic *cur) {
    int nshort = 0, nlong = 0;
    struct h264_pic *oldest = 0;
    for (int i = 0; i < d->n_pics; i++) {
        struct h264_pic *p = &d->pics[i];
        if (p == cur || !p->ref) continue;
        if (p->ref == 2) nlong++;
        else {
            nshort++;
            if (!oldest || p->frame_num_wrap < oldest->frame_num_wrap) oldest = p;
        }
    }
    int maxr = h264_max(d->act_sps->max_num_ref_frames, 1);
    if (nshort + nlong >= maxr && oldest) oldest->ref = 0;
}

static void mark_picture(h264dec *d, struct h264_pic *cur, const struct h264_slice *s) {
    int max = 1 << d->act_sps->log2_max_frame_num;
    for (int i = 0; i < d->n_pics; i++)
        if (d->pics[i].ref == 1) d->pics[i].frame_num_wrap = d->pics[i].frame_num > cur->frame_num ? d->pics[i].frame_num - max : d->pics[i].frame_num;
    if (s->idr) {
        for (int i = 0; i < d->n_pics; i++) if (&d->pics[i] != cur) d->pics[i].ref = 0;
        if (s->long_term_ref) { cur->ref = 2; cur->lt_idx = 0; d->max_lt_idx = 0; }
        else { cur->ref = 1; d->max_lt_idx = -1; }
        return;
    }
    int cur_long = 0;
    if (s->adaptive_marking) {
        for (int k = 0; k < s->n_mmco; k++) {
            const struct h264_mmco *m = &s->mmco[k];
            switch (m->op) {
            case 1: case 3: {
                int pn = cur->frame_num - (m->a + 1);
                for (int i = 0; i < d->n_pics; i++) {
                    struct h264_pic *p = &d->pics[i];
                    if (p == cur || p->ref != 1 || p->frame_num_wrap != pn) continue;
                    if (m->op == 1) p->ref = 0;
                    else {
                        for (int j = 0; j < d->n_pics; j++)
                            if (d->pics[j].ref == 2 && d->pics[j].lt_idx == m->b && &d->pics[j] != p) d->pics[j].ref = 0;
                        p->ref = 2; p->lt_idx = m->b;
                    }
                    break;
                }
                break;
            }
            case 2:
                for (int i = 0; i < d->n_pics; i++)
                    if (&d->pics[i] != cur && d->pics[i].ref == 2 && d->pics[i].lt_idx == m->a) d->pics[i].ref = 0;
                break;
            case 4:
                d->max_lt_idx = m->a - 1;
                for (int i = 0; i < d->n_pics; i++)
                    if (d->pics[i].ref == 2 && d->pics[i].lt_idx > d->max_lt_idx) d->pics[i].ref = 0;
                break;
            case 5:
                for (int i = 0; i < d->n_pics; i++) if (&d->pics[i] != cur) d->pics[i].ref = 0;
                d->max_lt_idx = -1;
                cur->mmco5 = 1;
                break;
            case 6:
                for (int i = 0; i < d->n_pics; i++)
                    if (&d->pics[i] != cur && d->pics[i].ref == 2 && d->pics[i].lt_idx == m->b) d->pics[i].ref = 0;
                cur->ref = 2; cur->lt_idx = m->b;
                cur_long = 1;
                break;
            }
        }
    } else sliding_window(d, cur);
    if (!cur_long) { cur->ref = 1; cur->frame_num_wrap = cur->mmco5 ? 0 : cur->frame_num; }
    /* robustness: never keep more references than allowed */
    int maxr = h264_max(d->act_sps->max_num_ref_frames, 1);
    for (;;) {
        int n = 0;
        struct h264_pic *oldest = 0;
        for (int i = 0; i < d->n_pics; i++) {
            struct h264_pic *p = &d->pics[i];
            if (!p->ref) continue;
            n++;
            if (p != cur && p->ref == 1 && (!oldest || p->frame_num_wrap < oldest->frame_num_wrap)) oldest = p;
        }
        if (n <= maxr || !oldest) break;
        oldest->ref = 0;
    }
}

/* ---------------------------------------------------------------- picture order count */
static void compute_poc(h264dec *d, const struct h264_slice *s, int *top, int *bottom, int *msb_out) {
    const struct h264_sps *sps = d->act_sps;
    int maxfn = 1 << sps->log2_max_frame_num;
    if (sps->poc_type == 0) {
        int prev_msb, prev_lsb;
        if (s->idr) { prev_msb = 0; prev_lsb = 0; }
        else { prev_msb = d->prev_poc_msb; prev_lsb = d->prev_poc_lsb; }
        int maxlsb = 1 << sps->log2_max_poc_lsb, lsb = s->poc_lsb, msb;
        if (lsb < prev_lsb && prev_lsb - lsb >= maxlsb / 2) msb = prev_msb + maxlsb;
        else if (lsb > prev_lsb && lsb - prev_lsb > maxlsb / 2) msb = prev_msb - maxlsb;
        else msb = prev_msb;
        *msb_out = msb;
        *top = msb + lsb;
        *bottom = *top + s->delta_poc_bottom;
        return;
    }
    int offset;
    if (s->idr) offset = 0;
    else {
        int prev = d->prev_has_mmco5 ? 0 : d->prev_frame_num_offset;
        offset = d->prev_frame_num > s->frame_num ? prev + maxfn : prev;
    }
    d->frame_num_offset = offset;
    if (sps->poc_type == 1) {
        int n = sps->num_ref_frames_in_poc_cycle;
        int abs_fn = n ? offset + s->frame_num : 0;
        if (s->nal_ref_idc == 0 && abs_fn > 0) abs_fn--;
        int64_t expected = 0;
        if (abs_fn > 0) {
            int64_t delta_cycle = 0;
            for (int i = 0; i < n; i++) delta_cycle += sps->offset_for_ref_frame[i];
            int cyc = (abs_fn - 1) / n, in = (abs_fn - 1) % n;
            expected = (int64_t)cyc * delta_cycle;
            for (int i = 0; i <= in; i++) expected += sps->offset_for_ref_frame[i];
        }
        if (s->nal_ref_idc == 0) expected += sps->offset_for_non_ref_pic;
        int64_t t = expected + s->delta_poc[0];
        int64_t bt = t + sps->offset_for_top_to_bottom_field + s->delta_poc[1];
        *top = (int)t; *bottom = (int)bt;
    } else {
        int t = s->idr ? 0 : s->nal_ref_idc == 0 ? 2 * (offset + s->frame_num) - 1 : 2 * (offset + s->frame_num);
        *top = *bottom = t;
    }
    *msb_out = 0;
}

/* ---------------------------------------------------------------- reference lists */
static void sort_pics(struct h264_pic **a, int n, int (*key)(const struct h264_pic *), int desc) {
    for (int i = 1; i < n; i++) {
        struct h264_pic *p = a[i];
        int k = key(p), j = i - 1;
        while (j >= 0 && (desc ? key(a[j]) < k : key(a[j]) > k)) { a[j + 1] = a[j]; j--; }
        a[j + 1] = p;
    }
}
static int key_fnw(const struct h264_pic *p) { return p->frame_num_wrap; }
static int key_poc(const struct h264_pic *p) { return p->poc; }
static int key_lt(const struct h264_pic *p) { return p->lt_idx; }

static void build_lists(h264dec *d) {
    struct h264_slice *s = &d->sl;
    struct h264_pic *cur = d->cur;
    struct h264_pic *st[H264_MAX_PICS], *lt[H264_MAX_PICS];
    int ns = 0, nl = 0;
    for (int i = 0; i < d->n_pics; i++) {
        struct h264_pic *p = &d->pics[i];
        if (p == cur || !p->ref) continue;
        if (p->ref == 1) { p->frame_num_wrap = frame_num_wrap(d, p); st[ns++] = p; }
        else lt[nl++] = p;
    }
    sort_pics(lt, nl, key_lt, 0);
    struct h264_pic *init[2][H264_MAX_PICS * 2];
    int len[2] = { 0, 0 };
    if (s->type == 0) {
        sort_pics(st, ns, key_fnw, 1);
        for (int i = 0; i < ns; i++) init[0][len[0]++] = st[i];
        for (int i = 0; i < nl; i++) init[0][len[0]++] = lt[i];
    } else if (s->type == 1) {
        struct h264_pic *before[H264_MAX_PICS], *after[H264_MAX_PICS];
        int nb = 0, na = 0;
        for (int i = 0; i < ns; i++) {
            if (st[i]->poc <= cur->poc) before[nb++] = st[i];
            else after[na++] = st[i];
        }
        sort_pics(before, nb, key_poc, 1);
        sort_pics(after, na, key_poc, 0);
        for (int i = 0; i < nb; i++) init[0][len[0]++] = before[i];
        for (int i = 0; i < na; i++) init[0][len[0]++] = after[i];
        for (int i = 0; i < nl; i++) init[0][len[0]++] = lt[i];
        for (int i = 0; i < na; i++) init[1][len[1]++] = after[i];
        for (int i = 0; i < nb; i++) init[1][len[1]++] = before[i];
        for (int i = 0; i < nl; i++) init[1][len[1]++] = lt[i];
        if (len[1] > 1 && len[0] == len[1]) {
            int same = 1;
            for (int i = 0; i < len[0]; i++) if (init[0][i] != init[1][i]) { same = 0; break; }
            if (same) { struct h264_pic *t = init[1][0]; init[1][0] = init[1][1]; init[1][1] = t; }
        }
    }
    int maxfn = 1 << d->act_sps->log2_max_frame_num;
    for (int li = 0; li < 2; li++) {
        int n = s->num_ref[li];
        struct h264_pic *list[34];
        for (int i = 0; i <= n && i < 34; i++) list[i] = i < len[li] ? init[li][i] : 0;
        int pred = d->cur_frame_num, idx = 0;
        for (int k = 0; k < s->n_mod[li] && idx < n; k++) {
            int idc = s->mod_idc[li][k], v = s->mod_val[li][k];
            struct h264_pic *pic = 0;
            if (idc < 2) {
                int64_t diff = (int64_t)v + 1;
                if (diff > maxfn) diff = maxfn;
                int nw = idc == 0 ? pred - (int)diff : pred + (int)diff;
                if (nw < 0) nw += maxfn;
                if (nw >= maxfn) nw -= maxfn;
                pred = nw;
                int pn = nw > d->cur_frame_num ? nw - maxfn : nw;
                for (int i = 0; i < ns; i++) if (st[i]->frame_num_wrap == pn) { pic = st[i]; break; }
            } else {
                for (int i = 0; i < nl; i++) if (lt[i]->lt_idx == v) { pic = lt[i]; break; }
            }
            for (int c = n; c > idx; c--) list[c] = list[c - 1];
            list[idx++] = pic;
            int ni = idx;
            for (int c = idx; c <= n; c++) if (list[c] != pic || !pic) list[ni++] = list[c];
            for (; ni <= n; ni++) list[ni] = 0;
        }
        struct h264_pic *fallback = 0;
        for (int i = 0; i < n; i++) if (list[i]) { fallback = list[i]; break; }
        if (!fallback) fallback = d->missing;
        for (int i = 0; i < n; i++) {
            struct h264_pic *p = list[i] ? list[i] : fallback;
            s->ref[li][i] = p;
            s->ref_poc[li][i] = p->poc;
            s->ref_long[li][i] = p->ref == 2;
            s->ref_uid[li][i] = p->uid;
        }
        for (int i = n; i < 33; i++) { s->ref[li][i] = d->missing; s->ref_uid[li][i] = -1; }
    }
    if (s->type == 1) {
        s->col_long = s->ref[1][0]->ref == 2;
        if (!s->direct_spatial) h264_direct_prepare(d);
        if (s->wp_mode == 2) {
            for (int i = 0; i < s->num_ref[0]; i++)
                for (int j = 0; j < s->num_ref[1]; j++) {
                    int poc0 = s->ref_poc[0][i], poc1 = s->ref_poc[1][j];
                    int w = 32;
                    int td = h264_clip(poc1 - poc0, -128, 127);
                    if (poc1 != poc0 && !s->ref_long[0][i] && !s->ref_long[1][j]) {
                        int tb = h264_clip(cur->poc - poc0, -128, 127);
                        int tx = (16384 + h264_abs(td / 2)) / td;
                        int dsf = h264_clip((tb * tx + 32) >> 6, -1024, 1023);
                        if ((dsf >> 2) >= -64 && (dsf >> 2) <= 128) w = dsf >> 2;
                    }
                    s->implicit_w[i][j] = (int16_t)w;
                }
        }
    }
}

/* ---------------------------------------------------------------- pictures */
static struct h264_pic *latest_ref(h264dec *d) {
    struct h264_pic *best = 0;
    for (int i = 0; i < d->n_pics; i++) {
        struct h264_pic *p = &d->pics[i];
        if (p->ref && p != d->cur && (!best || p->uid > best->uid)) best = p;
    }
    return best;
}

void h264_finish_picture(h264dec *d) {
    struct h264_pic *cur = d->cur;
    if (!cur) return;
    const struct h264_slice *fs = &d->first_sl;
    /* conceal lost macroblocks */
    if (d->mbs_decoded < d->mb_count) {
        const struct h264_pic *src = 0;
        if (d->sl.type != 2 && d->sl.num_ref[0] > 0 && d->sl.ref[0][0] != d->missing) src = d->sl.ref[0][0];
        if (!src) src = latest_ref(d);
        for (int i = 0; i < d->mb_count; i++)
            if (d->mbi[i].slice < 0) h264_conceal_mb(d, i, src);
    }
    h264_deblock_picture(d);
    int top = d->cur_top_poc;
    if (fs->nal_ref_idc) {
        h264_pad_picture(d, cur);
        mark_picture(d, cur, fs);
    }
    /* POC / frame_num state for the next picture */
    if (cur->mmco5) {
        int temp = cur->poc;
        cur->poc = 0;
        top -= temp;
        d->prev_frame_num_offset = 0;
        d->prev_frame_num = 0;
    } else {
        d->prev_frame_num_offset = d->frame_num_offset;
        d->prev_frame_num = cur->frame_num;
    }
    d->prev_has_mmco5 = cur->mmco5;
    if (fs->nal_ref_idc) {
        if (cur->mmco5) { d->prev_poc_msb = 0; d->prev_poc_lsb = top; d->prev_ref_frame_num = 0; }
        else { d->prev_poc_msb = d->cur_poc_msb; d->prev_poc_lsb = fs->poc_lsb; d->prev_ref_frame_num = cur->frame_num; }
        d->have_prev_ref = 1;
    }
    cur->epoch = d->epoch;
    cur->busy = 0;
    int out = 1;
    if (d->drop_leading && cur->epoch == d->key_epoch && cur->poc < d->key_poc) out = 0;
    cur->waiting = out;
    d->cur = 0;
}

static int start_picture(h264dec *d, const struct h264_slice *s, const struct h264_sps *sps) {
    if (!d->have_sps || !same_geometry(&d->cur_sps, sps)) {
        d->cur_sps = *sps;
        d->act_sps = &d->cur_sps;
        if (setup_geometry(d, sps) < 0) { h264_set_error(d, "out of memory"); return -2; }
        d->have_sps = 1;
    } else {
        d->cur_sps = *sps;
    }
    const struct h264_sps *a = d->act_sps;
    int lvl = level_max_dpb_mbs(a->level_idc, a->constraint);
    int maxf = h264_clip(lvl / (a->mb_w * a->mb_h), 1, 16);
    maxf = h264_max(maxf, a->max_num_ref_frames);
    if (a->max_dec_buf > maxf) maxf = a->max_dec_buf;
    d->max_frames = maxf;
    d->reorder = a->reorder >= 0 ? a->reorder : (a->poc_type == 2 ? 0 : maxf);
    if (d->reorder > maxf) d->reorder = maxf;

    /* keyframe gating */
    int just_keyed = 0;
    if (d->need_key) {
        if (!s->idr && s->type != 2) return 1;   /* skip until a keyframe */
        d->need_key = 0;
        just_keyed = !s->idr;
        d->drop_leading = !s->idr;
        if (!s->idr) d->have_prev_ref = 0;
    }
    if (s->idr) { d->drop_leading = 0; d->epoch++; }
    for (int i = 0; i < s->n_mmco; i++) if (s->mmco[i].op == 5) { d->epoch++; break; }

    int maxfn = 1 << a->log2_max_frame_num;
    d->cur_frame_num = s->frame_num;
    /* gaps in frame_num (8.2.5.2): insert "non-existing" short-term frames */
    if (!s->idr && d->have_prev_ref && s->frame_num != d->prev_ref_frame_num &&
        s->frame_num != (d->prev_ref_frame_num + 1) % maxfn) {
        int missing = (s->frame_num - d->prev_ref_frame_num - 1 + maxfn) % maxfn;
        int start = (d->prev_ref_frame_num + 1) % maxfn;
        int keep = h264_max(a->max_num_ref_frames, 1);
        if (missing > keep) { start = (start + missing - keep) % maxfn; missing = keep; }
        for (int k = 0; k < missing; k++) {
            int fn = (start + k) % maxfn;
            struct h264_pic *src = latest_ref(d);
            struct h264_pic *p = h264_alloc_pic(d);
            if (!p) break;
            d->cur_frame_num = fn;
            p->frame_num = fn;
            p->nonexisting = 1;
            p->poc = src ? src->poc : 0;
            p->idr = 0;
            int n4 = d->mb_w * 4 * d->mb_h * 4, n8 = d->mb_w * 2 * d->mb_h * 2;
            if (src) memcpy(p->mem, src->mem, d->img_size);
            for (int li = 0; li < 2; li++) {
                memset(p->mv[li], 0, (size_t)n4 * 4);
                memset(p->refidx[li], -1, (size_t)n8);
                for (int i = 0; i < n8; i++) p->refuid[li][i] = -1;
            }
            for (int i = 0; i < d->n_pics; i++) {
                struct h264_pic *q = &d->pics[i];
                if (q->ref == 1) q->frame_num_wrap = q->frame_num > fn ? q->frame_num - maxfn : q->frame_num;
            }
            sliding_window(d, p);
            p->ref = 1;
            p->frame_num_wrap = fn;
            p->busy = 0;
            p->waiting = 0;
            d->prev_ref_frame_num = fn;
            d->prev_frame_num = fn;
            d->prev_frame_num_offset = d->frame_num_offset;
        }
        d->cur_frame_num = s->frame_num;
    }

    struct h264_pic *p = h264_alloc_pic(d);
    if (!p) { h264_set_error(d, "out of picture buffers"); return -1; }
    p->busy = 1;
    p->frame_num = s->frame_num;
    p->idr = s->idr;
    p->mmco5 = 0;
    p->nonexisting = 0;
    p->ref = 0;
    p->pts = d->nal_pts;
    p->crop_l = a->crop_l; p->crop_t = a->crop_t;
    p->disp_w = a->mb_w * 16 - a->crop_l - a->crop_r;
    p->disp_h = a->mb_h * 16 - a->crop_t - a->crop_b;
    p->full_range = a->full_range;
    p->matrix = a->matrix;
    d->cur = p;
    int top, bottom, msb;
    compute_poc(d, s, &top, &bottom, &msb);
    d->cur_poc_msb = msb;
    p->poc = h264_min(top, bottom);
    d->cur_top_poc = top;
    if (just_keyed) { d->key_epoch = d->epoch; d->key_poc = p->poc; }
    if (s->idr) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
    d->first_sl = *s;
    d->mbs_decoded = 0;
    d->n_slices = 0;
    for (int i = 0; i < d->mb_count; i++) d->mbi[i].slice = -1;
    return 0;
}

static int is_new_picture(const h264dec *d, const struct h264_slice *s) {
    if (!d->cur) return 1;
    const struct h264_slice *f = &d->first_sl;
    if (s->first_mb == 0) return 1;
    if (s->frame_num != f->frame_num || s->pps_id != f->pps_id || (s->nal_ref_idc == 0) != (f->nal_ref_idc == 0) ||
        s->idr != f->idr || (s->idr && s->idr_pic_id != f->idr_pic_id) || s->poc_lsb != f->poc_lsb ||
        s->delta_poc_bottom != f->delta_poc_bottom || s->delta_poc[0] != f->delta_poc[0] || s->delta_poc[1] != f->delta_poc[1])
        return 1;
    if (d->mbi[s->first_mb].slice >= 0) return 1;
    return 0;
}

/* ---------------------------------------------------------------- slice data */
int h264_decode_slice_data(h264dec *d) {
    struct h264_slice *sl = &d->sl;
    int mb = sl->first_mb;
    int cabac = d->p->cabac;
    struct h264_br *b = &d->br;
    if (cabac) {
        uint32_t pos = (b->pos + 7) >> 3;
        uint32_t endb = (b->end + 8) >> 3;
        if (pos > endb) return -1;
        h264_cabac_start(&d->cabac, b->buf + pos, b->buf + endb);
        h264_cabac_init_slice(d);
    }
    d->skip_run = -1;
    d->qp = sl->qp;
    d->last_qp_delta = 0;
    d->qpc[0] = h264_chroma_qp[h264_clip(d->qp + d->p->cqp_offset[0], 0, 51)];
    d->qpc[1] = h264_chroma_qp[h264_clip(d->qp + d->p->cqp_offset[1], 0, 51)];
    for (;;) {
        d->mb_xy = mb;
        d->mb_x = mb % d->mb_w;
        d->mb_y = mb / d->mb_w;
        if (d->mbi[mb].slice >= 0) return -1;
        int r = cabac ? h264_mb_cabac(d) : h264_mb_cavlc(d);
        if (r < 0) return -1;
        h264_recon_mb(d);
        h264_save_mb(d);
        d->mbs_decoded++;
        if (cabac) {
            if (cabac_terminate(&d->cabac)) break;
            if (d->cabac.p > d->cabac.end + 8) return -1;
        } else {
            if (d->skip_run <= 0 && b->pos >= b->end) break;
        }
        if (++mb >= d->mb_count) break;
    }
    return 0;
}

/* handles a coded slice NAL (types 1 and 5) */
int h264_slice_nal(h264dec *d, struct h264_br *b, int nal_type, int nal_ref_idc) {
    struct h264_slice *s = &d->nsl;
    int r = parse_header(d, b, s, nal_type, nal_ref_idc);
    if (r < 0) return r;
    if (s->redundant_cnt > 0) return 0;
    const struct h264_pps *p = d->pps[s->pps_id];
    const struct h264_sps *sps = d->sps[p->sps_id];
    if (d->cur && !same_geometry(&d->cur_sps, sps)) h264_finish_picture(d);
    if (is_new_picture(d, s)) {
        h264_finish_picture(d);
        r = start_picture(d, s, sps);
        if (r != 0) { d->skip_pic = 1; return r == 1 ? 0 : r; }
        d->skip_pic = 0;
    } else if (d->skip_pic) return 0;
    if (!d->cur) return 0;
    /* slice data */
    s->slice_num = d->n_slices;
    if (d->n_slices >= d->slices_cap) {
        int nc = d->slices_cap ? d->slices_cap * 2 : 16;
        struct h264_slice_info *ns = (struct h264_slice_info *)realloc(d->slices, (size_t)nc * sizeof(*ns));
        if (!ns) return -1;
        d->slices = ns;
        d->slices_cap = nc;
    }
    struct h264_slice_info *si = &d->slices[d->n_slices++];
    si->deblock_idc = s->deblock_idc;
    si->alpha_off = s->alpha_off;
    si->beta_off = s->beta_off;
    si->cqp_offset[0] = p->cqp_offset[0];
    si->cqp_offset[1] = p->cqp_offset[1];
    d->p = p;
    if (d->dq_pps != s->pps_id || d->dq_gen != d->ps_gen) {
        h264_build_dequant(d, d->act_sps, p);
        d->dq_pps = s->pps_id;
        d->dq_gen = d->ps_gen;
    }
    memcpy(&d->sl, s, sizeof(*s));
    build_lists(d);
    d->br = *b;
    if (h264_decode_slice_data(d) < 0) return 0;   /* corrupt data: the rest of the picture is concealed */
    if (d->mbs_decoded >= d->mb_count) h264_finish_picture(d);
    return 0;
}
