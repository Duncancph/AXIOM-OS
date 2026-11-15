/* AXIOM AAC decoder: configuration (AudioSpecificConfig, ADTS, PCE), raw_data_block / channel element
 * parsing, spectral decoding, stereo tools, PNS, TNS, prediction, filterbank and output (downmix).
 * Freestanding C11: no libc / libm. See README.md. */
#include "aac_int.h"

const int aac_rates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };

static struct aac_vlc vlc_sf, vlc_spec[11];
static int tables_ready;
static const uint8_t cb_dim[12] = { 0, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2 };
static const uint8_t cb_unsigned[12] = { 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1 };
static const uint8_t cb_mod[12] = { 0, 3, 3, 3, 3, 9, 9, 8, 8, 13, 13, 17 };
static const int8_t cb_off[12] = { 0, 1, 1, 0, 0, 4, 4, 0, 0, 0, 0, 0 };

static int init_tables(void) {
    if (tables_ready) return 1;
    aac_dsp_init();
    struct aac_hsrc sf = { 0, aac_sf_code, aac_sf_bits, 121 };
    if (!aac_vlc_build(&vlc_sf, &sf, 9, -60)) return 0;
    for (int i = 0; i < 11; i++)
        if (!aac_vlc_build(&vlc_spec[i], &aac_spec_src[i], 9, 0)) return 0;
    tables_ready = 1;
    return 1;
}

/* ---------------------------------------------------------------- configuration */
static int get_aot(struct aac_bits *b) {
    int aot = (int)aac_get(b, 5);
    if (aot == 31) aot = 32 + (int)aac_get(b, 6);
    return aot;
}

static int get_rate(struct aac_bits *b, int *sfi) {
    int i = (int)aac_get(b, 4);
    *sfi = i;
    if (i == 15) return (int)aac_getl(b, 24);
    return aac_rates[i];
}

static int rate_index(int rate) {   /* nearest table index for explicit rates (as in the standard) */
    static const int thr[12] = { 92017, 75132, 55426, 46009, 37566, 27713, 23004, 18783, 13856, 11502, 9391, 0 };
    for (int i = 0; i < 12; i++)
        if (rate >= thr[i]) return i;
    return 11;
}

static void parse_pce(struct aac_bits *b, struct aac_pce *p, int *sfi_out) {
    aac_get(b, 4);                 /* element_instance_tag */
    aac_get(b, 2);                 /* object_type */
    int sfi = (int)aac_get(b, 4);
    if (sfi_out) *sfi_out = sfi;
    p->nfront = (int)aac_get(b, 4);
    p->nside = (int)aac_get(b, 4);
    p->nback = (int)aac_get(b, 4);
    p->nlfe = (int)aac_get(b, 2);
    int nassoc = (int)aac_get(b, 3);
    int ncc = (int)aac_get(b, 4);
    if (aac_get1(b)) aac_get(b, 4);
    if (aac_get1(b)) aac_get(b, 4);
    if (aac_get1(b)) aac_get(b, 3);
    for (int i = 0; i < p->nfront; i++) { p->front_cpe[i] = (uint8_t)aac_get1(b); aac_get(b, 4); }
    for (int i = 0; i < p->nside; i++) { p->side_cpe[i] = (uint8_t)aac_get1(b); aac_get(b, 4); }
    for (int i = 0; i < p->nback; i++) { p->back_cpe[i] = (uint8_t)aac_get1(b); aac_get(b, 4); }
    aac_skip(b, 4 * p->nlfe + 4 * nassoc + 5 * ncc);
}

static int pce_channels(const struct aac_pce *p) {
    int n = p->nlfe;
    for (int i = 0; i < p->nfront; i++) n += 1 + p->front_cpe[i];
    for (int i = 0; i < p->nside; i++) n += 1 + p->side_cpe[i];
    for (int i = 0; i < p->nback; i++) n += 1 + p->back_cpe[i];
    return n;
}

static int cfg_channels(int cfg) {
    static const int n[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 0, 8, 0 };
    return n[cfg & 15];
}

static void set_output(aacdec *d) {
    int n = d->have_pce ? pce_channels(&d->pce) : cfg_channels(d->chan_cfg);
    d->out_ch = n == 1 ? 1 : 2;
    if (d->ps == 1) d->out_ch = 2;
    d->out_rate = d->sbr == 1 ? (d->ext_rate ? d->ext_rate : 2 * d->core_rate) : d->core_rate;
}

static int parse_asc(aacdec *d, const uint8_t *cfg, int len) {
    uint8_t tmp[64 + 8];
    uint8_t *buf = len <= 64 ? tmp : malloc((size_t)len + 8);
    if (!buf) return 0;
    memcpy(buf, cfg, (size_t)len);
    memset(buf + len, 0, 8);
    struct aac_bits b = { buf, 0, len * 8, 0 };
    int ok = 0;
    int aot = get_aot(&b), sfi;
    d->core_rate = get_rate(&b, &sfi);
    d->chan_cfg = (int)aac_get(&b, 4);
    d->sbr = -1;
    d->ps = -1;
    if (aot == AOT_SBR || aot == AOT_PS) {
        d->sbr = 1;
        d->ps = aot == AOT_PS;
        int esfi;
        d->ext_rate = get_rate(&b, &esfi);
        aot = get_aot(&b);
    }
    if (aot != AOT_LC && aot != AOT_MAIN && aot != AOT_LTP) goto done;
    d->aot = aot;
    if (sfi == 15) sfi = rate_index(d->core_rate);
    d->sfi = sfi;
    if (aac_get1(&b)) goto done;            /* frameLengthFlag: 960-sample frames are not supported */
    if (aac_get1(&b)) aac_get(&b, 14);      /* dependsOnCoreCoder */
    int ext = (int)aac_get1(&b);
    if (d->chan_cfg == 0) {
        parse_pce(&b, &d->pce, 0);
        aac_align(&b, 0);
        aac_skip(&b, 8 * (int)aac_get(&b, 8));   /* comment */
        d->have_pce = 1;
        if (b.err || pce_channels(&d->pce) == 0 || pce_channels(&d->pce) > AAC_MAX_CH) goto done;
    } else if (cfg_channels(d->chan_cfg) == 0) goto done;
    if (ext) aac_get1(&b);                  /* extensionFlag3 (AOT 2 has no other fields) */
    /* backward compatible explicit SBR / PS signalling */
    if (d->sbr == -1 && aac_left(&b) >= 16) {
        if (aac_get(&b, 11) == 0x2b7) {
            int eaot = get_aot(&b);
            if (eaot == AOT_SBR) {
                d->sbr = (int)aac_get1(&b);
                if (d->sbr) {
                    int esfi;
                    d->ext_rate = get_rate(&b, &esfi);
                    if (aac_left(&b) >= 12 && aac_get(&b, 11) == 0x548) d->ps = (int)aac_get1(&b);
                }
            }
        }
    }
    if (d->core_rate <= 0 || d->core_rate > 96000) goto done;
    if (d->sbr == 1 && d->ext_rate == d->core_rate) d->sbr = -1;
    if (d->sbr == 0) d->ps = 0;
    if ((d->ps == -1 && d->aot != AOT_LC) || (d->have_pce ? pce_channels(&d->pce) : cfg_channels(d->chan_cfg)) != 1)
        if (d->ps != 1) d->ps = 0;
    ok = 1;
done:
    if (buf != tmp) free(buf);
    d->configured = ok;
    if (ok) set_output(d);
    return ok;
}

