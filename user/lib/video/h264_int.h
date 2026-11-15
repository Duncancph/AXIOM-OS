/* AXIOM H.264 decoder: internal declarations shared by the decoder sources. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "h264.h"

/* ---- port layer: the only external functions used are the ones below. */
#ifdef H264_HOST_TEST
#include <stdlib.h>
#include <string.h>
#else
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
#endif

#define H264_INLINE static inline __attribute__((always_inline))
#define H264_ALIGN(n) __attribute__((aligned(n)))

typedef uint64_t __attribute__((may_alias, aligned(1))) h264_u64u;
typedef uint32_t __attribute__((may_alias, aligned(1))) h264_u32u;
typedef uint16_t __attribute__((may_alias, aligned(1))) h264_u16u;
#define RD64(p) (*(const h264_u64u *)(const void *)(p))
#define RD32(p) (*(const h264_u32u *)(const void *)(p))
#define RD16(p) (*(const h264_u16u *)(const void *)(p))
#define WR64(p, v) (*(h264_u64u *)(void *)(p) = (v))
#define WR32(p, v) (*(h264_u32u *)(void *)(p) = (v))
#define WR16(p, v) (*(h264_u16u *)(void *)(p) = (v))

H264_INLINE int h264_clip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
H264_INLINE uint8_t h264_clip8(int v) { return (uint8_t)(v & ~255 ? (~v >> 31) & 255 : v); }
H264_INLINE int h264_abs(int v) { return v < 0 ? -v : v; }
H264_INLINE int h264_min(int a, int b) { return a < b ? a : b; }
H264_INLINE int h264_max(int a, int b) { return a > b ? a : b; }
H264_INLINE int h264_median(int a, int b, int c) {
    int mx = h264_max(a, b), mn = h264_min(a, b);
    return c > mx ? mx : c < mn ? mn : c;
}

/* ---------------------------------------------------------------- bit reader (RBSP, big endian).
   The buffer must be followed by at least 16 zero bytes; reads past `lim` return zero bits. */
struct h264_br {
    const uint8_t *buf;
    uint32_t pos;    /* bit position */
    uint32_t lim;    /* byte-aligned bit size of the buffer (reads beyond return zeros) */
    uint32_t end;    /* bit position of the rbsp_stop_one_bit (end of payload) */
};
H264_INLINE uint32_t br_peek32(const struct h264_br *b) {
    uint32_t pos = b->pos < b->lim ? b->pos : b->lim;
    uint64_t v = __builtin_bswap64(RD64(b->buf + (pos >> 3)));
    return (uint32_t)((v << (pos & 7)) >> 32);
}
H264_INLINE void br_skip(struct h264_br *b, int n) { b->pos += (uint32_t)n; }
H264_INLINE uint32_t br_bits(struct h264_br *b, int n) {   /* 1..32 */
    uint32_t v = br_peek32(b) >> (32 - n);
    b->pos += (uint32_t)n;
    return v;
}
H264_INLINE uint32_t br_bit(struct h264_br *b) { return br_bits(b, 1); }
H264_INLINE int br_overrun(const struct h264_br *b) { return b->pos > b->end; }
uint32_t h264_br_ue_slow(struct h264_br *b);
H264_INLINE uint32_t br_ue(struct h264_br *b) {
    uint32_t v = br_peek32(b);
    if (v >= (1u << 16)) {
        int lz = __builtin_clz(v);
        int len = 2 * lz + 1;
        b->pos += (uint32_t)len;
        return (v >> (32 - len)) - 1;
    }
    return h264_br_ue_slow(b);
}
H264_INLINE int32_t br_se(struct h264_br *b) {
    uint32_t k = br_ue(b);
    int64_t v = (k & 1) ? ((int64_t)k + 1) / 2 : -((int64_t)k / 2);
    if (v > 0x3fffffff) v = 0x3fffffff;
    if (v < -0x3fffffff) v = -0x3fffffff;
    return (int32_t)v;
}
/* te(v) with range > 0 */
H264_INLINE uint32_t br_te(struct h264_br *b, int range) {
    if (range == 1) return br_bit(b) ^ 1;
    return br_ue(b);
}

