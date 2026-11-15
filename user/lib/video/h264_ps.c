/* AXIOM H.264 decoder: sequence and picture parameter sets (7.3.2.1, 7.3.2.2, Annex E VUI). */
#include "h264_int.h"

uint32_t h264_br_ue_slow(struct h264_br *b) {
    uint32_t v = br_peek32(b);
    if (v == 0) {                       /* 32+ leading zeros: invalid */
        b->pos = b->end + 1 > b->pos ? b->end + 1 : b->pos + 32;
        return 0xffffffffu;
    }
    int lz = __builtin_clz(v);
    b->pos += (uint32_t)lz + 1;
    uint32_t suf = lz ? br_bits(b, lz) : 0;
    return (uint32_t)(((uint64_t)1 << lz) - 1 + suf);
}

/* parses one scaling list (zigzag coded) into raster order; returns 2 if "use default", 1 otherwise */
static int scaling_list(struct h264_br *b, uint8_t *out, int n) {
    const uint8_t *zz = n == 16 ? h264_zigzag4 : h264_zigzag8;
    int last = 8, next = 8;
    for (int j = 0; j < n; j++) {
        if (next) {
            int delta = br_se(b);
            if (delta < -128 || delta > 127) return -1;
            next = (last + delta + 256) & 255;
            if (j == 0 && next == 0) return 2;
        }
        out[zz[j]] = (uint8_t)(next ? next : last);
        last = out[zz[j]];
    }
    return 1;
}

static void default_list(uint8_t *out, int n, int inter) {
    const uint8_t *zz = n == 16 ? h264_zigzag4 : h264_zigzag8;
    const uint8_t *src = n == 16 ? h264_default4[inter] : h264_default8[inter];
    for (int j = 0; j < n; j++) out[zz[j]] = src[j];
}

static void skip_hrd(struct h264_br *b) {
    uint32_t cnt = br_ue(b);
    if (cnt > 31) { b->pos = b->end + 1; return; }
    br_bits(b, 8);
    for (uint32_t i = 0; i <= cnt; i++) { br_ue(b); br_ue(b); br_bit(b); }
    br_bits(b, 20);
}