/* ADTS header; returns header length or 0 */
static int parse_adts(aacdec *d, const uint8_t *p, int len, int *frame_len, int *nblocks) {
    if (len < 7 || p[0] != 0xff || (p[1] & 0xf6) != 0xf0) return 0;
    int prot_absent = p[1] & 1;
    int profile = p[2] >> 6;
    int sfi = (p[2] >> 2) & 15;
    int cc = (p[2] & 1) << 2 | p[3] >> 6;
    int flen = (p[3] & 3) << 11 | p[4] << 3 | p[5] >> 5;
    *nblocks = (p[6] & 3) + 1;
    int hlen = prot_absent ? 7 : 9;
    if (sfi > 12 || flen < hlen) return 0;
    *frame_len = flen;
    int aot = profile + 1;
    if (aot == AOT_SSR) return 0;
    if (!d->configured || d->sfi != sfi || d->chan_cfg != cc || d->aot != aot || !d->adts) {
        int keep_sbr = d->configured && d->adts && d->sfi == sfi;
        d->aot = aot;
        d->sfi = sfi;
        d->core_rate = aac_rates[sfi];
        d->chan_cfg = cc;
        d->have_pce = 0;
        if (!keep_sbr) {
            d->sbr = -1;
            d->ps = cc == 1 && aot == AOT_LC ? -1 : 0;
            d->ext_rate = 0;
        }
        d->adts = 1;
        d->configured = 1;
        set_output(d);
    }
    return hlen;
}

/* ---------------------------------------------------------------- channel elements */
static int elem_roles(aacdec *d, int type, int k, int *r) {
    /* returns the roles of the k-th element of that type for the current configuration */
    r[0] = r[1] = R_NONE;
    if (d->have_pce) {
        const struct aac_pce *p = &d->pce;
        int ks = 0, kc = 0, kl = 0, pairs = 0;
        for (int sec = 0; sec < 3; sec++) {
            int n = sec == 0 ? p->nfront : sec == 1 ? p->nside : p->nback;
            const uint8_t *cpe = sec == 0 ? p->front_cpe : sec == 1 ? p->side_cpe : p->back_cpe;
            for (int i = 0; i < n; i++) {
                int is_cpe = cpe[i];
                int mine = (is_cpe && type == EL_CPE && kc == k) || (!is_cpe && type == EL_SCE && ks == k);
                if (mine) {
                    if (sec == 0) {
                        if (is_cpe) { r[0] = pairs ? R_FLC : R_FL; r[1] = pairs ? R_FRC : R_FR; }
                        else r[0] = R_FC;
                    } else if (sec == 1) {
                        if (is_cpe) { r[0] = R_SL; r[1] = R_SR; } else r[0] = R_BC;
                    } else {
                        if (is_cpe) { r[0] = R_BL; r[1] = R_BR; } else r[0] = R_BC;
                    }
                    return 1;
                }
                if (is_cpe) { kc++; if (sec == 0) pairs++; } else ks++;
            }
        }
        if (type == EL_LFE && kl + k < p->nlfe) { r[0] = R_LFE; return 1; }
        return 0;
    }
    static const struct { int8_t type, k, r0, r1; } map[] = {
        /* cfg 1..7, 11, 12, 14: SCE / CPE / LFE in order */
        { EL_SCE, 0, R_FC, 0 },                                                    /* 1 */
        { EL_CPE, 0, R_FL, R_FR },                                                 /* 2 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR },                         /* 3 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_SCE, 1, R_BC, 0 }, /* 4 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_CPE, 1, R_BL, R_BR },   /* 5 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_CPE, 1, R_BL, R_BR }, { EL_LFE, 0, R_LFE, 0 }, /* 6 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_CPE, 1, R_SL, R_SR }, { EL_CPE, 2, R_BL, R_BR }, { EL_LFE, 0, R_LFE, 0 }, /* 7 (as 7.1, like ffmpeg) */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_CPE, 1, R_SL, R_SR }, { EL_SCE, 1, R_BC, 0 }, { EL_LFE, 0, R_LFE, 0 }, /* 11 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_CPE, 1, R_SL, R_SR }, { EL_CPE, 2, R_BL, R_BR }, { EL_LFE, 0, R_LFE, 0 }, /* 12 */
        { EL_SCE, 0, R_FC, 0 }, { EL_CPE, 0, R_FL, R_FR }, { EL_CPE, 1, R_BL, R_BR }, { EL_LFE, 0, R_LFE, 0 }, { EL_CPE, 2, R_FLC, R_FRC }, /* 14 */
    };
    static const uint8_t start[16] = { 0, 0, 1, 2, 4, 7, 10, 14, 0, 0, 0, 19, 24, 0, 29, 0 };
    static const uint8_t count[16] = { 0, 1, 1, 2, 3, 3, 4, 5, 0, 0, 0, 5, 5, 0, 5, 0 };
    int cfg = d->chan_cfg & 15;
    for (int i = start[cfg]; i < start[cfg] + count[cfg]; i++)
        if (map[i].type == type && map[i].k == k) { r[0] = map[i].r0; r[1] = map[i].r1; return 1; }
    /* broken streams: a lone SCE in a stereo config, or a CPE in a mono config */
    if (cfg == 2 && type == EL_SCE && k == 0) { r[0] = R_FC; return 1; }
    if (cfg == 1 && type == EL_CPE && k == 0) { r[0] = R_FL; r[1] = R_FR; return 1; }
    return 0;
}

static struct aac_chan *new_chan(aacdec *d) {
    struct aac_chan *c = calloc(1, sizeof *c);
    if (!c) return 0;
    if (d->aot == AOT_LTP) {
        c->ltp_buf = calloc(3072, sizeof(float));
        if (!c->ltp_buf) { free(c); return 0; }
    }
    if (d->aot == AOT_MAIN) {
        c->pred = calloc(1024, sizeof(struct aac_pred));
        if (!c->pred) { free(c); return 0; }
        for (int i = 0; i < 1024; i++) { c->pred[i].var0 = c->pred[i].var1 = 1; }
    }
    return c;
}