/* ---------------------------------------------------------------- CABAC engine */
struct h264_cabac {
    uint64_t low;        /* codIOffset << bits | not yet consumed bits */
    uint32_t range;
    int bits;
    const uint8_t *p, *start, *end;
};
extern const uint8_t h264_cabac_lps_range[64][4];
extern const uint8_t h264_cabac_mps_next[128], h264_cabac_lps_next[128];
extern const uint8_t h264_cabac_renorm[256];
H264_INLINE void cabac_refill(struct h264_cabac *c) {
    if (c->p + 4 <= c->end) {
        c->low = (c->low << 32) | __builtin_bswap32(RD32(c->p));
        c->p += 4;
    } else {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            v <<= 8;
            if (c->p < c->end) v |= *c->p;
            c->p++;
        }
        c->low = (c->low << 32) | v;
    }
    c->bits += 32;
}
H264_INLINE int cabac_decide(struct h264_cabac *c, uint8_t *st) {
    int s = *st;
    uint32_t rlps = h264_cabac_lps_range[s >> 1][(c->range >> 6) & 3];
    uint32_t rmps = c->range - rlps;
    uint64_t scaled = (uint64_t)rmps << c->bits;
    int bin;
    if (c->low < scaled) {
        bin = s & 1;
        *st = h264_cabac_mps_next[s];
        if (rmps >= 256) { c->range = rmps; return bin; }
        c->range = rmps << 1;
        c->bits--;
    } else {
        c->low -= scaled;
        bin = (s & 1) ^ 1;
        *st = h264_cabac_lps_next[s];
        int n = h264_cabac_renorm[rlps];
        c->range = rlps << n;
        c->bits -= n;
    }
    if (c->bits < 16) cabac_refill(c);
    return bin;
}
H264_INLINE int cabac_bypass(struct h264_cabac *c) {
    c->bits--;
    uint64_t scaled = (uint64_t)c->range << c->bits;
    int bin = 0;
    if (c->low >= scaled) { c->low -= scaled; bin = 1; }
    if (c->bits < 16) cabac_refill(c);
    return bin;
}
H264_INLINE int cabac_terminate(struct h264_cabac *c) {
    c->range -= 2;
    uint64_t scaled = (uint64_t)c->range << c->bits;
    if (c->low >= scaled) return 1;
    if (c->range < 256) { c->range <<= 1; c->bits--; if (c->bits < 16) cabac_refill(c); }
    return 0;
}
void h264_cabac_start(struct h264_cabac *c, const uint8_t *p, const uint8_t *end);
/* byte position just after the arithmetic-coded data that ended with a terminate bin of 1 */
const uint8_t *h264_cabac_pcm_start(struct h264_cabac *c);

/* ---------------------------------------------------------------- parameter sets */
struct h264_sps {
    int valid;
    int profile_idc, level_idc, constraint;
    int chroma_format_idc, bit_depth_luma, bit_depth_chroma;
    int transform_bypass;
    int scaling_present;
    uint8_t sl4[6][16], sl8[2][64];   /* raster order; flat (16) when not present */
    int log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
    int offset_for_non_ref_pic, offset_for_top_to_bottom_field, num_ref_frames_in_poc_cycle;
    int offset_for_ref_frame[256];
    int max_num_ref_frames, gaps_allowed;
    int mb_w, mb_h, frame_mbs_only, mbaff, direct_8x8_inference;
    int crop_l, crop_r, crop_t, crop_b;   /* in luma samples */
    int full_range, matrix;
    int reorder;        /* max_num_reorder_frames, -1 if unknown */
    int max_dec_buf;    /* max_dec_frame_buffering, -1 if unknown */
};

struct h264_pps {
    int valid, sps_id, cabac, bottom_field_pic_order, num_slice_groups;
    int num_ref_idx[2], weighted_pred, weighted_bipred, init_qp, init_qs;
    int cqp_offset[2], deblock_ctrl, constrained_intra, redundant_pic_cnt_present, transform_8x8;
    int scaling_present;
    int list_present[8];               /* pic_scaling_list_present_flag (2: use default) */
    uint8_t sl4[6][16], sl8[2][64];   /* raw lists as parsed (raster), valid where list_present==1 */
};