int h264_parse_sps(h264dec *d, struct h264_br *b) {
    struct h264_sps s;
    memset(&s, 0, sizeof(s));
    s.profile_idc = (int)br_bits(b, 8);
    s.constraint = (int)br_bits(b, 8);
    s.level_idc = (int)br_bits(b, 8);
    uint32_t id = br_ue(b);
    if (id > 31) return -1;
    s.chroma_format_idc = 1;
    s.bit_depth_luma = s.bit_depth_chroma = 8;
    for (int i = 0; i < 6; i++) memset(s.sl4[i], 16, 16);
    for (int i = 0; i < 2; i++) memset(s.sl8[i], 16, 64);
    int p = s.profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
        p == 138 || p == 139 || p == 134 || p == 135) {
        uint32_t cf = br_ue(b);
        if (cf > 3) return -1;
        s.chroma_format_idc = (int)cf;
        if (cf == 3) br_bit(b);   /* separate_colour_plane_flag */
        uint32_t bl = br_ue(b), bc = br_ue(b);
        if (bl > 6 || bc > 6) return -1;
        s.bit_depth_luma = (int)bl + 8;
        s.bit_depth_chroma = (int)bc + 8;
        s.transform_bypass = (int)br_bit(b);
        s.scaling_present = (int)br_bit(b);
        if (s.scaling_present) {
            int n = cf != 3 ? 8 : 12;
            for (int i = 0; i < n; i++) {
                int present = (int)br_bit(b);
                if (i >= 8) { if (present) { uint8_t tmp[64]; if (scaling_list(b, tmp, 64) < 0) return -1; } continue; }
                uint8_t *dst = i < 6 ? s.sl4[i] : s.sl8[i - 6];
                int sz = i < 6 ? 16 : 64;
                int r = present ? scaling_list(b, dst, sz) : 0;
                if (r < 0) return -1;
                if (r == 2) default_list(dst, sz, i < 6 ? i >= 3 : i == 7);
                else if (r == 0) {   /* fall-back rule A */
                    if (i == 0 || i == 3) default_list(dst, 16, i == 3);
                    else if (i < 6) memcpy(dst, s.sl4[i - 1], 16);
                    else default_list(dst, 64, i == 7);
                }
            }
        }
    }
    uint32_t v = br_ue(b);
    if (v > 12) return -1;
    s.log2_max_frame_num = (int)v + 4;
    v = br_ue(b);
    if (v > 2) return -1;
    s.poc_type = (int)v;
    if (s.poc_type == 0) {
        v = br_ue(b);
        if (v > 12) return -1;
        s.log2_max_poc_lsb = (int)v + 4;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = (int)br_bit(b);
        s.offset_for_non_ref_pic = br_se(b);
        s.offset_for_top_to_bottom_field = br_se(b);
        v = br_ue(b);
        if (v > 255) return -1;
        s.num_ref_frames_in_poc_cycle = (int)v;
        for (int i = 0; i < s.num_ref_frames_in_poc_cycle; i++) s.offset_for_ref_frame[i] = br_se(b);
    }
    v = br_ue(b);
    if (v > 16) return -1;
    s.max_num_ref_frames = (int)v;
    s.gaps_allowed = (int)br_bit(b);
    uint32_t w = br_ue(b), h = br_ue(b);
    s.frame_mbs_only = (int)br_bit(b);
    if (w > 1023 || h > 1023) return -1;
    s.mb_w = (int)w + 1;
    s.mb_h = ((int)h + 1) * (2 - s.frame_mbs_only);
    if (s.mb_w * s.mb_h > 139264) return -1;          /* level 6.2 limit */
    if (!s.frame_mbs_only) s.mbaff = (int)br_bit(b);
    s.direct_8x8_inference = (int)br_bit(b);
    if (br_bit(b)) {
        uint32_t cl = br_ue(b), cr = br_ue(b), ct = br_ue(b), cb = br_ue(b);
        int cux = (s.chroma_format_idc == 1 || s.chroma_format_idc == 2) ? 2 : 1;
        int cuy = (s.chroma_format_idc == 1 ? 2 : 1) * (2 - s.frame_mbs_only);
        if ((uint64_t)(cl + cr) * cux >= (uint64_t)s.mb_w * 16 || (uint64_t)(ct + cb) * cuy >= (uint64_t)s.mb_h * 16) {
            cl = cr = ct = cb = 0;   /* invalid cropping: ignore */
        }
        s.crop_l = (int)cl * cux; s.crop_r = (int)cr * cux;
        s.crop_t = (int)ct * cuy; s.crop_b = (int)cb * cuy;
    }
    s.full_range = 0;
    s.matrix = 2;
    s.reorder = -1;
    s.max_dec_buf = -1;
    if (br_bit(b)) {   /* VUI: tolerate truncation */
        struct h264_br save = *b;
        int ok = 1;
        if (br_bit(b)) { if (br_bits(b, 8) == 255) br_bits(b, 32); }
        if (br_bit(b)) br_bit(b);
        int full = 0, matrix = 2;
        if (br_bit(b)) {
            br_bits(b, 3);
            full = (int)br_bit(b);
            if (br_bit(b)) { br_bits(b, 16); matrix = (int)br_bits(b, 8); }
        }
        if (br_bit(b)) { br_ue(b); br_ue(b); }
        if (br_bit(b)) { br_bits(b, 32); br_bits(b, 32); br_bit(b); }
        int nal_hrd = (int)br_bit(b);
        if (nal_hrd) skip_hrd(b);
        int vcl_hrd = (int)br_bit(b);
        if (vcl_hrd) skip_hrd(b);
        if (nal_hrd || vcl_hrd) br_bit(b);
        br_bit(b);   /* pic_struct_present_flag */
        if (b->pos > b->end) ok = 0;
        s.full_range = full;
        s.matrix = matrix;
        if (ok && br_bit(b)) {
            br_bit(b); br_ue(b); br_ue(b); br_ue(b); br_ue(b);
            uint32_t ro = br_ue(b), mdb = br_ue(b);
            if (b->pos <= b->end && ro <= 16 && mdb <= 16) {
                s.reorder = (int)ro;
                s.max_dec_buf = (int)mdb;
            }
        }
        (void)save;
    }
    s.valid = 1;
    if (!d->sps[id]) {
        d->sps[id] = (struct h264_sps *)malloc(sizeof(struct h264_sps));
        if (!d->sps[id]) return -1;
    }
    *d->sps[id] = s;
    d->ps_gen++;
    return 0;
}