static struct aac_elem *get_elem(aacdec *d, int type, int k) {
    for (int i = 0; i < d->nelem; i++)
        if (d->elem[i].type == type && d->elem_type_idx[i] == k) return &d->elem[i];
    if (d->nelem >= AAC_MAX_ELEM) return 0;
    int nch = type == EL_CPE ? 2 : 1;
    if (d->nch + nch > AAC_MAX_CH) return 0;
    int r[2];
    elem_roles(d, type, k, r);
    struct aac_elem *e = &d->elem[d->nelem];
    memset(e, 0, sizeof *e);
    e->type = type;
    e->nch = nch;
    for (int i = 0; i < nch; i++) {
        struct aac_chan *c = new_chan(d);
        if (!c) return 0;
        c->role = r[i];
        e->ch[i] = d->nch;
        d->ch[d->nch++] = c;
    }
    d->elem_type_idx[d->nelem] = k;
    d->nelem++;
    return e;
}

/* ---------------------------------------------------------------- ICS */
static int decode_ltp_data(struct aac_ics *ics, struct aac_bits *b) {
    static const float ltp_coef[8] = { 0.570829f, 0.696616f, 0.813004f, 0.911304f, 0.984900f, 1.067894f, 1.194601f, 1.369533f };
    ics->ltp_lag = (int)aac_get(b, 11);
    ics->ltp_coef = ltp_coef[aac_get(b, 3)];
    int n = ics->max_sfb < 40 ? ics->max_sfb : 40;
    for (int i = 0; i < n; i++) ics->ltp_used[i] = (uint8_t)aac_get1(b);
    for (int i = n; i < 48; i++) ics->ltp_used[i] = 0;
    return 1;
}

static int decode_ics_info(aacdec *d, struct aac_ics *ics, struct aac_bits *b) {
    aac_get1(b);
    ics->ws = (int)aac_get(b, 2);
    ics->shape = (int)aac_get1(b);
    ics->pred_present = 0;
    ics->ltp_present = 0;
    ics->pdp = 0;
    if (ics->ws == EIGHT_SHORT) {
        ics->max_sfb = (int)aac_get(b, 4);
        int grouping = (int)aac_get(b, 7);
        ics->num_windows = 8;
        ics->num_groups = 0;
        ics->group_len[0] = 1;
        for (int i = 0; i < 7; i++) {
            if (grouping & (1 << (6 - i))) ics->group_len[ics->num_groups]++;
            else ics->group_len[++ics->num_groups] = 1;
        }
        ics->num_groups++;
        ics->swb = aac_swb_short[d->sfi];
        ics->num_swb = aac_num_swb_short[d->sfi];
    } else {
        ics->max_sfb = (int)aac_get(b, 6);
        ics->num_windows = 1;
        ics->num_groups = 1;
        ics->group_len[0] = 1;
        ics->swb = aac_swb_long[d->sfi];
        ics->num_swb = aac_num_swb_long[d->sfi];
        if (aac_get1(b)) {   /* predictor_data_present */
            ics->pdp = 1;
            if (d->aot == AOT_MAIN) {
                ics->pred_present = 1;
                ics->pred_reset_group = aac_get1(b) ? (int)aac_get(b, 5) : 0;
                if (ics->pred_reset_group > 30) return 0;
                int n = ics->max_sfb < aac_pred_sfb_max[d->sfi] ? ics->max_sfb : aac_pred_sfb_max[d->sfi];
                /* bands from n on keep their previous flags (ffmpeg does this; it matters for bit-exactness) */
                for (int i = 0; i < n; i++) ics->pred_used[i] = (uint8_t)aac_get1(b);
            } else if (d->aot == AOT_LTP) {
                ics->ltp_present = (int)aac_get1(b);
                if (ics->ltp_present) decode_ltp_data(ics, b);
            } else return 0;
        }
    }
    if (ics->max_sfb > ics->num_swb) return 0;
    return !b->err;
}

static int decode_section(struct aac_chan *c, struct aac_bits *b) {
    struct aac_ics *ics = &c->ics;
    int sbits = ics->ws == EIGHT_SHORT ? 3 : 5;
    int esc = (1 << sbits) - 1;
    int idx = 0;
    for (int g = 0; g < ics->num_groups; g++) {
        int k = 0;
        while (k < ics->max_sfb) {
            int cb = (int)aac_get(b, 4);
            if (cb == 12) return 0;
            int len = 0, inc;
            do {
                inc = (int)aac_get(b, sbits);
                len += inc;
                if (b->err) return 0;
            } while (inc == esc);
            if (k + len > ics->max_sfb) return 0;
            for (int i = 0; i < len; i++) c->band_type[idx++] = (uint8_t)cb;
            k += len;
        }
    }
    return 1;
}

static int decode_scalefactors(aacdec *d, struct aac_chan *c, struct aac_bits *b, int global_gain) {
    struct aac_ics *ics = &c->ics;
    int off[3] = { global_gain, global_gain - 90, 0 };
    int noise_first = 1;
    int n = ics->num_groups * ics->max_sfb;
    for (int idx = 0; idx < n; idx++) {
        int bt = c->band_type[idx];
        if (bt == ZERO_HCB) {
            c->sf[idx] = 0;
        } else if (bt == INTENSITY_HCB || bt == INTENSITY_HCB2) {
            off[2] += aac_vlc_get(b, &vlc_sf);
            int v = off[2] < -155 ? -155 : off[2] > 100 ? 100 : off[2];
            c->sf[idx] = aac_pow2q(-v);
        } else if (bt == NOISE_HCB) {
            if (noise_first) { off[1] += (int)aac_get(b, 9) - 256; noise_first = 0; }
            else off[1] += aac_vlc_get(b, &vlc_sf);
            int v = off[1] < -100 ? -100 : off[1] > 155 ? 155 : off[1];
            c->sf[idx] = aac_pow2q(v) * (1.0f / 32768.0f);
        } else {
            off[0] += aac_vlc_get(b, &vlc_sf);
            if ((unsigned)off[0] > 255) return 0;
            c->sf[idx] = aac_pow2q(off[0] - 100) * (1.0f / 32768.0f);
        }
        if (b->err) return 0;
    }
    return 1;
}