/* ---------------------------------------------------------------- pictures */
struct h264_pic {
    uint8_t *mem;
    uint8_t *plane[3];           /* top-left sample of the coded picture */
    int16_t (*mv[2])[2];         /* per 4x4 block, stride mb_w*4 */
    int8_t *refidx[2];           /* per 8x8 block, stride mb_w*2 */
    int32_t *refuid[2];          /* per 8x8 block: uid of the referenced picture or -1 */
    int alloc_w, alloc_h;        /* coded size in MBs this buffer was made for */
    int stride_y, stride_c;
    int poc, frame_num, frame_num_wrap, lt_idx;
    int ref;                     /* 0 unused, 1 short-term, 2 long-term */
    int uid;
    int idr, mmco5, nonexisting;
    int waiting;                 /* waiting for output */
    int held;                    /* returned to the user since the last decode call */
    int busy;                    /* being decoded */
    int epoch;                   /* output epoch (incremented at IDR / MMCO5) */
    int64_t pts;
    int crop_l, crop_t, disp_w, disp_h, full_range, matrix;
};

/* ---------------------------------------------------------------- slices */
struct h264_mmco { int op, a, b; };

struct h264_slice_info {   /* per slice of the current picture, for deblocking */
    int deblock_idc, alpha_off, beta_off;
    int cqp_offset[2];
};

struct h264_slice {
    int nal_type, nal_ref_idc, idr;
    int first_mb, type;          /* type: 0 P, 1 B, 2 I */
    int pps_id, frame_num, idr_pic_id, poc_lsb, delta_poc_bottom, delta_poc[2], redundant_cnt;
    int direct_spatial, num_ref[2];
    int cabac_init_idc, qp, deblock_idc, alpha_off, beta_off;
    int no_output_prior, long_term_ref, adaptive_marking, n_mmco;
    struct h264_mmco mmco[66];
    int n_mod[2];
    int mod_idc[2][34], mod_val[2][34];
    /* weighted prediction: 0 default, 1 explicit, 2 implicit */
    int wp_mode;
    int luma_log2_wd, chroma_log2_wd;
    int16_t lw[2][32], lo[2][32], cw[2][32][2], co[2][32][2];
    uint8_t wflag[2][32];        /* explicit: weights differ from default for this ref */
    int16_t implicit_w[32][32];  /* implicit: w1 (w0 = 64 - w1); 32 = default */
    struct h264_pic *ref[2][33];
    int ref_poc[2][33];
    int ref_long[2][33];
    int32_t ref_uid[2][33];
    int dist_scale[33];          /* temporal direct, per refIdxL0 */
    int col_long;                /* RefPicList1[0] is long-term */
    int slice_num;
};

/* ---------------------------------------------------------------- macroblocks */
enum {
    MB_I4x4 = 1, MB_I8x8 = 2, MB_I16 = 4, MB_PCM = 8, MB_SKIP = 16, MB_DIRECT = 32,
    MB_16x16 = 64, MB_16x8 = 128, MB_8x16 = 256, MB_8x8 = 512, MB_INTER = 1024,
    MB_INTRA = MB_I4x4 | MB_I8x8 | MB_I16 | MB_PCM,
};

/* per-macroblock data kept for the whole current picture (neighbour context, deblocking) */
struct h264_mbinfo {
    uint32_t type;
    int16_t slice;               /* slice number in the picture, -1 = not decoded */
    int8_t qp;                   /* QPY (0 for I_PCM) */
    uint8_t cbp;                 /* bits 0-3 luma 8x8, 4-5 chroma (0,1,2) */
    uint32_t cbf;                /* CABAC coded_block_flag bits: 0-15 luma 4x4 (raster), 16-19 Cb AC, 20-23 Cr AC,
                                    24 luma DC, 25 Cb DC, 26 Cr DC */
    uint16_t nzdb;               /* deblocking: 4x4 luma blocks (raster) with non-zero coefficients */
    uint8_t t8x8;
    uint8_t chroma_pred;
    uint8_t direct8;             /* B: 8x8 blocks predicted in direct mode (CABAC ref_idx context) */
    uint8_t nnz[24];             /* total_coeff: 16 luma (raster), 4 Cb, 4 Cr */
    int8_t ipred[16];            /* intra 4x4 / 8x8 modes (raster) */
    uint8_t mvd[2][16][2];       /* |mvd| clamped to 127 (raster) */
};

/* cache layout: 8 columns x 5 rows; column 0 = left neighbour, row 0 = top neighbour, (x+1, y+1) = block (x, y) */
#define CI(x, y) (((y) + 1) * 8 + (x) + 1)

/* ---------------------------------------------------------------- decoder */
#define H264_MAX_PICS 40
#define H264_PAD 32
#define H264_PADC 16