int h264_parse_pps(h264dec *d, struct h264_br *b) {
    struct h264_pps p;
    memset(&p, 0, sizeof(p));
    uint32_t id = br_ue(b), sid = br_ue(b);
    if (id > 255 || sid > 31) return -1;
    p.sps_id = (int)sid;
    p.cabac = (int)br_bit(b);
    p.bottom_field_pic_order = (int)br_bit(b);
    uint32_t ng = br_ue(b);
    if (ng > 7) return -1;
    p.num_slice_groups = (int)ng + 1;
    if (ng > 0) {
        /* slice groups (FMO) are not supported; keep the PPS so slices can be rejected with a clear message */
        p.valid = 1;
        goto store;
    }
    uint32_t r0 = br_ue(b), r1 = br_ue(b);
    if (r0 > 31 || r1 > 31) return -1;
    p.num_ref_idx[0] = (int)r0 + 1;
    p.num_ref_idx[1] = (int)r1 + 1;
    p.weighted_pred = (int)br_bit(b);
    p.weighted_bipred = (int)br_bits(b, 2);
    if (p.weighted_bipred > 2) return -1;
    int qp = br_se(b), qs = br_se(b);
    if (qp < -26 || qp > 25 || qs < -26 || qs > 25) return -1;
    p.init_qp = 26 + qp;
    p.init_qs = 26 + qs;
    int co = br_se(b);
    if (co < -12 || co > 12) return -1;
    p.cqp_offset[0] = p.cqp_offset[1] = co;
    p.deblock_ctrl = (int)br_bit(b);
    p.constrained_intra = (int)br_bit(b);
    p.redundant_pic_cnt_present = (int)br_bit(b);
    if (b->pos < b->end) {   /* more_rbsp_data() */
        p.transform_8x8 = (int)br_bit(b);
        p.scaling_present = (int)br_bit(b);
        if (p.scaling_present) {
            const struct h264_sps *sps = d->sps[sid];
            int cf = sps ? sps->chroma_format_idc : 1;
            int n = 6 + ((cf != 3) ? 2 : 6) * p.transform_8x8;
            for (int i = 0; i < n; i++) {
                int present = (int)br_bit(b);
                if (i >= 8) { if (present) { uint8_t tmp[64]; if (scaling_list(b, tmp, 64) < 0) return -1; } continue; }
                if (!present) { p.list_present[i] = 0; continue; }
                uint8_t *dst = i < 6 ? p.sl4[i] : p.sl8[i - 6];
                int r = scaling_list(b, dst, i < 6 ? 16 : 64);
                if (r < 0) return -1;
                p.list_present[i] = r;
            }
        }
        int c2 = br_se(b);
        if (c2 < -12 || c2 > 12) return -1;
        p.cqp_offset[1] = c2;
    }
    if (b->pos > b->end + 8) return -1;
    p.valid = 1;
store:
    if (!d->pps[id]) {
        d->pps[id] = (struct h264_pps *)malloc(sizeof(struct h264_pps));
        if (!d->pps[id]) return -1;
    }
    *d->pps[id] = p;
    d->ps_gen++;
    return 0;
}

/* resolves the scaling lists (Table 7-2) and builds the dequantisation tables for an SPS/PPS pair */
void h264_build_dequant(h264dec *d, const struct h264_sps *s, const struct h264_pps *p) {
    static const uint8_t v4[6][3] = { { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 }, { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 } };
    static const uint8_t v8[6][6] = {
        { 20, 18, 32, 19, 25, 24 }, { 22, 19, 35, 21, 28, 26 }, { 26, 23, 42, 24, 33, 31 },
        { 28, 25, 45, 26, 35, 33 }, { 32, 28, 51, 30, 40, 38 }, { 36, 32, 58, 34, 46, 43 },
    };
    uint8_t l4[6][16], l8[2][64];
    if (!p->scaling_present) {
        memcpy(l4, s->sl4, sizeof(l4));
        memcpy(l8, s->sl8, sizeof(l8));
    } else {
        for (int i = 0; i < 8; i++) {
            int sz = i < 6 ? 16 : 64;
            uint8_t *dst = i < 6 ? l4[i] : l8[i - 6];
            int pr = p->list_present[i];
            if (pr == 1) memcpy(dst, i < 6 ? p->sl4[i] : p->sl8[i - 6], (size_t)sz);
            else if (pr == 2) default_list(dst, sz, i < 6 ? i >= 3 : i == 7);
            else if (!s->scaling_present) {   /* fall-back rule A */
                if (i == 0 || i == 3) default_list(dst, 16, i == 3);
                else if (i < 6) memcpy(dst, l4[i - 1], 16);
                else default_list(dst, 64, i == 7);
            } else {                          /* fall-back rule B */
                if (i == 0 || i == 3) memcpy(dst, s->sl4[i], 16);
                else if (i < 6) memcpy(dst, l4[i - 1], 16);
                else memcpy(dst, s->sl8[i - 6], 64);
            }
        }
    }
    for (int q = 0; q < 52; q++) {
        int m = q % 6, sh = q / 6;
        for (int i = 0; i < 6; i++)
            for (int k = 0; k < 16; k++) {
                int x = k & 3, y = k >> 2;
                int idx = ((x & 1) == 0 && (y & 1) == 0) ? 0 : ((x & 1) && (y & 1)) ? 1 : 2;
                d->dq4[i][q][k] = ((uint32_t)l4[i][k] * v4[m][idx]) << (sh + 2);
            }
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < 64; k++) {
                int x = k & 7, y = k >> 3, idx;
                if ((x & 3) == 0 && (y & 3) == 0) idx = 0;
                else if ((x & 1) && (y & 1)) idx = 1;
                else if ((x & 3) == 2 && (y & 3) == 2) idx = 2;
                else if (((x & 3) == 0 && (y & 1)) || ((x & 1) && (y & 3) == 0)) idx = 3;
                else if (((x & 3) == 0 && (y & 3) == 2) || ((x & 3) == 2 && (y & 3) == 0)) idx = 4;
                else idx = 5;
                d->dq8[i][q][k] = ((uint32_t)l8[i][k] * v8[m][idx]) << sh;
            }
    }
}