static int decode_tns(struct aac_chan *c, struct aac_bits *b) {
    struct aac_ics *ics = &c->ics;
    struct aac_tns *t = &c->tns;
    int is8 = ics->ws == EIGHT_SHORT;
    for (int w = 0; w < ics->num_windows; w++) {
        t->n_filt[w] = (int)aac_get(b, is8 ? 1 : 2);
        if (!t->n_filt[w]) continue;
        int coef_res = (int)aac_get1(b);
        for (int f = 0; f < t->n_filt[w]; f++) {
            t->length[w][f] = (int)aac_get(b, is8 ? 4 : 6);
            int order = (int)aac_get(b, is8 ? 3 : 5);
            t->order[w][f] = order;
            if (order > (is8 ? 7 : 20)) return 0;
            if (!order) continue;
            t->dir[w][f] = (int)aac_get1(b);
            int compress = (int)aac_get1(b);
            int nb = coef_res + 3 - compress;
            float tmp[20];
            double iqfac = ((1 << (coef_res + 2)) - 0.5) / (AAC_PI / 2);
            double iqfac_m = ((1 << (coef_res + 2)) + 0.5) / (AAC_PI / 2);
            for (int i = 0; i < order; i++) {
                int v = (int)aac_get(b, nb);
                if (v & (1 << (nb - 1))) v -= 1 << nb;
                tmp[i] = (float)aac_sin(v / (v >= 0 ? iqfac : iqfac_m));
            }
            /* reflection -> LPC coefficients */
            float *a = t->lpc[w][f];
            float bb[21];
            for (int m = 0; m < order; m++) {
                for (int i = 0; i < m; i++) bb[i] = a[i] + tmp[m] * a[m - 1 - i];
                for (int i = 0; i < m; i++) a[i] = bb[i];
                a[m] = tmp[m];
            }
        }
    }
    return !b->err;
}

static void apply_tns(aacdec *d, struct aac_chan *c) {
    struct aac_ics *ics = &c->ics;
    struct aac_tns *t = &c->tns;
    int is8 = ics->ws == EIGHT_SHORT;
    int mmm = is8 ? aac_tns_max_short[d->sfi] : aac_tns_max_long[d->sfi];
    if (mmm > ics->max_sfb) mmm = ics->max_sfb;
    for (int w = 0; w < ics->num_windows; w++) {
        float *coef = c->coef + w * 128;
        int bottom = ics->num_swb;
        for (int f = 0; f < t->n_filt[w]; f++) {
            int top = bottom;
            bottom = top - t->length[w][f];
            if (bottom < 0) bottom = 0;
            int order = t->order[w][f];
            if (!order) continue;
            int lo = bottom < mmm ? bottom : mmm, hi = top < mmm ? top : mmm;
            int start = ics->swb[lo], end = ics->swb[hi];
            int size = end - start;
            if (size <= 0) continue;
            const float *a = t->lpc[w][f];
            int inc = 1;
            if (t->dir[w][f]) { inc = -1; start = end - 1; }
            /* all-pole (IIR) filter, as in the standard and ffmpeg */
            for (int m = 0; m < size; m++, start += inc) {
                float acc = coef[start];
                for (int i = 1; i <= (m < order ? m : order); i++) acc -= coef[start - i * inc] * a[i - 1];
                coef[start] = acc;
            }
        }
    }
}

static int decode_spectral(aacdec *d, struct aac_chan *c, struct aac_bits *b, int pulse_n, const int *pulse_pos, const int *pulse_amp) {
    struct aac_ics *ics = &c->ics;
    float *coef = c->coef;
    const uint16_t *swb = ics->swb;
    memset(coef, 0, sizeof c->coef);
    int win = 0, idx = 0;
    for (int g = 0; g < ics->num_groups; g++) {
        for (int sfb = 0; sfb < ics->max_sfb; sfb++, idx++) {
            int cb = c->band_type[idx];
            int start = swb[sfb], end = swb[sfb + 1];
            if (cb == ZERO_HCB || cb >= INTENSITY_HCB2) continue;
            if (cb == NOISE_HCB) {
                for (int w = 0; w < ics->group_len[g]; w++) {
                    float *cf = coef + (win + w) * 128;
                    float e = 0;
                    for (int k = start; k < end; k++) {
                        d->rand_state = d->rand_state * 1664525u + 1013904223u;
                        float r = (float)(int32_t)d->rand_state;
                        cf[k] = r;
                        e += r * r;
                    }
                    float s = c->sf[idx] / aac_sqrtf(e);
                    for (int k = start; k < end; k++) cf[k] *= s;
                }
                continue;
            }
            const struct aac_vlc *v = &vlc_spec[cb - 1];
            int dim = cb_dim[cb], uns = cb_unsigned[cb], mod = cb_mod[cb], o = cb_off[cb];
            float gain = c->sf[idx];
            for (int w = 0; w < ics->group_len[g]; w++) {
                float *cf = coef + (win + w) * 128;
                for (int k = start; k < end; k += dim) {
                    int sym = aac_vlc_get(b, v);
                    int q[4];
                    if (dim == 4) {
                        q[0] = sym / 27 - o; q[1] = sym / 9 % 3 - o; q[2] = sym / 3 % 3 - o; q[3] = sym % 3 - o;
                    } else {
                        q[0] = sym / mod - o; q[1] = sym % mod - o;
                    }
                    if (uns) {
                        for (int j = 0; j < dim; j++)
                            if (q[j] && aac_get1(b)) q[j] = -q[j];
                        if (cb == ESC_HCB) {
                            for (int j = 0; j < 2; j++) {
                                int a = q[j] < 0 ? -q[j] : q[j];
                                if (a != 16) continue;
                                int n = 4;
                                while (aac_get1(b)) {
                                    n++;
                                    if (n > 12 || b->err) return 0;
                                }
                                int val = (1 << n) + (int)aac_get(b, n);
                                if (val > 8191) return 0;
                                q[j] = q[j] < 0 ? -val : val;
                            }
                        }
                    }
                    for (int j = 0; j < dim; j++) {
                        int a = q[j] < 0 ? -q[j] : q[j];
                        float x = aac_pow43[a] * gain;
                        cf[k + j] = q[j] < 0 ? -x : x;
                    }
                }
                if (b->err) return 0;
            }
        }
        win += ics->group_len[g];
    }
    /* pulses (long windows only): applied to the quantized value */
    for (int i = 0; i < pulse_n; i++) {
        int pos = pulse_pos[i];
        int band = 0;
        while (swb[band + 1] <= pos) band++;
        int cb = c->band_type[band];
        if (cb == ZERO_HCB || cb >= NOISE_HCB) continue;
        float gain = c->sf[band];
        float x = coef[pos] / gain;
        /* recover the integer: |x| = q^(4/3) */
        float ax = x < 0 ? -x : x;
        int qa = 0;
        while (qa < 8191 && aac_pow43[qa + 1] <= ax * 1.0001f + 0.5f) {
            if (aac_pow43[qa + 1] > ax * 1.0001f) break;
            qa++;
        }
        int q = x < 0 ? -qa : qa;
        q += q > 0 ? pulse_amp[i] : -pulse_amp[i];
        int a = q < 0 ? -q : q;
        if (a > 8191) a = 8191;
        coef[pos] = (q < 0 ? -aac_pow43[a] : aac_pow43[a]) * gain;
    }
    return 1;
}