struct h264dec {
    char err[96];
    int sticky_error;
    /* input */
    int avcc, nal_len_size;
    uint8_t *in;  size_t in_len, in_cap;          /* Annex B accumulation */
    struct { size_t off; int64_t pts; } in_pts[64];
    int n_in_pts;
    uint8_t *rbsp; size_t rbsp_cap;
    /* parameter sets */
    struct h264_sps *sps[32];
    struct h264_pps *pps[256];
    int ps_gen;
    const struct h264_sps *act_sps;   /* active SPS (copy below) */
    struct h264_sps cur_sps;
    int have_sps;
    int dq_pps, dq_gen;               /* which PPS / generation the dequant tables are for */
    uint32_t dq4[6][52][16];
    uint32_t dq8[2][52][64];
    /* geometry */
    int mb_w, mb_h, mb_count, width, height, stride[3];
    /* pictures */
    struct h264_pic pics[H264_MAX_PICS];
    int n_pics;
    struct h264_pic *cur;
    struct h264_pic *missing;         /* gray picture used for absent references */
    int uid_counter;
    int epoch;
    int flushing;
    int max_frames;                   /* DPB size in frames */
    int reorder;
    /* picture order / frame num state */
    int prev_poc_msb, prev_poc_lsb, prev_frame_num_offset, prev_frame_num, prev_ref_frame_num;
    int prev_has_mmco5, prev_ref_has_mmco5;
    int max_lt_idx;                   /* MaxLongTermFrameIdx, -1 = none */
    int frame_num_offset;
    int need_key;                     /* waiting for a keyframe after open/reset */
    int key_poc, key_epoch, drop_leading;
    int recovery_seen;
    /* current picture */
    struct h264_slice sl;             /* current slice */
    struct h264_slice nsl;            /* slice header being parsed */
    int skip_pic, have_prev_ref, cur_poc_msb, cur_top_poc;
    size_t img_size, pic_size;
    uint8_t direct8_cur;
    struct h264_slice first_sl;       /* first slice of the current picture (marking) */
    int cur_pps_id, cur_frame_num, cur_idr_pic_id, cur_poc_lsb, cur_nal_ref_idc, cur_idr;
    int cur_delta_poc_bottom, cur_delta_poc0, cur_delta_poc1;
    int64_t cur_pts, nal_pts;
    int mbs_decoded;
    long concealed;
    struct h264_slice_info *slices; int n_slices, slices_cap;
    struct h264_mbinfo *mbi;
    const struct h264_pps *p;         /* active PPS */
    /* per-MB working state */
    int mb_x, mb_y, mb_xy;
    uint32_t mb_type;
    int qp, qpc[2], last_qp_delta, prev_mb_skipped;
    int cbp, t8x8, chroma_pred, intra16_mode;
    int sub_type[4];
    int part_pred[4];                 /* per partition / 8x8: bit0 L0, bit1 L1 */
    int avail_l, avail_t, avail_tr, avail_tl;   /* neighbour MBs available (same slice) */
    int ia_l, ia_t, ia_tr, ia_tl;     /* available for intra prediction (constrained_intra_pred) */
    int8_t ref_c[2][40];
    int16_t mv_c[2][40][2] H264_ALIGN(16);
    uint8_t mvd_c[2][40][2];
    int8_t nnz_c[3][40];              /* -1 = unavailable */
    int8_t ipm_c[40];                 /* -1 = unavailable */
    uint32_t cbf_left, cbf_top;       /* CABAC coded_block_flag of neighbours */
    int left_type, top_type;          /* neighbour MB types (0 = unavailable) */
    int left_cbp, top_cbp;
    uint8_t direct8_l, direct8_t;
    uint32_t cbf_cur;
    uint16_t nz_luma;                 /* blocks with coefficients to add (luma4x4BlkIdx order bits) */
    uint8_t nz_chroma;                /* bits 0-3 Cb blocks, 4-7 Cr blocks */
    uint16_t nzdb_cur;
    uint8_t nnz_cur[24];
    int16_t coef[256] H264_ALIGN(16);
    int16_t coefc[2][64] H264_ALIGN(16);
    int16_t dc_y[16], dc_c[2][4];
    int bypass;                       /* TransformBypassModeFlag for this MB */
    int ipred4[16];                   /* intra 4x4/8x8 modes, luma4x4BlkIdx order */
    int col_spatial_zero;             /* scratch */
    /* CABAC */
    struct h264_cabac cabac;
    uint8_t cabac_state[460];
    /* CAVLC */
    struct h264_br br;
    int skip_run;
    /* MC scratch */
    uint8_t mc_tmp[2][16 * 16 * 2] H264_ALIGN(16);
    uint8_t edge_buf[32 * 24] H264_ALIGN(16);
    int16_t mc_tmp16[24 * 24] H264_ALIGN(16);
};