int aac_decode_ics(aacdec *d, struct aac_chan *c, struct aac_bits *b, int common_window, int scale_flag) {
    struct aac_ics *ics = &c->ics;
    int global_gain = (int)aac_get(b, 8);
    if (!common_window && !scale_flag)
        if (!decode_ics_info(d, ics, b)) return 0;
    if (!decode_section(c, b)) return 0;
    if (!decode_scalefactors(d, c, b, global_gain)) return 0;
    int pulse_n = 0, pulse_pos[4], pulse_amp[4];
    c->tns.present = 0;
    if (!scale_flag) {
        if (aac_get1(b)) {   /* pulse_data_present */
            if (ics->ws == EIGHT_SHORT) return 0;
            pulse_n = (int)aac_get(b, 2) + 1;
            int sfb = (int)aac_get(b, 6);
            if (sfb > ics->num_swb) return 0;
            int pos = ics->swb[sfb];
            for (int i = 0; i < pulse_n; i++) {
                pos += (int)aac_get(b, 5);
                if (pos > 1023) return 0;
                pulse_pos[i] = pos;
                pulse_amp[i] = (int)aac_get(b, 4);
            }
        }
        if (aac_get1(b)) {   /* tns_data_present */
            if (!decode_tns(c, b)) return 0;
            c->tns.present = 1;
        }
        if (aac_get1(b)) return 0;   /* gain_control_data_present: SSR only */
    }
    if (b->err) return 0;
    return decode_spectral(d, c, b, pulse_n, pulse_pos, pulse_amp);
}

/* ---------------------------------------------------------------- stereo tools */
static void apply_ms(struct aac_chan *l, struct aac_chan *r, const uint8_t *ms) {
    struct aac_ics *ics = &l->ics;
    int idx = 0, win = 0;
    for (int g = 0; g < ics->num_groups; g++) {
        for (int sfb = 0; sfb < ics->max_sfb; sfb++, idx++) {
            if (!ms[idx] || l->band_type[idx] >= NOISE_HCB || r->band_type[idx] >= NOISE_HCB) continue;
            for (int w = 0; w < ics->group_len[g]; w++) {
                float *a = l->coef + (win + w) * 128, *b = r->coef + (win + w) * 128;
                for (int k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++) {
                    float m = a[k], s = b[k];
                    a[k] = m + s;
                    b[k] = m - s;
                }
            }
        }
        win += ics->group_len[g];
    }
}

static void apply_is(struct aac_chan *l, struct aac_chan *r, const uint8_t *ms, int ms_present) {
    struct aac_ics *ics = &r->ics;
    int idx = 0, win = 0;
    for (int g = 0; g < ics->num_groups; g++) {
        for (int sfb = 0; sfb < ics->max_sfb; sfb++, idx++) {
            int bt = r->band_type[idx];
            if (bt != INTENSITY_HCB && bt != INTENSITY_HCB2) continue;
            float s = r->sf[idx] * (bt == INTENSITY_HCB ? 1.0f : -1.0f);
            if (ms_present && ms[idx]) s = -s;
            for (int w = 0; w < ics->group_len[g]; w++) {
                float *a = l->coef + (win + w) * 128, *b = r->coef + (win + w) * 128;
                for (int k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++) b[k] = a[k] * s;
            }
        }
        win += ics->group_len[g];
    }
}

/* ---------------------------------------------------------------- AAC Main prediction */
static float flt_round(float x) {   /* round to 16 mantissa bits (as the standard specifies) */
    union { float f; uint32_t u; } v = { x };
    v.u = (v.u + 0x00008000u) & 0xffff0000u;
    return v.f;
}
static float flt_trunc(float x) {
    union { float f; uint32_t u; } v = { x };
    v.u &= 0xffff0000u;
    return v.f;
}
static float flt_even(float x) {   /* as ffmpeg (its "round to even" adds the lowest bit) */
    union { float f; uint32_t u; } v = { x };
    v.u = (v.u + 0x7fffu + (v.u & 1)) & 0xffff0000u;
    return v.f;
}

static void reset_pred(struct aac_pred *p) {
    p->r0 = p->r1 = p->cor0 = p->cor1 = 0;
    p->var0 = p->var1 = 1;
}

static void predict_one(struct aac_pred *ps, float *coef, int output_enable) {
    const float a = 0.953125f, alpha = 0.90625f;
    float r0 = ps->r0, r1 = ps->r1, cor0 = ps->cor0, cor1 = ps->cor1, var0 = ps->var0, var1 = ps->var1;
    float k1 = var0 > 1 ? cor0 * flt_even(a / var0) : 0;
    float k2 = var1 > 1 ? cor1 * flt_even(a / var1) : 0;
    float pv = flt_round(k1 * r0 + k2 * r1);
    if (output_enable) *coef += pv;
    float e0 = *coef, e1 = e0 - k1 * r0;
    ps->cor1 = flt_trunc(alpha * cor1 + r1 * e1);
    ps->var1 = flt_trunc(alpha * var1 + 0.5f * (r1 * r1 + e1 * e1));
    ps->cor0 = flt_trunc(alpha * cor0 + r0 * e0);
    ps->var0 = flt_trunc(alpha * var0 + 0.5f * (r0 * r0 + e0 * e0));
    ps->r1 = flt_trunc(a * (r0 - k1 * e0));
    ps->r0 = flt_trunc(a * e0);
}

static void apply_prediction(aacdec *d, struct aac_chan *c) {
    struct aac_ics *ics = &c->ics;
    if (!c->pred) return;
    if (ics->ws == EIGHT_SHORT) {
        for (int i = 0; i < 1024; i++) reset_pred(&c->pred[i]);
        return;
    }
    int maxb = aac_pred_sfb_max[d->sfi];
    /* the predictor's thresholds and 16-bit roundings are defined for coefficients in 16-bit PCM scale */
    for (int sfb = 0; sfb < maxb; sfb++)
        for (int k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++) {
            float x = c->coef[k] * 32768.0f;
            predict_one(&c->pred[k], &x, ics->pred_present && ics->pred_used[sfb]);
            c->coef[k] = x * (1.0f / 32768.0f);
        }
    if (ics->pred_present && ics->pred_reset_group)
        for (int i = ics->pred_reset_group - 1; i < 672; i += 30) reset_pred(&c->pred[i]);
}

/* ---------------------------------------------------------------- LTP */
static void apply_ltp(aacdec *d, struct aac_chan *c) {
    struct aac_ics *ics = &c->ics;
    if (!c->ltp_buf || !ics->ltp_present || ics->ws == EIGHT_SHORT) return;
    float x[2048], X[1024];
    int lag = ics->ltp_lag;
    /* predicted time signal: ltp_buf holds [2048 past output | 1024 current overlap-only estimate] */
    for (int i = 0; i < 2048; i++) {
        int j = i + 2048 - lag;
        x[i] = j >= 0 && j < 3072 ? c->ltp_buf[j] * ics->ltp_coef : 0;
    }
    /* window with the current window sequence / shapes */
    const float *lw = c->prev_shape ? aac_kbd_long : aac_sine_long;
    const float *rw = ics->shape ? aac_kbd_long : aac_sine_long;
    const float *lws = c->prev_shape ? aac_kbd_short : aac_sine_short;
    const float *rws = ics->shape ? aac_kbd_short : aac_sine_short;
    if (ics->ws == LONG_STOP) {
        for (int i = 0; i < 448; i++) x[i] = 0;
        for (int i = 0; i < 128; i++) x[448 + i] *= lws[i];
    } else
        for (int i = 0; i < 1024; i++) x[i] *= lw[i];
    if (ics->ws == LONG_START) {
        for (int i = 0; i < 128; i++) x[1472 + i] *= rws[127 - i];
        for (int i = 1600; i < 2048; i++) x[i] = 0;
    } else
        for (int i = 0; i < 1024; i++) x[1024 + i] *= rw[1023 - i];
    aac_mdct(x, X, 2048);
    float s = 1.0f / 1024.0f;
    for (int sfb = 0; sfb < ics->max_sfb && sfb < 40; sfb++)
        if (ics->ltp_used[sfb])
            for (int k = ics->swb[sfb]; k < ics->swb[sfb + 1]; k++) c->coef[k] += X[k] * s;
}

/* ---------------------------------------------------------------- filterbank */
static void filterbank(struct aac_chan *c, float *out) {
    struct aac_ics *ics = &c->ics;
    float buf[2048];
    const float *lw = c->prev_shape ? aac_kbd_long : aac_sine_long;
    const float *rw = ics->shape ? aac_kbd_long : aac_sine_long;
    const float *lws = c->prev_shape ? aac_kbd_short : aac_sine_short;
    const float *rws = ics->shape ? aac_kbd_short : aac_sine_short;
    float *ov = c->overlap;
    if (ics->ws != EIGHT_SHORT) {
        aac_imdct(c->coef, buf, 2048);
        if (ics->ws == LONG_STOP) {
            for (int i = 0; i < 448; i++) out[i] = ov[i];
            for (int i = 0; i < 128; i++) out[448 + i] = ov[448 + i] + buf[448 + i] * lws[i];
            for (int i = 576; i < 1024; i++) out[i] = ov[i] + buf[i];
        } else {
            for (int i = 0; i < 1024; i++) out[i] = ov[i] + buf[i] * lw[i];
        }
        if (ics->ws == LONG_START) {
            for (int i = 0; i < 448; i++) ov[i] = buf[1024 + i];
            for (int i = 0; i < 128; i++) ov[448 + i] = buf[1472 + i] * rws[127 - i];
            for (int i = 576; i < 1024; i++) ov[i] = 0;
        } else {
            for (int i = 0; i < 1024; i++) ov[i] = buf[1024 + i] * rw[1023 - i];
        }
    } else {
        float t[2048], s[256];
        memset(t, 0, sizeof t);
        for (int w = 0; w < 8; w++) {
            aac_imdct(c->coef + w * 128, s, 256);
            const float *l = w == 0 ? lws : rws;
            float *p = t + 448 + 128 * w;
            for (int i = 0; i < 128; i++) p[i] += s[i] * l[i];
            for (int i = 0; i < 128; i++) p[128 + i] += s[128 + i] * rws[127 - i];
        }
        for (int i = 0; i < 1024; i++) out[i] = ov[i] + t[i];
        memcpy(ov, t + 1024, sizeof(float) * 1024);
    }
    c->prev_shape = ics->shape;
}

static void finish_channel(aacdec *d, struct aac_chan *c) {
    if (c->pred && c->ics.ws != EIGHT_SHORT) {
        /* prediction is applied in the element decoder */
    }
    apply_ltp(d, c);
    if (c->tns.present) apply_tns(d, c);
    filterbank(c, c->out);
    if (c->ltp_buf) {
        /* LTP history: last 2048 output samples + the current overlap (the next frame's first half estimate) */
        memmove(c->ltp_buf, c->ltp_buf + 1024, sizeof(float) * 1024);
        memcpy(c->ltp_buf + 1024, c->out, sizeof(float) * 1024);
        memcpy(c->ltp_buf + 2048, c->overlap, sizeof(float) * 1024);
    }
}

/* ---------------------------------------------------------------- elements */
static int decode_sce(aacdec *d, struct aac_elem *e, struct aac_bits *b) {
    struct aac_chan *c = d->ch[e->ch[0]];
    if (!aac_decode_ics(d, c, b, 0, 0)) return 0;
    if (d->aot == AOT_MAIN) apply_prediction(d, c);
    return 1;
}

static int decode_cpe(aacdec *d, struct aac_elem *e, struct aac_bits *b) {
    struct aac_chan *l = d->ch[e->ch[0]], *r = d->ch[e->ch[1]];
    uint8_t ms[128];
    int ms_present = 0;
    int common = (int)aac_get1(b);
    if (common) {
        if (!decode_ics_info(d, &l->ics, b)) return 0;
        r->ics = l->ics;
        if (d->aot == AOT_LTP) {   /* the second channel's ltp data follows the first one's */
            r->ics.ltp_present = 0;
            if (l->ics.pdp && (r->ics.ltp_present = (int)aac_get1(b))) decode_ltp_data(&r->ics, b);
        }
        ms_present = (int)aac_get(b, 2);
        if (ms_present == 3) return 0;
        int n = l->ics.num_groups * l->ics.max_sfb;
        for (int i = 0; i < n; i++) ms[i] = ms_present == 2 ? 1 : ms_present == 1 ? (uint8_t)aac_get1(b) : 0;
    }
    if (!aac_decode_ics(d, l, b, common, 0)) return 0;
    if (!aac_decode_ics(d, r, b, common, 0)) return 0;
    if (common) {
        if (ms_present) apply_ms(l, r, ms);
        if (d->aot == AOT_MAIN) { apply_prediction(d, l); apply_prediction(d, r); }
        apply_is(l, r, ms, ms_present);
    } else if (d->aot == AOT_MAIN) {
        apply_prediction(d, l);
        apply_prediction(d, r);
    }
    return 1;
}