/* ---------------------------------------------------------------- tables (h264_tables.c) */
extern const int8_t h264_cabac_init[4][460][2];
extern const uint8_t h264_zigzag4[16], h264_zigzag8[64];
extern const uint32_t h264_dq_flat[64];
extern const uint8_t h264_default4[2][16], h264_default8[2][64];   /* zigzag order */
extern const uint8_t h264_chroma_qp[52];
extern const uint8_t h264_blk_xy[16];    /* luma4x4BlkIdx -> raster 4x4 index */
extern const uint8_t h264_alpha[52], h264_beta[52], h264_tc0[52][3];
extern const uint8_t h264_golomb_to_intra_cbp[48], h264_golomb_to_inter_cbp[48];
extern const uint8_t h264_sig8_ctx[63], h264_last8_ctx[63];

/* ---------------------------------------------------------------- functions */
void h264_set_error(h264dec *d, const char *msg);
int h264_parse_sps(h264dec *d, struct h264_br *b);
int h264_parse_pps(h264dec *d, struct h264_br *b);
void h264_build_dequant(h264dec *d, const struct h264_sps *s, const struct h264_pps *p);

int h264_slice_nal(h264dec *d, struct h264_br *b, int nal_type, int nal_ref_idc);
void h264_finish_picture(h264dec *d);
int h264_decode_slice_data(h264dec *d);

int h264_mb_cavlc(h264dec *d);
int h264_mb_cabac(h264dec *d);
void h264_cabac_init_slice(h264dec *d);
int h264_cabac_skip(h264dec *d);

void h264_fill_cache(h264dec *d);
void h264_mv_pred(h264dec *d, int list, int x, int y, int w, int h, int ref, int16_t mvp[2]);
void h264_pskip_mv(h264dec *d);
void h264_direct(h264dec *d, int b8_mask);   /* fills ref/mv cache for the 8x8 blocks in b8_mask */
void h264_direct_prepare(h264dec *d);
void h264_fill_part(h264dec *d, int list, int x, int y, int w, int h, int ref, int mx, int my);

void h264_recon_mb(h264dec *d);
void h264_save_mb(h264dec *d);
void h264_conceal_mb(h264dec *d, int mb_xy, const struct h264_pic *src);

void h264_intra4x4(uint8_t *dst, int stride, int mode, int avail_l, int avail_t, int avail_tr, int avail_tl);
void h264_intra8x8(uint8_t *dst, int stride, int mode, int avail_l, int avail_t, int avail_tr, int avail_tl);
void h264_intra16x16(uint8_t *dst, int stride, int mode, int avail_l, int avail_t, int avail_tl);
void h264_intra_chroma(uint8_t *dst, int stride, int mode, int avail_l, int avail_t, int avail_tl);

void h264_idct4_add(uint8_t *dst, int stride, int16_t *blk);
void h264_idct4_dc_add(uint8_t *dst, int stride, int16_t *blk);
void h264_idct8_add(uint8_t *dst, int stride, int16_t *blk);
void h264_idct8_dc_add(uint8_t *dst, int stride, int16_t *blk);
void h264_luma_dc_dequant(int16_t *coef, const int16_t *dc, int qmul);
void h264_chroma_dc_dequant(int16_t *coef, const int16_t *dc, int qmul);

void h264_mc_luma(uint8_t *dst, int dst_stride, const struct h264_pic *ref, int stride, int pw, int ph,
                  int x, int y, int mvx, int mvy, int w, int h, uint8_t *edge, int16_t *tmp);
void h264_mc_chroma(uint8_t *dst, int dst_stride, const uint8_t *src_plane, int stride, int pw, int ph,
                    int x, int y, int mvx, int mvy, int w, int h, uint8_t *edge);
void h264_avg(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int w, int h);
void h264_weight(uint8_t *dst, int dst_stride, int w, int h, int log2wd, int wt, int ofs);
void h264_biweight(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int w, int h,
                   int log2wd, int w0, int w1, int ofs);

void h264_deblock_picture(h264dec *d);
void h264_pad_picture(h264dec *d, struct h264_pic *p);

struct h264_pic *h264_alloc_pic(h264dec *d);
void h264_dpb_store(h264dec *d, struct h264_pic *p);
int h264_alloc_frame_data(h264dec *d);