static int skip_cce(aacdec *d, struct aac_bits *b) {
    if (!d->scratch) {
        d->scratch = calloc(1, sizeof *d->scratch);
        if (!d->scratch) return 0;
    }
    aac_get(b, 4);
    int ind_sw = (int)aac_get1(b);
    int ncoupled = (int)aac_get(b, 3);
    int ngain = 0;
    for (int c = 0; c <= ncoupled; c++) {
        ngain++;
        int is_cpe = (int)aac_get1(b);
        aac_get(b, 4);
        if (is_cpe && aac_get(b, 2) == 3) ngain++;
    }
    aac_get(b, 4);   /* domain, sign, scale */
    struct aac_chan *c = d->scratch;
    if (!aac_decode_ics(d, c, b, 0, 0)) return 0;
    for (int i = 1; i < ngain; i++) {
        int cge = ind_sw ? 1 : (int)aac_get1(b);
        if (cge) aac_vlc_get(b, &vlc_sf);
        else {
            int n = c->ics.num_groups * c->ics.max_sfb;
            for (int k = 0; k < n; k++)
                if (c->band_type[k] != ZERO_HCB) aac_vlc_get(b, &vlc_sf);
        }
    }
    return !b->err;
}

static int decode_fil(aacdec *d, struct aac_bits *b, struct aac_elem *prev) {
    int cnt = (int)aac_get(b, 4);
    if (cnt == 15) cnt += (int)aac_get(b, 8) - 1;
    if (cnt <= 0) return 1;
    int end = b->pos + 8 * cnt;
    if (end > b->end) return 0;
    int type = (int)aac_get(b, 4);
    if ((type == 13 || type == 14) && prev && (d->sbr == 1 || (d->sbr == -1 && d->frames == 0))) {
        /* implicit signalling (as ffmpeg): SBR found in the first frame turns it on; a mono stream that may
           carry PS is then decoded as stereo */
        if (d->sbr == -1) {
            d->sbr = 1;
            if (d->ps == -1 && d->nch == 1) d->ps = 1;
            set_output(d);
        }
        if (d->ps == -1 && d->nch == 1 && d->frames == 0) { d->ps = 1; set_output(d); }
        if (!prev->sbr) prev->sbr = aac_sbr_new(d->core_rate);
        if (prev->sbr) {
            int ps_seen = 0;
            struct aac_bits sb = *b;
            sb.end = end;
            aac_sbr_parse(prev->sbr, &sb, end - b->pos, prev->type, type == 14, d->ps == 1, &ps_seen);
            prev->sbr_seen = 1;
        }
    }
    b->pos = end;
    return 1;
}

static int decode_block(aacdec *d, struct aac_bits *b) {
    int count[8] = { 0 };
    struct aac_elem *prev = 0;
    for (int i = 0; i < d->nelem; i++) d->elem[i].sbr_seen = d->elem[i].decoded = 0;
    int n = 0;
    d->dbg_n = 0;
    for (;;) {
        if (b->err || aac_left(b) < 3) return 0;
        int type = (int)aac_get(b, 3);
        if (type == EL_END) {
            if (d->dbg_n < 64) { d->dbg_type[d->dbg_n] = EL_END; d->dbg_end[d->dbg_n++] = b->pos - 3; }
            break;
        }
        if (++n > 64) return 0;
        switch (type) {
        case EL_SCE: case EL_CPE: case EL_LFE: {
            aac_get(b, 4);
            struct aac_elem *e = get_elem(d, type, count[type]++);
            if (!e) return 0;
            if (e->decoded) return 0;   /* the same element twice in one frame */
            if (!(type == EL_CPE ? decode_cpe(d, e, b) : decode_sce(d, e, b))) return 0;
            e->decoded = 1;
            prev = e;
            break;
        }
        case EL_CCE:
            if (!skip_cce(d, b)) return 0;
            break;
        case EL_DSE: {
            aac_get(b, 4);
            int align = (int)aac_get1(b);
            int cnt = (int)aac_get(b, 8);
            if (cnt == 255) cnt += (int)aac_get(b, 8);
            if (align) aac_align(b, 0);
            aac_skip(b, 8 * cnt);
            break;
        }
        case EL_PCE: {
            struct aac_pce p;
            int sfi;
            memset(&p, 0, sizeof p);
            parse_pce(b, &p, &sfi);
            aac_align(b, 0);
            aac_skip(b, 8 * (int)aac_get(b, 8));
            /* in-band PCE (ADTS with channel_configuration 0): it defines the channel layout */
            int nc = pce_channels(&p);
            if (!b->err && d->chan_cfg == 0 && nc > 0 && nc <= AAC_MAX_CH &&
                (!d->have_pce || memcmp(&p, &d->pce, sizeof p))) {
                d->pce = p;
                d->have_pce = 1;
                set_output(d);
                int cnt[8] = { 0 };
                for (int i = 0; i < d->nelem; i++) {
                    int r[2];
                    struct aac_elem *e = &d->elem[i];
                    elem_roles(d, e->type, cnt[e->type]++, r);
                    for (int c = 0; c < e->nch; c++) d->ch[e->ch[c]]->role = r[c];
                }
            }
            break;
        }
        case EL_FIL:
            if (!decode_fil(d, b, prev)) return 0;
            prev = 0;
            break;
        }
        if (b->err) return 0;
        if (d->dbg_n < 64) { d->dbg_type[d->dbg_n] = type; d->dbg_end[d->dbg_n++] = b->pos; }
    }
    return !b->err;
}

/* ---------------------------------------------------------------- output */
static void downmix_weights(aacdec *d, float *wl, float *wr) {
    /* left/right weights per decoded channel: ITU-R BS.775 coefficients as ffmpeg / mpv use them
       (centre and surrounds at -3 dB, LFE dropped, no normalisation; int16 output saturates) */
    for (int i = 0; i < d->nch; i++) {
        float l = 0, r = 0;
        switch (d->ch[i]->role) {
        case R_FL: case R_FLC: l = 1; break;
        case R_FR: case R_FRC: r = 1; break;
        case R_FC: l = r = 0.70710678f; break;
        case R_SL: case R_BL: l = 0.70710678f; break;
        case R_SR: case R_BR: r = 0.70710678f; break;
        case R_BC: l = r = 0.5f; break;
        default: break;
        }
        wl[i] = l;
        wr[i] = r;
    }
}

static int decode_frame(aacdec *d, const uint8_t *data, int len, float *out) {
    if (!d || !data || len <= 0) return -1;
    int nblocks = 1, hlen = 0, flen = len;
    if (d->adts || !d->configured || (len >= 7 && data[0] == 0xff && (data[1] & 0xf6) == 0xf0 && !d->configured)) {
        hlen = parse_adts(d, data, len, &flen, &nblocks);
        if (!hlen) {
            if (!d->configured) return -1;
            /* not an ADTS header: damaged frame */
            goto silence;
        }
        if (flen > len) flen = len;
    }
    if (!d->configured) return -1;
    if (len > (1 << 20)) return -1;
    if (d->bufcap < flen + 16) {
        uint8_t *nb = realloc(d->buf, (size_t)flen + 16);
        if (!nb) return -1;
        d->buf = nb;
        d->bufcap = flen + 16;
    }
    memcpy(d->buf, data, (size_t)flen);
    memset(d->buf + flen, 0, 16);
    struct aac_bits b = { d->buf, hlen * 8, flen * 8, 0 };
    (void)nblocks;
    int ok = decode_block(d, &b);
    if (!ok) goto silence;
    for (int i = 0; i < d->nelem; i++) {
        struct aac_elem *e = &d->elem[i];
        if (!e->decoded) {
            /* element missing from this frame: silence */
            for (int c = 0; c < e->nch; c++) memset(d->ch[e->ch[c]]->out, 0, sizeof(float) * 2048);
            continue;
        }
        for (int c = 0; c < e->nch; c++) finish_channel(d, d->ch[e->ch[c]]);
        if (d->sbr == 1) {
            /* every element goes through the QMF banks (LFE and elements without SBR data: plain upsampling) */
            if (!e->sbr) e->sbr = aac_sbr_new(d->core_rate);
            int ps_out = d->ps == 1 && e->type == EL_SCE && d->nch == 1;
            if (ps_out && !d->scratch) d->scratch = calloc(1, sizeof *d->scratch);
            float *r = e->nch == 2 ? d->ch[e->ch[1]]->out : ps_out && d->scratch ? d->scratch->out : 0;
            if (e->sbr) aac_sbr_apply(e->sbr, e->type, d->ch[e->ch[0]]->out, r, ps_out && r);
            else for (int c = 0; c < e->nch; c++) memset(d->ch[e->ch[c]]->out, 0, sizeof(float) * 2048);
        }
    }
    d->frames++;
    int n = d->sbr == 1 ? 2048 : 1024;
    if (d->all_channels) {
        for (int c = 0; c < d->nch; c++)
            for (int k = 0; k < n; k++) out[k * d->nch + c] = d->ch[c]->out[k];
        return n;
    }
    if (d->out_ch == 1) {
        int src = 0;
        for (int c = 0; c < d->nch; c++)
            if (d->ch[c]->role == R_FC) { src = c; break; }
        if (d->nch) memcpy(out, d->ch[src]->out, sizeof(float) * (size_t)n);
        else memset(out, 0, sizeof(float) * (size_t)n);
        return n;
    }
    if (d->nch == 1) {
        const float *s = d->ch[0]->out;
        const float *r = d->ps == 1 && d->scratch ? d->scratch->out : s;
        for (int k = 0; k < n; k++) { out[2 * k] = s[k]; out[2 * k + 1] = r[k]; }
        return n;
    }
    if (d->nch == 2 && d->ch[0]->role == R_FL && d->ch[1]->role == R_FR) {
        const float *l = d->ch[0]->out, *r = d->ch[1]->out;
        for (int k = 0; k < n; k++) { out[2 * k] = l[k]; out[2 * k + 1] = r[k]; }
        return n;
    }
    float wl[AAC_MAX_CH], wr[AAC_MAX_CH];
    downmix_weights(d, wl, wr);
    for (int k = 0; k < n; k++) {
        float l = 0, r = 0;
        for (int c = 0; c < d->nch; c++) {
            float v = d->ch[c]->out[k];
            l += v * wl[c];
            r += v * wr[c];
        }
        out[2 * k] = l;
        out[2 * k + 1] = r;
    }
    return n;
silence: {
        int n = d->sbr == 1 ? 2048 : 1024;
        int nc = d->all_channels ? d->nch : d->out_ch;
        if (nc < 1) nc = 1;
        memset(out, 0, sizeof(float) * (size_t)(n * nc));
        /* keep the overlap consistent: the next frame fades in from silence */
        for (int c = 0; c < d->nch; c++) memset(d->ch[c]->overlap, 0, sizeof(float) * 1024);
        d->frames++;
        return n;
    }
}

/* ---------------------------------------------------------------- API */
aacdec *aac_open(const uint8_t *config, int len) {
    if (!init_tables()) return 0;
    aacdec *d = calloc(1, sizeof *d);
    if (!d) return 0;
    d->rand_state = 0x1f2e3d4c;
    if (config && len > 0) {
        if (!parse_asc(d, config, len)) { free(d); return 0; }
    } else {
        d->adts = 1;
        d->out_ch = 2;
        d->out_rate = 44100;
    }
    return d;
}

int aac_decode_float(aacdec *d, const uint8_t *data, int len, float *pcm) {
    return decode_frame(d, data, len, pcm);
}

int aac_decode(aacdec *d, const uint8_t *data, int len, int16_t *pcm) {
    if (!d) return -1;
    if (!d->fbuf) {
        d->fbuf = malloc(sizeof(float) * AAC_MAX_SAMPLES * AAC_MAX_CH);
        if (!d->fbuf) return -1;
    }
    float *tmp = d->fbuf;
    int n = decode_frame(d, data, len, tmp);
    if (n <= 0) return n;
    int nc = d->all_channels ? d->nch : d->out_ch;
    for (int i = 0; i < n * nc; i++) {
        float v = tmp[i] * 32768.0f;
        int s = (int)(v < 0 ? v - 0.5f : v + 0.5f);
        pcm[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    return n;
}

int aac_sample_rate(aacdec *d) { return d ? d->out_rate : 0; }
int aac_channels(aacdec *d) { return d ? d->out_ch : 0; }

void aac_info(aacdec *d, int *object_type, int *sbr, int *ps) {
    if (object_type) *object_type = d ? d->aot : 0;
    if (sbr) *sbr = d && d->sbr == 1;
    if (ps) *ps = d && d->ps == 1;
}

void aac_reset(aacdec *d) {
    if (!d) return;
    for (int c = 0; c < d->nch; c++) {
        struct aac_chan *ch = d->ch[c];
        memset(ch->overlap, 0, sizeof ch->overlap);
        ch->prev_shape = 0;
        if (ch->ltp_buf) memset(ch->ltp_buf, 0, 3072 * sizeof(float));
        if (ch->pred)
            for (int i = 0; i < 1024; i++) reset_pred(&ch->pred[i]);
    }
    for (int i = 0; i < d->nelem; i++)
        if (d->elem[i].sbr) aac_sbr_reset(d->elem[i].sbr);
}

void aac_close(aacdec *d) {
    if (!d) return;
    for (int c = 0; c < d->nch; c++) {
        free(d->ch[c]->ltp_buf);
        free(d->ch[c]->pred);
        free(d->ch[c]);
    }
    for (int i = 0; i < d->nelem; i++) aac_sbr_free(d->elem[i].sbr);
    free(d->scratch);
    free(d->fbuf);
    free(d->buf);
    free(d);
}
