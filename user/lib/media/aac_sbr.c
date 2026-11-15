/* AXIOM AAC decoder: Spectral Band Replication (HE-AAC, ISO/IEC 14496-3 4.6.18).
 * Bitstream parsing, frequency band tables, 32-band QMF analysis, HF generation (LPC inverse filtering +
 * patching), HF adjustment (envelope / noise floor / sinusoids, limiter, smoothing) and 64-band QMF
 * synthesis. Behaviour on corner cases follows ffmpeg's decoder so the outputs can be compared. */
#include "aac_int.h"

#define ENV_ADJ_OFF 2      /* t_HFAdj */
#define NOISE_FLOOR_OFFSET 6
enum { FIXFIX, FIXVAR, VARFIX, VARVAR };
enum { T_ENV_15, F_ENV_15, T_BAL_15, F_BAL_15, T_ENV_30, F_ENV_30, T_BAL_30, F_BAL_30, T_NOISE_30, T_NOISE_BAL_30 };
static const int8_t sbr_lav[10] = { 60, 60, 24, 24, 31, 31, 12, 12, 31, 12 };

extern const struct aac_hsrc aac_sbr_huff_src[10];
extern const int8_t aac_sbr_offset[6][16];
extern const float aac_sbr_qmf_window[640];
extern const float aac_sbr_noise[512][2];

static struct aac_vlc sbr_vlc[10];
static float ana_cos[32][64], ana_sin[32][64];     /* 2 cos / 2 sin (pi (k + 0.5)(2n - 0.5) / 64) */
static float syn_cos[128][64], syn_sin[128][64];   /* cos / sin (pi (k + 0.5)(2n - 255) / 128) / 64 */
static int sbr_ready;

static int sbr_init_tables(void) {
    if (sbr_ready) return 1;
    for (int i = 0; i < 10; i++)
        if (!aac_vlc_build(&sbr_vlc[i], &aac_sbr_huff_src[i], 9, 0)) return 0;
    for (int k = 0; k < 32; k++)
        for (int n = 0; n < 64; n++) {
            double a = AAC_PI * (k + 0.5) * (2 * n - 0.5) / 64;
            ana_cos[k][n] = (float)(2 * aac_cos(a));
            ana_sin[k][n] = (float)(2 * aac_sin(a));
        }
    for (int n = 0; n < 128; n++)
        for (int k = 0; k < 64; k++) {
            double a = AAC_PI * (k + 0.5) * (2 * n - 255) / 128;
            syn_cos[n][k] = (float)(aac_cos(a) / 64);
            syn_sin[n][k] = (float)(aac_sin(a) / 64);
        }
    sbr_ready = 1;
    return 1;
}

struct sbr_ch {
    int frame_class, add_harmonic_flag, num_env, num_noise, amp_res;
    uint8_t freq_res[7], df_env[5], df_noise[2];
    uint8_t invf[2][5];
    uint8_t add_harmonic[48];
    float ana[288 + 1024];          /* analysis input history (time ascending) */
    float v[1280 + 128 * 32];       /* synthesis V buffer, newest first from v_off */
    int v_off;
    int e_a[2];
    float bw[5];
    float W[2][32][32][2];
    int ypos;
    float Y[2][38][64][2];
    float g_temp[42][48], q_temp[42][48];
    uint8_t s_indexmapped[8][48];
    uint8_t env_q[6][48];
    float env[6][48];
    uint8_t noise_q[3][5];
    float noise[3][5];
    uint8_t t_env[8], t_env_num_env_old, t_q[3];
    unsigned f_indexnoise, f_indexsine;
};

struct sbr_spec { int8_t start, stop, xover, freq_scale, alter_scale, noise_bands; };

struct aac_sbr {
    int sample_rate, start, ready, id_aac, reset;
    struct sbr_spec spec;
    int amp_res_header, limiter_bands, limiter_gains, interpol_freq, smoothing_mode, coupling;
    int k[3], kx[2], m[2], pushed, n_master, n[2], n_q, n_lim;
    uint16_t f_master[49], f_low[25], f_high[49], f_noise[6], f_lim[30];
    int num_patches;
    uint8_t patch_num[6], patch_start[6];
    float X_low[32][40][2];
    float X_high[64][40][2];
    float X[2][2][38][64];
    float alpha0[64][2], alpha1[64][2];
    float e_origmapped[7][48], q_mapped[7][48], e_curr[7][48], q_m[7][48], s_m[7][48], gain[7][48];
    uint8_t s_mapped[7][48];
    struct sbr_ch ch[2];
    struct aac_ps *ps;
};

/* ---------------------------------------------------------------- helpers */
static float exp2fi(int x) {
    if (x > 127) return 1e38f * 10;   /* inf */
    if (x < -149) return 0;
    union { uint32_t u; float f; } v;
    if (x >= -126) v.u = (uint32_t)(x + 127) << 23;
    else v.u = 1u << (x + 149);
    return v.f;
}

static int lrintf_(double x) {   /* round half to even, as lrintf */
    double f = (double)(long)x;
    if (x < 0 && f != x) f -= 1;   /* floor */
    double d = x - f;
    long r = (long)f;
    if (d > 0.5 || (d == 0.5 && (r & 1))) r++;
    return (int)r;
}

static float log2f_(float x) { return (float)(aac_log(x) / 0.69314718055994530942); }

static void sort16(int16_t *a, int n) {
    for (int i = 1; i < n; i++) {
        int16_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static void sortu16(uint16_t *a, int n) {
    for (int i = 1; i < n; i++) {
        uint16_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

static void make_bands(int16_t *bands, int start, int stop, int num) {
    float base = (float)aac_pow((float)stop / start, 1.0f / num);
    float prod = (float)start;
    int prev = start;
    for (int k = 0; k < num - 1; k++) {
        prod *= base;
        int pres = lrintf_(prod);
        bands[k] = (int16_t)(pres - prev);
        prev = pres;
    }
    bands[num - 1] = (int16_t)(stop - prev);
}

static void turnoff(struct aac_sbr *s) {
    s->start = 0;
    s->ready = 0;
    s->kx[1] = 32;
    s->m[1] = 0;
    s->ch[0].e_a[1] = s->ch[1].e_a[1] = -1;
    memset(&s->spec, -1, sizeof s->spec);
}

/* ---------------------------------------------------------------- frequency tables */
static void make_f_lim(struct aac_sbr *s) {
    if (s->limiter_bands > 0) {
        static const float warped[3] = { 1.32715174233856803909f, 1.18509277094158210129f, 1.11987160404675912501f };
        float lbpo = warped[s->limiter_bands - 1];
        int16_t pb[7];
        pb[0] = (int16_t)s->kx[1];
        for (int k = 1; k <= s->num_patches; k++) pb[k] = (int16_t)(pb[k - 1] + s->patch_num[k - 1]);
        memcpy(s->f_lim, s->f_low, (size_t)(s->n[0] + 1) * sizeof(uint16_t));
        if (s->num_patches > 1)
            for (int k = 1; k < s->num_patches; k++) s->f_lim[s->n[0] + k] = (uint16_t)pb[k];
        sortu16(s->f_lim, s->num_patches + s->n[0]);
        s->n_lim = s->n[0] + s->num_patches - 1;
        uint16_t *in = s->f_lim + 1, *out = s->f_lim;
        while (out < s->f_lim + s->n_lim) {
            int in_pb_in = 0, in_pb_out = 0;
            for (int i = 0; i <= s->num_patches; i++) {
                if (pb[i] == *in) in_pb_in = 1;
                if (pb[i] == *out) in_pb_out = 1;
            }
            if (*in >= *out * lbpo) *++out = *in++;
            else if (*in == *out || !in_pb_in) { in++; s->n_lim--; }
            else if (!in_pb_out) { *out = *in++; s->n_lim--; }
            else *++out = *in++;
        }
    } else {
        s->f_lim[0] = s->f_low[0];
        s->f_lim[1] = s->f_low[s->n[0]];
        s->n_lim = 1;
    }
}

static int make_f_master(struct aac_sbr *s) {
    const int8_t *off;
    switch (s->sample_rate) {
    case 16000: off = aac_sbr_offset[0]; break;
    case 22050: off = aac_sbr_offset[1]; break;
    case 24000: off = aac_sbr_offset[2]; break;
    case 32000: off = aac_sbr_offset[3]; break;
    case 44100: case 48000: case 64000: off = aac_sbr_offset[4]; break;
    case 88200: case 96000: case 128000: case 176400: case 192000: off = aac_sbr_offset[5]; break;
    default: return -1;
    }
    unsigned temp = s->sample_rate < 32000 ? 3000 : s->sample_rate < 64000 ? 4000 : 5000;
    unsigned start_min = ((temp << 7) + (unsigned)(s->sample_rate >> 1)) / (unsigned)s->sample_rate;
    unsigned stop_min = ((temp << 8) + (unsigned)(s->sample_rate >> 1)) / (unsigned)s->sample_rate;
    s->k[0] = (int)start_min + off[s->spec.start];
    if (s->spec.stop < 14) {
        int16_t dk[13];
        s->k[2] = (int)stop_min;
        make_bands(dk, (int)stop_min, 64, 13);
        sort16(dk, 13);
        for (int k = 0; k < s->spec.stop; k++) s->k[2] += dk[k];
    } else if (s->spec.stop == 14) s->k[2] = 2 * s->k[0];
    else if (s->spec.stop == 15) s->k[2] = 3 * s->k[0];
    else return -1;
    if (s->k[2] > 64) s->k[2] = 64;
    int maxq = s->sample_rate <= 32000 ? 48 : s->sample_rate == 44100 ? 35 : 32;
    if (s->sample_rate > 32000 && s->sample_rate < 44100) return -1;
    if (s->k[2] - s->k[0] > maxq) return -1;
    if (s->k[0] <= 0) return -1;
    if (!s->spec.freq_scale) {
        int dk = s->spec.alter_scale + 1;
        s->n_master = ((s->k[2] - s->k[0] + (dk & 2)) >> dk) << 1;
        if (s->n_master <= 0 || s->spec.xover >= s->n_master || s->n_master > 48) return -1;
        for (int k = 1; k <= s->n_master; k++) s->f_master[k] = (uint16_t)dk;
        int k2diff = s->k[2] - s->k[0] - s->n_master * dk;
        if (k2diff < 0) {
            s->f_master[1]--;
            s->f_master[2] -= (k2diff < -1);
        } else if (k2diff) s->f_master[s->n_master]++;
        s->f_master[0] = (uint16_t)s->k[0];
        for (int k = 1; k <= s->n_master; k++) s->f_master[k] += s->f_master[k - 1];
    } else {
        int half = 7 - s->spec.freq_scale;
        int two = 49 * s->k[2] > 110 * s->k[0];
        s->k[1] = two ? 2 * s->k[0] : s->k[2];
        int nb0 = lrintf_(half * log2f_(s->k[1] / (float)s->k[0])) * 2;
        if (nb0 <= 0 || nb0 > 48) return -1;
        int16_t vk0[49];
        vk0[0] = 0;
        make_bands(vk0 + 1, s->k[0], s->k[1], nb0);
        sort16(vk0 + 1, nb0);
        int vdk0_max = vk0[nb0];
        vk0[0] = (int16_t)s->k[0];
        for (int k = 1; k <= nb0; k++) {
            if (vk0[k] <= 0) return -1;
            vk0[k] += vk0[k - 1];
        }
        if (two) {
            int16_t vk1[49];
            float invwarp = s->spec.alter_scale ? 0.76923076923076923077f : 1.0f;
            int nb1 = lrintf_(half * invwarp * log2f_(s->k[2] / (float)s->k[1])) * 2;
            if (nb1 <= 0 || nb0 + nb1 > 48) return -1;
            make_bands(vk1 + 1, s->k[1], s->k[2], nb1);
            int vdk1_min = vk1[1];
            for (int i = 2; i <= nb1; i++) if (vk1[i] < vdk1_min) vdk1_min = vk1[i];
            if (vdk1_min < vdk0_max) {
                sort16(vk1 + 1, nb1);
                int change = vdk0_max - vk1[1];
                int lim = (vk1[nb1] - vk1[1]) >> 1;
                if (lim < change) change = lim;
                vk1[1] += (int16_t)change;
                vk1[nb1] -= (int16_t)change;
            }
            sort16(vk1 + 1, nb1);
            vk1[0] = (int16_t)s->k[1];
            for (int k = 1; k <= nb1; k++) {
                if (vk1[k] <= 0) return -1;
                vk1[k] += vk1[k - 1];
            }
            s->n_master = nb0 + nb1;
            if (s->spec.xover >= s->n_master) return -1;
            for (int k = 0; k <= nb0; k++) s->f_master[k] = (uint16_t)vk0[k];
            for (int k = 1; k <= nb1; k++) s->f_master[nb0 + k] = (uint16_t)vk1[k];
        } else {
            s->n_master = nb0;
            if (s->spec.xover >= s->n_master) return -1;
            for (int k = 0; k <= nb0; k++) s->f_master[k] = (uint16_t)vk0[k];
        }
    }
    return 0;
}

static int calc_patches(struct aac_sbr *s) {
    int i, k, last_k = -1, last_msb = -1, sb = 0;
    int msb = s->k[0], usb = s->kx[1];
    int goal_sb = ((1000 << 11) + (s->sample_rate >> 1)) / s->sample_rate;
    s->num_patches = 0;
    if (goal_sb < s->kx[1] + s->m[1]) {
        for (k = 0; k < s->n_master && s->f_master[k] < goal_sb; k++) ;
    } else k = s->n_master;
    do {
        int odd = 0;
        if (k == last_k && msb == last_msb) return -1;
        last_k = k;
        last_msb = msb;
        for (i = k; i == k || sb > (s->k[0] - 1 + msb - odd); i--) {
            if (i < 0) return -1;
            sb = s->f_master[i];
            odd = (sb + s->k[0]) & 1;
        }
        if (s->num_patches > 5) return -1;
        int pn = sb - usb > 0 ? sb - usb : 0;
        s->patch_num[s->num_patches] = (uint8_t)pn;
        s->patch_start[s->num_patches] = (uint8_t)(s->k[0] - odd - pn);
        if (pn > 0) {
            usb = sb;
            msb = sb;
            s->num_patches++;
        } else msb = s->kx[1];
        if (s->f_master[k] - sb < 3) k = s->n_master;
    } while (sb != s->kx[1] + s->m[1]);
    if (s->num_patches > 1 && s->patch_num[s->num_patches - 1] < 3) s->num_patches--;
    for (i = 0; i < s->num_patches; i++)
        if (s->patch_start[i] + s->patch_num[i] > 32 || (int8_t)s->patch_start[i] < 0) return -1;
    return 0;
}

static int make_f_derived(struct aac_sbr *s) {
    s->n[1] = s->n_master - s->spec.xover;
    s->n[0] = (s->n[1] + 1) >> 1;
    memcpy(s->f_high, &s->f_master[s->spec.xover], (size_t)(s->n[1] + 1) * sizeof(uint16_t));
    s->m[1] = s->f_high[s->n[1]] - s->f_high[0];
    s->kx[1] = s->f_high[0];
    if (s->kx[1] + s->m[1] > 64 || s->kx[1] > 32) return -1;
    s->f_low[0] = s->f_high[0];
    int t = s->n[1] & 1;
    for (int k = 1; k <= s->n[0]; k++) s->f_low[k] = s->f_high[2 * k - t];
    int nq = lrintf_(s->spec.noise_bands * log2f_(s->k[2] / (float)s->kx[1]));
    s->n_q = nq < 1 ? 1 : nq;
    if (s->n_q > 5) return -1;
    s->f_noise[0] = s->f_low[0];
    t = 0;
    for (int k = 1; k <= s->n_q; k++) {
        t += (s->n[0] - t) / (s->n_q + 1 - k);
        s->f_noise[k] = s->f_low[t];
    }
    if (calc_patches(s) < 0) return -1;
    make_f_lim(s);
    s->ch[0].f_indexnoise = s->ch[1].f_indexnoise = 0;
    return 0;
}

/* ---------------------------------------------------------------- bitstream */
static void read_header(struct aac_sbr *s, struct aac_bits *b) {
    int old_lim = s->limiter_bands;
    struct sbr_spec old = s->spec;
    s->start = 1;
    s->ready = 0;
    s->amp_res_header = (int)aac_get1(b);
    s->spec.start = (int8_t)aac_get(b, 4);
    s->spec.stop = (int8_t)aac_get(b, 4);
    s->spec.xover = (int8_t)aac_get(b, 3);
    aac_get(b, 2);
    int extra1 = (int)aac_get1(b), extra2 = (int)aac_get1(b);
    if (extra1) {
        s->spec.freq_scale = (int8_t)aac_get(b, 2);
        s->spec.alter_scale = (int8_t)aac_get1(b);
        s->spec.noise_bands = (int8_t)aac_get(b, 2);
    } else {
        s->spec.freq_scale = 2;
        s->spec.alter_scale = 1;
        s->spec.noise_bands = 2;
    }
    if (memcmp(&old, &s->spec, sizeof old)) s->reset = 1;
    if (extra2) {
        s->limiter_bands = (int)aac_get(b, 2);
        s->limiter_gains = (int)aac_get(b, 2);
        s->interpol_freq = (int)aac_get1(b);
        s->smoothing_mode = (int)aac_get1(b);
    } else {
        s->limiter_bands = 2;
        s->limiter_gains = 2;
        s->interpol_freq = 1;
        s->smoothing_mode = 1;
    }
    if (s->limiter_bands != old_lim && !s->reset) make_f_lim(s);
}

static int read_grid(struct aac_sbr *s, struct aac_bits *b, struct sbr_ch *c) {
    static const int8_t ceil_log2[6] = { 0, 1, 2, 2, 3, 3 };
    int bs_pointer = 0, abs_bord_trail = 16, num_rel_lead, num_rel_trail;
    int num_env_old = c->num_env;
    c->freq_res[0] = c->freq_res[c->num_env];
    c->amp_res = s->amp_res_header;
    c->t_env_num_env_old = c->t_env[num_env_old];
    int cls = (int)aac_get(b, 2);
    switch (cls) {
    case FIXFIX: {
        int ne = 1 << aac_get(b, 2);
        if (ne > 4) return -1;
        c->num_env = ne;
        num_rel_lead = ne - 1;
        if (ne == 1) c->amp_res = 0;
        c->t_env[0] = 0;
        c->t_env[ne] = (uint8_t)abs_bord_trail;
        int step = (abs_bord_trail + (ne >> 1)) / ne;
        for (int i = 0; i < num_rel_lead; i++) c->t_env[i + 1] = (uint8_t)(c->t_env[i] + step);
        c->freq_res[1] = (uint8_t)aac_get1(b);
        for (int i = 1; i < ne; i++) c->freq_res[i + 1] = c->freq_res[1];
        break;
    }
    case FIXVAR:
        abs_bord_trail += (int)aac_get(b, 2);
        num_rel_trail = (int)aac_get(b, 2);
        c->num_env = num_rel_trail + 1;
        c->t_env[0] = 0;
        c->t_env[c->num_env] = (uint8_t)abs_bord_trail;
        for (int i = 0; i < num_rel_trail; i++)
            c->t_env[c->num_env - 1 - i] = (uint8_t)(c->t_env[c->num_env - i] - 2 * (int)aac_get(b, 2) - 2);
        bs_pointer = (int)aac_get(b, ceil_log2[c->num_env]);
        for (int i = 0; i < c->num_env; i++) c->freq_res[c->num_env - i] = (uint8_t)aac_get1(b);
        break;
    case VARFIX:
        c->t_env[0] = (uint8_t)aac_get(b, 2);
        num_rel_lead = (int)aac_get(b, 2);
        c->num_env = num_rel_lead + 1;
        c->t_env[c->num_env] = (uint8_t)abs_bord_trail;
        for (int i = 0; i < num_rel_lead; i++) c->t_env[i + 1] = (uint8_t)(c->t_env[i] + 2 * (int)aac_get(b, 2) + 2);
        bs_pointer = (int)aac_get(b, ceil_log2[c->num_env]);
        for (int i = 0; i < c->num_env; i++) c->freq_res[i + 1] = (uint8_t)aac_get1(b);
        break;
    default: {   /* VARVAR */
        c->t_env[0] = (uint8_t)aac_get(b, 2);
        abs_bord_trail += (int)aac_get(b, 2);
        num_rel_lead = (int)aac_get(b, 2);
        num_rel_trail = (int)aac_get(b, 2);
        int ne = num_rel_lead + num_rel_trail + 1;
        if (ne > 5) return -1;
        c->num_env = ne;
        c->t_env[ne] = (uint8_t)abs_bord_trail;
        for (int i = 0; i < num_rel_lead; i++) c->t_env[i + 1] = (uint8_t)(c->t_env[i] + 2 * (int)aac_get(b, 2) + 2);
        for (int i = 0; i < num_rel_trail; i++)
            c->t_env[ne - 1 - i] = (uint8_t)(c->t_env[ne - i] - 2 * (int)aac_get(b, 2) - 2);
        bs_pointer = (int)aac_get(b, ceil_log2[ne]);
        for (int i = 0; i < ne; i++) c->freq_res[i + 1] = (uint8_t)aac_get1(b);
        break;
    }
    }
    c->frame_class = cls;
    if (bs_pointer > c->num_env + 1) return -1;
    for (int i = 1; i <= c->num_env; i++)
        if (c->t_env[i - 1] >= c->t_env[i]) return -1;
    c->num_noise = (c->num_env > 1) + 1;
    c->t_q[0] = c->t_env[0];
    c->t_q[c->num_noise] = c->t_env[c->num_env];
    if (c->num_noise > 1) {
        int idx;
        if (cls == FIXFIX) idx = c->num_env >> 1;
        else if (cls & 1) idx = c->num_env - (bs_pointer - 1 > 1 ? bs_pointer - 1 : 1);
        else idx = !bs_pointer ? 1 : bs_pointer == 1 ? c->num_env - 1 : bs_pointer - 1;
        if (idx < 0 || idx > c->num_env) return -1;
        c->t_q[1] = c->t_env[idx];
    }
    c->e_a[0] = -(c->e_a[1] != num_env_old);
    c->e_a[1] = -1;
    if ((cls & 1) && bs_pointer) c->e_a[1] = c->num_env + 1 - bs_pointer;
    else if (cls == VARFIX && bs_pointer > 1) c->e_a[1] = bs_pointer - 1;
    return 0;
}

static void copy_grid(struct sbr_ch *dst, const struct sbr_ch *src) {
    dst->freq_res[0] = dst->freq_res[dst->num_env];
    dst->t_env_num_env_old = dst->t_env[dst->num_env];
    dst->e_a[0] = -(dst->e_a[1] != dst->num_env);
    memcpy(dst->freq_res + 1, src->freq_res + 1, sizeof dst->freq_res - 1);
    memcpy(dst->t_env, src->t_env, sizeof dst->t_env);
    memcpy(dst->t_q, src->t_q, sizeof dst->t_q);
    dst->num_env = src->num_env;
    dst->amp_res = src->amp_res;
    dst->num_noise = src->num_noise;
    dst->frame_class = src->frame_class;
    dst->e_a[1] = src->e_a[1];
}

static void read_dtdf(struct aac_bits *b, struct sbr_ch *c) {
    for (int i = 0; i < c->num_env; i++) c->df_env[i] = (uint8_t)aac_get1(b);
    for (int i = 0; i < c->num_noise; i++) c->df_noise[i] = (uint8_t)aac_get1(b);
}

static void read_invf(struct aac_sbr *s, struct aac_bits *b, struct sbr_ch *c) {
    memcpy(c->invf[1], c->invf[0], 5);
    for (int i = 0; i < s->n_q; i++) c->invf[0][i] = (uint8_t)aac_get(b, 2);
}

static int read_envelope(struct aac_sbr *s, struct aac_bits *b, struct sbr_ch *c, int ch) {
    int bits, th, fh;
    int delta = (ch == 1 && s->coupling == 1) + 1;
    int odd = s->n[1] & 1;
    if (s->coupling && ch) {
        if (c->amp_res) { bits = 5; th = T_BAL_30; fh = F_BAL_30; }
        else { bits = 6; th = T_BAL_15; fh = F_BAL_15; }
    } else {
        if (c->amp_res) { bits = 6; th = T_ENV_30; fh = F_ENV_30; }
        else { bits = 7; th = T_ENV_15; fh = F_ENV_15; }
    }
    int tl = sbr_lav[th], fl = sbr_lav[fh];
    for (int i = 0; i < c->num_env; i++) {
        int fr = c->freq_res[i + 1], nb = s->n[fr];
        if (c->df_env[i]) {
            for (int j = 0; j < nb; j++) {
                int k;
                if (fr == c->freq_res[i]) k = j;
                else if (fr) k = (j + odd) >> 1;
                else k = j ? 2 * j - odd : 0;
                int v = c->env_q[i][k] + delta * (aac_vlc_get(b, &sbr_vlc[th]) - tl);
                if ((unsigned)(v & 0xff) > 127) return -1;
                c->env_q[i + 1][j] = (uint8_t)v;
            }
        } else {
            c->env_q[i + 1][0] = (uint8_t)(delta * (int)aac_get(b, bits));
            for (int j = 1; j < nb; j++) {
                int v = c->env_q[i + 1][j - 1] + delta * (aac_vlc_get(b, &sbr_vlc[fh]) - fl);
                if ((unsigned)(v & 0xff) > 127) return -1;
                c->env_q[i + 1][j] = (uint8_t)v;
            }
        }
        if (b->err) return -1;
    }
    memcpy(c->env_q[0], c->env_q[c->num_env], sizeof c->env_q[0]);
    return 0;
}

static int read_noise(struct aac_sbr *s, struct aac_bits *b, struct sbr_ch *c, int ch) {
    int th, fh;
    int delta = (ch == 1 && s->coupling == 1) + 1;
    if (s->coupling && ch) { th = T_NOISE_BAL_30; fh = F_BAL_30; }
    else { th = T_NOISE_30; fh = F_ENV_30; }
    int tl = sbr_lav[th], fl = sbr_lav[fh];
    for (int i = 0; i < c->num_noise; i++) {
        if (c->df_noise[i]) {
            for (int j = 0; j < s->n_q; j++) {
                int v = c->noise_q[i][j] + delta * (aac_vlc_get(b, &sbr_vlc[th]) - tl);
                if ((unsigned)(v & 0xff) > 30) return -1;
                c->noise_q[i + 1][j] = (uint8_t)v;
            }
        } else {
            c->noise_q[i + 1][0] = (uint8_t)(delta * (int)aac_get(b, 5));
            for (int j = 1; j < s->n_q; j++) {
                int v = c->noise_q[i + 1][j - 1] + delta * (aac_vlc_get(b, &sbr_vlc[fh]) - fl);
                if ((unsigned)(v & 0xff) > 30) return -1;
                c->noise_q[i + 1][j] = (uint8_t)v;
            }
        }
        if (b->err) return -1;
    }
    memcpy(c->noise_q[0], c->noise_q[c->num_noise], sizeof c->noise_q[0]);
    return 0;
}

static int read_sce(struct aac_sbr *s, struct aac_bits *b) {
    struct sbr_ch *c = &s->ch[0];
    if (aac_get1(b)) aac_get(b, 4);
    if (read_grid(s, b, c)) return -1;
    read_dtdf(b, c);
    read_invf(s, b, c);
    if (read_envelope(s, b, c, 0) || read_noise(s, b, c, 0)) return -1;
    if ((c->add_harmonic_flag = (int)aac_get1(b)))
        for (int i = 0; i < s->n[1]; i++) c->add_harmonic[i] = (uint8_t)aac_get1(b);
    return 0;
}

static int read_cpe(struct aac_sbr *s, struct aac_bits *b) {
    struct sbr_ch *c0 = &s->ch[0], *c1 = &s->ch[1];
    if (aac_get1(b)) aac_get(b, 8);
    if ((s->coupling = (int)aac_get1(b))) {
        if (read_grid(s, b, c0)) return -1;
        copy_grid(c1, c0);
        read_dtdf(b, c0);
        read_dtdf(b, c1);
        read_invf(s, b, c0);
        memcpy(c1->invf[1], c1->invf[0], 5);
        memcpy(c1->invf[0], c0->invf[0], 5);
        if (read_envelope(s, b, c0, 0) || read_noise(s, b, c0, 0)) return -1;
        if (read_envelope(s, b, c1, 1) || read_noise(s, b, c1, 1)) return -1;
    } else {
        if (read_grid(s, b, c0) || read_grid(s, b, c1)) return -1;
        read_dtdf(b, c0);
        read_dtdf(b, c1);
        read_invf(s, b, c0);
        read_invf(s, b, c1);
        if (read_envelope(s, b, c0, 0) || read_envelope(s, b, c1, 1)) return -1;
        if (read_noise(s, b, c0, 0) || read_noise(s, b, c1, 1)) return -1;
    }
    if ((c0->add_harmonic_flag = (int)aac_get1(b)))
        for (int i = 0; i < s->n[1]; i++) c0->add_harmonic[i] = (uint8_t)aac_get1(b);
    if ((c1->add_harmonic_flag = (int)aac_get1(b)))
        for (int i = 0; i < s->n[1]; i++) c1->add_harmonic[i] = (uint8_t)aac_get1(b);
    return 0;
}

int aac_sbr_parse(struct aac_sbr *s, struct aac_bits *b, int bits, int id_aac, int crc, int ps_allowed, int *ps_seen) {
    s->reset = 0;
    if (crc) aac_skip(b, 10);
    s->kx[0] = s->kx[1];
    s->m[0] = s->m[1];
    s->pushed = 1;
    if (aac_get1(b)) read_header(s, b);
    if (s->reset) {
        if (make_f_master(s) < 0 || make_f_derived(s) < 0) turnoff(s);
    }
    if (s->start) {
        s->id_aac = id_aac;
        s->ready = 1;
        int bad;
        if (id_aac == EL_SCE) bad = read_sce(s, b);
        else if (id_aac == EL_CPE) bad = read_cpe(s, b);
        else bad = -1;
        if (bad || b->err) {
            turnoff(s);
            return !b->err;
        }
        if (aac_get1(b)) {   /* bs_extended_data */
            int left = (int)aac_get(b, 4);
            if (left == 15) left += (int)aac_get(b, 8);
            left *= 8;
            while (left > 7 && !b->err) {
                int id = (int)aac_get(b, 2);
                left -= 2;
                if (id == 2 && ps_allowed && id_aac == EL_SCE) {
                    if (!s->ps) s->ps = aac_ps_new();
                    if (s->ps) {
                        int used = aac_ps_parse(s->ps, b, left);
                        left -= used;
                        *ps_seen = 1;
                        continue;
                    }
                }
                aac_skip(b, left);
                left = 0;
            }
            if (left > 0) aac_skip(b, left);
        }
    }
    if (b->err) turnoff(s);
    return 1;
}

/* ---------------------------------------------------------------- dequantisation */
static void dequant(struct aac_sbr *s, int id_aac) {
    const double sqrt2 = 1.41421356237309504880;
    if (id_aac == EL_CPE && s->coupling) {
        struct sbr_ch *c0 = &s->ch[0], *c1 = &s->ch[1];
        int pan = c0->amp_res ? 12 : 24;
        for (int e = 1; e <= c0->num_env; e++)
            for (int k = 0; k < s->n[c0->freq_res[e]]; k++) {
                float t1, t2;
                if (c0->amp_res) {
                    t1 = exp2fi(c0->env_q[e][k] + 7);
                    t2 = exp2fi(pan - c1->env_q[e][k]);
                } else {
                    t1 = (float)(exp2fi((c0->env_q[e][k] >> 1) + 7) * (c0->env_q[e][k] & 1 ? sqrt2 : 1.0));
                    int p = pan - c1->env_q[e][k];
                    t2 = (float)(exp2fi(p >> 1) * (p & 1 ? sqrt2 : 1.0));
                }
                if (t1 > 1e20f) t1 = 1;
                float fac = t1 / (1.0f + t2);
                c0->env[e][k] = fac;
                c1->env[e][k] = fac * t2;
            }
        for (int e = 1; e <= c0->num_noise; e++)
            for (int k = 0; k < s->n_q; k++) {
                float t1 = exp2fi(NOISE_FLOOR_OFFSET - c0->noise_q[e][k] + 1);
                float t2 = exp2fi(12 - c1->noise_q[e][k]);
                float fac = t1 / (1.0f + t2);
                c0->noise[e][k] = fac;
                c1->noise[e][k] = fac * t2;
            }
    } else {
        for (int ch = 0; ch < (id_aac == EL_CPE) + 1; ch++) {
            struct sbr_ch *c = &s->ch[ch];
            for (int e = 1; e <= c->num_env; e++)
                for (int k = 0; k < s->n[c->freq_res[e]]; k++) {
                    float v;
                    if (c->amp_res) v = exp2fi(c->env_q[e][k] + 6);
                    else v = (float)(exp2fi((c->env_q[e][k] >> 1) + 6) * (c->env_q[e][k] & 1 ? sqrt2 : 1.0));
                    if (v > 1e20f) v = 1;
                    c->env[e][k] = v;
                }
            for (int e = 1; e <= c->num_noise; e++)
                for (int k = 0; k < s->n_q; k++) c->noise[e][k] = exp2fi(NOISE_FLOOR_OFFSET - c->noise_q[e][k]);
        }
    }
}

/* ---------------------------------------------------------------- QMF */
static void qmf_analysis(struct sbr_ch *c, const float *in, float W[32][32][2]) {
    float *x = c->ana;
    memcpy(x + 288, in, 1024 * sizeof(float));
    for (int l = 0; l < 32; l++) {
        /* spec x[n] = newest-first window of 320 samples ending at sample 32 l + 31 */
        const float *xn = x + 32 * l + 319;   /* x[n] = xn[-n] */
        float u[64];
        for (int n = 0; n < 64; n++) {
            float acc = 0;
            for (int j = 0; j < 5; j++) acc += xn[-(n + 64 * j)] * aac_sbr_qmf_window[2 * (n + 64 * j)];
            u[n] = acc * 32768.0f;
        }
        for (int k = 0; k < 32; k++) {
            float re = 0, im = 0;
            const float *cc = ana_cos[k], *ss = ana_sin[k];
            for (int n = 0; n < 64; n++) { re += u[n] * cc[n]; im += u[n] * ss[n]; }
            W[l][k][0] = re;
            W[l][k][1] = im;
        }
    }
    memmove(x, x + 1024, 288 * sizeof(float));
}

static void qmf_synthesis(struct sbr_ch *c, float X[2][38][64], float *out) {
    for (int l = 0; l < 32; l++) {
        if (c->v_off < 128) {
            memmove(c->v + (int)(sizeof c->v / sizeof c->v[0]) - 1152, c->v, 1152 * sizeof(float));
            c->v_off = (int)(sizeof c->v / sizeof c->v[0]) - 1152 - 128;
        } else c->v_off -= 128;
        float *v = c->v + c->v_off;
        const float *xr = X[0][l], *xi = X[1][l];
        for (int n = 0; n < 128; n++) {
            float acc = 0;
            const float *cc = syn_cos[n], *ss = syn_sin[n];
            for (int k = 0; k < 64; k++) acc += xr[k] * cc[k] - xi[k] * ss[k];
            v[n] = acc;
        }
        const float *w = aac_sbr_qmf_window;
        for (int k = 0; k < 64; k++) {
            float acc = 0;
            for (int n = 0; n < 5; n++)
                acc += v[256 * n + k] * w[128 * n + k] + v[256 * n + 192 + k] * w[128 * n + 64 + k];
            out[64 * l + k] = acc * (1.0f / 32768.0f);
        }
    }
}

/* ---------------------------------------------------------------- HF generation */
static void lf_gen(struct aac_sbr *s, float W[2][32][32][2], int ypos) {
    memset(s->X_low, 0, sizeof s->X_low);
    for (int k = 0; k < s->kx[1]; k++)
        for (int i = 8; i < 40; i++) {
            s->X_low[k][i][0] = W[ypos][i - 8][k][0];
            s->X_low[k][i][1] = W[ypos][i - 8][k][1];
        }
    int o = 1 - ypos;
    for (int k = 0; k < s->kx[0] && k < 32; k++)
        for (int i = 0; i < 8; i++) {
            s->X_low[k][i][0] = W[o][i + 24][k][0];
            s->X_low[k][i][1] = W[o][i + 24][k][1];
        }
}

static void autocorrelate(const float x[40][2], float phi[3][2][2]) {
    float rs2 = x[0][0] * x[2][0] + x[0][1] * x[2][1];
    float is2 = x[0][0] * x[2][1] - x[0][1] * x[2][0];
    float rs1 = 0, is1 = 0, rs0 = 0;
    for (int i = 1; i < 38; i++) {
        rs0 += x[i][0] * x[i][0] + x[i][1] * x[i][1];
        rs1 += x[i][0] * x[i + 1][0] + x[i][1] * x[i + 1][1];
        is1 += x[i][0] * x[i + 1][1] - x[i][1] * x[i + 1][0];
        rs2 += x[i][0] * x[i + 2][0] + x[i][1] * x[i + 2][1];
        is2 += x[i][0] * x[i + 2][1] - x[i][1] * x[i + 2][0];
    }
    phi[0][1][0] = rs2;
    phi[0][1][1] = is2;
    phi[2][1][0] = rs0 + x[0][0] * x[0][0] + x[0][1] * x[0][1];
    phi[1][0][0] = rs0 + x[38][0] * x[38][0] + x[38][1] * x[38][1];
    phi[1][1][0] = rs1 + x[0][0] * x[1][0] + x[0][1] * x[1][1];
    phi[1][1][1] = is1 + x[0][0] * x[1][1] - x[0][1] * x[1][0];
    phi[0][0][0] = rs1 + x[38][0] * x[39][0] + x[38][1] * x[39][1];
    phi[0][0][1] = is1 + x[38][0] * x[39][1] - x[38][1] * x[39][0];
}

static void inverse_filter(struct aac_sbr *s) {
    for (int k = 0; k < s->k[0] && k < 32; k++) {
        float phi[3][2][2];
        float *a0 = s->alpha0[k], *a1 = s->alpha1[k];
        autocorrelate((const float (*)[2])s->X_low[k], phi);
        float dk = phi[2][1][0] * phi[1][0][0] - (phi[1][1][0] * phi[1][1][0] + phi[1][1][1] * phi[1][1][1]) / 1.000001f;
        if (!dk) a1[0] = a1[1] = 0;
        else {
            float tr = phi[0][0][0] * phi[1][1][0] - phi[0][0][1] * phi[1][1][1] - phi[0][1][0] * phi[1][0][0];
            float ti = phi[0][0][0] * phi[1][1][1] + phi[0][0][1] * phi[1][1][0] - phi[0][1][1] * phi[1][0][0];
            a1[0] = tr / dk;
            a1[1] = ti / dk;
        }
        if (!phi[1][0][0]) a0[0] = a0[1] = 0;
        else {
            float tr = phi[0][0][0] + a1[0] * phi[1][1][0] + a1[1] * phi[1][1][1];
            float ti = phi[0][0][1] + a1[1] * phi[1][1][0] - a1[0] * phi[1][1][1];
            a0[0] = -tr / phi[1][0][0];
            a0[1] = -ti / phi[1][0][0];
        }
        if (a1[0] * a1[0] + a1[1] * a1[1] >= 16.0f || a0[0] * a0[0] + a0[1] * a0[1] >= 16.0f)
            a0[0] = a0[1] = a1[0] = a1[1] = 0;
    }
}

static void chirp(struct aac_sbr *s, struct sbr_ch *c) {
    static const float bw_tab[4] = { 0.0f, 0.75f, 0.9f, 0.98f };
    for (int i = 0; i < s->n_q; i++) {
        float nb = c->invf[0][i] + c->invf[1][i] == 1 ? 0.6f : bw_tab[c->invf[0][i]];
        if (nb < c->bw[i]) nb = 0.75f * nb + 0.25f * c->bw[i];
        else nb = 0.90625f * nb + 0.09375f * c->bw[i];
        c->bw[i] = nb < 0.015625f ? 0.0f : nb;
    }
}

static int hf_gen(struct aac_sbr *s, struct sbr_ch *c) {
    int g = 0, k = s->kx[1];
    int start = 2 * c->t_env[0], end = 2 * c->t_env[c->num_env];
    for (int j = 0; j < s->num_patches; j++) {
        for (int x = 0; x < s->patch_num[j]; x++, k++) {
            int p = s->patch_start[j] + x;
            while (g <= s->n_q && k >= s->f_noise[g]) g++;
            g--;
            if (g < 0 || k >= 64 || p >= 32) return -1;
            float bw = c->bw[g];
            float ar = s->alpha1[p][0] * bw * bw, ai = s->alpha1[p][1] * bw * bw;
            float br = s->alpha0[p][0] * bw, bi = s->alpha0[p][1] * bw;
            float (*xh)[2] = s->X_high[k] + ENV_ADJ_OFF;
            const float (*xl)[2] = (const float (*)[2])s->X_low[p] + ENV_ADJ_OFF;
            for (int i = start; i < end; i++) {
                xh[i][0] = xl[i - 2][0] * ar - xl[i - 2][1] * ai + xl[i - 1][0] * br - xl[i - 1][1] * bi + xl[i][0];
                xh[i][1] = xl[i - 2][1] * ar + xl[i - 2][0] * ai + xl[i - 1][1] * br + xl[i - 1][0] * bi + xl[i][1];
            }
        }
    }
    if (k < s->m[1] + s->kx[1]) memset(s->X_high + k, 0, (size_t)(s->m[1] + s->kx[1] - k) * sizeof s->X_high[0]);
    return 0;
}

/* ---------------------------------------------------------------- HF adjustment */
static int mapping(struct aac_sbr *s, struct sbr_ch *c) {
    int kx = s->kx[1];
    memset(c->s_indexmapped[1], 0, 7 * sizeof c->s_indexmapped[1]);
    for (int e = 0; e < c->num_env; e++) {
        int ilim = s->n[c->freq_res[e + 1]];
        const uint16_t *table = c->freq_res[e + 1] ? s->f_high : s->f_low;
        if (kx != table[0]) return -1;
        for (int i = 0; i < ilim; i++)
            for (int m = table[i]; m < table[i + 1]; m++) s->e_origmapped[e][m - kx] = c->env[e + 1][i];
        int k = c->num_noise > 1 && c->t_env[e] >= c->t_q[1];
        for (int i = 0; i < s->n_q; i++)
            for (int m = s->f_noise[i]; m < s->f_noise[i + 1]; m++) s->q_mapped[e][m - kx] = c->noise[k + 1][i];
        for (int i = 0; i < s->n[1]; i++)
            if (c->add_harmonic_flag) {
                int mid = (s->f_high[i] + s->f_high[i + 1]) >> 1;
                c->s_indexmapped[e + 1][mid - kx] = (uint8_t)(c->add_harmonic[i] *
                    (e >= c->e_a[1] || c->s_indexmapped[0][mid - kx] == 1));
            }
        for (int i = 0; i < ilim; i++) {
            int present = 0;
            for (int m = table[i]; m < table[i + 1]; m++)
                if (c->s_indexmapped[e + 1][m - kx]) { present = 1; break; }
            memset(&s->s_mapped[e][table[i] - kx], present, (size_t)(table[i + 1] - table[i]));
        }
    }
    memcpy(c->s_indexmapped[0], c->s_indexmapped[c->num_env], sizeof c->s_indexmapped[0]);
    return 0;
}

static void env_estimate(struct aac_sbr *s, struct sbr_ch *c) {
    int kx = s->kx[1];
    for (int e = 0; e < c->num_env; e++) {
        int ilb = c->t_env[e] * 2 + ENV_ADJ_OFF, iub = c->t_env[e + 1] * 2 + ENV_ADJ_OFF;
        if (s->interpol_freq) {
            float r = 0.5f / (c->t_env[e + 1] - c->t_env[e]);
            for (int m = 0; m < s->m[1]; m++) {
                float s0 = 0, s1 = 0;
                const float (*x)[2] = (const float (*)[2])s->X_high[m + kx];
                for (int i = ilb; i < iub; i++) { s0 += x[i][0] * x[i][0]; s1 += x[i][1] * x[i][1]; }
                s->e_curr[e][m] = (s0 + s1) * r;
            }
        } else {
            int env_size = 2 * (c->t_env[e + 1] - c->t_env[e]);
            const uint16_t *table = c->freq_res[e + 1] ? s->f_high : s->f_low;
            for (int p = 0; p < s->n[c->freq_res[e + 1]]; p++) {
                float sum = 0;
                int den = env_size * (table[p + 1] - table[p]);
                for (int k = table[p]; k < table[p + 1]; k++) {
                    float s0 = 0, s1 = 0;
                    for (int i = ilb; i < iub; i++) { s0 += s->X_high[k][i][0] * s->X_high[k][i][0]; s1 += s->X_high[k][i][1] * s->X_high[k][i][1]; }
                    sum += s0 + s1;
                }
                sum /= den;
                for (int k = table[p]; k < table[p + 1]; k++) s->e_curr[e][k - kx] = sum;
            }
        }
    }
}

static void gain_calc(struct aac_sbr *s, struct sbr_ch *c) {
    static const float limgain[4] = { 0.70795f, 1.0f, 1.41254f, 10000000000.0f };
    const float eps = 1.19209290e-07f, fmin = 1.17549435e-38f;
    int kx = s->kx[1];
    for (int e = 0; e < c->num_env; e++) {
        int delta = !(e == c->e_a[1] || e == c->e_a[0]);
        for (int k = 0; k < s->n_lim; k++) {
            int m0 = s->f_lim[k] - kx, m1 = s->f_lim[k + 1] - kx;
            float sum0 = 0, sum1 = 0;
            for (int m = m0; m < m1; m++) {
                float t = s->e_origmapped[e][m] / (1.0f + s->q_mapped[e][m]);
                s->q_m[e][m] = aac_sqrtf(t * s->q_mapped[e][m]);
                s->s_m[e][m] = aac_sqrtf(t * c->s_indexmapped[e + 1][m]);
                if (!s->s_mapped[e][m])
                    s->gain[e][m] = aac_sqrtf(s->e_origmapped[e][m] / ((1.0f + s->e_curr[e][m]) * (1.0f + s->q_mapped[e][m] * delta)));
                else
                    s->gain[e][m] = aac_sqrtf(s->e_origmapped[e][m] * s->q_mapped[e][m] / ((1.0f + s->e_curr[e][m]) * (1.0f + s->q_mapped[e][m])));
                s->gain[e][m] += fmin;
            }
            for (int m = m0; m < m1; m++) { sum0 += s->e_origmapped[e][m]; sum1 += s->e_curr[e][m]; }
            float gmax = limgain[s->limiter_gains] * aac_sqrtf((eps + sum0) / (eps + sum1));
            if (gmax > 100000.f) gmax = 100000.f;
            for (int m = m0; m < m1; m++) {
                float qmax = s->q_m[e][m] * gmax / s->gain[e][m];
                if (qmax < s->q_m[e][m]) s->q_m[e][m] = qmax;
                if (gmax < s->gain[e][m]) s->gain[e][m] = gmax;
            }
            sum0 = sum1 = 0;
            for (int m = m0; m < m1; m++) {
                sum0 += s->e_origmapped[e][m];
                sum1 += s->e_curr[e][m] * s->gain[e][m] * s->gain[e][m] + s->s_m[e][m] * s->s_m[e][m]
                        + (delta && !s->s_m[e][m]) * s->q_m[e][m] * s->q_m[e][m];
            }
            float boost = aac_sqrtf((eps + sum0) / (eps + sum1));
            if (boost > 1.584893192f) boost = 1.584893192f;
            for (int m = m0; m < m1; m++) {
                s->gain[e][m] *= boost;
                s->q_m[e][m] *= boost;
                s->s_m[e][m] *= boost;
            }
        }
    }
}

static void hf_assemble(struct aac_sbr *s, struct sbr_ch *c, float Y1[38][64][2]) {
    static const float h_smooth[5] = { 0.33333333333333f, 0.30150283239582f, 0.21816949906249f, 0.11516383427084f, 0.03183050093751f };
    int h_sl = 4 * !s->smoothing_mode;
    int kx = s->kx[1], m_max = s->m[1];
    float (*g_temp)[48] = c->g_temp, (*q_temp)[48] = c->q_temp;
    int indexnoise = (int)c->f_indexnoise, indexsine = (int)c->f_indexsine;
    if (s->reset) {
        for (int i = 0; i < h_sl; i++) {
            memcpy(g_temp[i + 2 * c->t_env[0]], s->gain[0], (size_t)m_max * sizeof(float));
            memcpy(q_temp[i + 2 * c->t_env[0]], s->q_m[0], (size_t)m_max * sizeof(float));
        }
    } else if (h_sl) {
        for (int i = 0; i < 4; i++) {
            memcpy(g_temp[i + 2 * c->t_env[0]], g_temp[i + 2 * c->t_env_num_env_old], sizeof g_temp[0]);
            memcpy(q_temp[i + 2 * c->t_env[0]], q_temp[i + 2 * c->t_env_num_env_old], sizeof q_temp[0]);
        }
    }
    for (int e = 0; e < c->num_env; e++)
        for (int i = 2 * c->t_env[e]; i < 2 * c->t_env[e + 1]; i++) {
            memcpy(g_temp[h_sl + i], s->gain[e], (size_t)m_max * sizeof(float));
            memcpy(q_temp[h_sl + i], s->q_m[e], (size_t)m_max * sizeof(float));
        }
    for (int e = 0; e < c->num_env; e++) {
        for (int i = 2 * c->t_env[e]; i < 2 * c->t_env[e + 1]; i++) {
            float gf_tab[48], qf_tab[48];
            const float *g_filt, *q_filt;
            if (h_sl && e != c->e_a[0] && e != c->e_a[1]) {
                for (int m = 0; m < m_max; m++) {
                    int idx1 = i + h_sl;
                    float gs = 0, qs = 0;
                    for (int j = 0; j <= h_sl; j++) {
                        gs += g_temp[idx1 - j][m] * h_smooth[j];
                        qs += q_temp[idx1 - j][m] * h_smooth[j];
                    }
                    gf_tab[m] = gs;
                    qf_tab[m] = qs;
                }
                g_filt = gf_tab;
                q_filt = qf_tab;
            } else {
                g_filt = g_temp[i + h_sl];
                q_filt = q_temp[i];
            }
            float (*y)[2] = Y1[i] + kx;
            for (int m = 0; m < m_max; m++) {
                y[m][0] = s->X_high[kx + m][i + ENV_ADJ_OFF][0] * g_filt[m];
                y[m][1] = s->X_high[kx + m][i + ENV_ADJ_OFF][1] * g_filt[m];
            }
            const float *sm = s->s_m[e];
            if (e != c->e_a[0] && e != c->e_a[1]) {
                float ps0, ps1;
                float sg = (float)(1 - 2 * (kx & 1));
                switch (indexsine) {
                case 0: ps0 = 1; ps1 = 0; break;
                case 1: ps0 = 0; ps1 = sg; break;
                case 2: ps0 = -1; ps1 = 0; break;
                default: ps0 = 0; ps1 = -sg; break;
                }
                int noise = indexnoise;
                for (int m = 0; m < m_max; m++) {
                    noise = (noise + 1) & 0x1ff;
                    if (sm[m]) {
                        y[m][0] += sm[m] * ps0;
                        y[m][1] += sm[m] * ps1;
                    } else {
                        y[m][0] += q_filt[m] * aac_sbr_noise[noise][0];
                        y[m][1] += q_filt[m] * aac_sbr_noise[noise][1];
                    }
                    ps1 = -ps1;
                }
            } else {
                int idx = indexsine & 1;
                int A = 1 - ((indexsine + (kx & 1)) & 2);
                int B = (A ^ (-idx)) + idx;
                for (int m = 0; m < m_max; m++) y[m][idx] += sm[m] * (float)(m & 1 ? B : A);
            }
            indexnoise = (indexnoise + m_max) & 0x1ff;
            indexsine = (indexsine + 1) & 3;
        }
    }
    c->f_indexnoise = (unsigned)indexnoise;
    c->f_indexsine = (unsigned)indexsine;
}

static void x_gen(struct aac_sbr *s, float X[2][38][64], float Y0[38][64][2], float Y1[38][64][2], int ch) {
    int i_temp = 2 * s->ch[ch].t_env_num_env_old - 32;
    if (i_temp < 0) i_temp = 0;
    memset(X, 0, 2 * sizeof X[0]);
    int k;
    for (k = 0; k < s->kx[0] && k < 32; k++)
        for (int i = 0; i < i_temp; i++) {
            X[0][i][k] = s->X_low[k][i + ENV_ADJ_OFF][0];
            X[1][i][k] = s->X_low[k][i + ENV_ADJ_OFF][1];
        }
    for (; k < s->kx[0] + s->m[0] && k < 64; k++)
        for (int i = 0; i < i_temp; i++) {
            X[0][i][k] = Y0[i + 32][k][0];
            X[1][i][k] = Y0[i + 32][k][1];
        }
    for (k = 0; k < s->kx[1] && k < 32; k++)
        for (int i = i_temp; i < 38; i++) {
            X[0][i][k] = s->X_low[k][i + ENV_ADJ_OFF][0];
            X[1][i][k] = s->X_low[k][i + ENV_ADJ_OFF][1];
        }
    for (; k < s->kx[1] + s->m[1] && k < 64; k++)
        for (int i = i_temp; i < 32; i++) {
            X[0][i][k] = Y1[i][k][0];
            X[1][i][k] = Y1[i][k][1];
        }
}

/* ---------------------------------------------------------------- API */
struct aac_sbr *aac_sbr_new(int core_rate) {
    if (!sbr_init_tables()) return 0;
    struct aac_sbr *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->sample_rate = 2 * core_rate;
    s->kx[0] = s->kx[1];
    s->id_aac = -1;
    turnoff(s);
    for (int c = 0; c < 2; c++) s->ch[c].v_off = (int)(sizeof s->ch[c].v / sizeof s->ch[c].v[0]) - 1152;
    return s;
}

void aac_sbr_free(struct aac_sbr *s) {
    if (!s) return;
    aac_ps_free(s->ps);
    free(s);
}

void aac_sbr_reset(struct aac_sbr *s) {
    if (!s) return;
    for (int c = 0; c < 2; c++) {
        memset(s->ch[c].ana, 0, sizeof s->ch[c].ana);
        memset(s->ch[c].v, 0, sizeof s->ch[c].v);
        memset(s->ch[c].W, 0, sizeof s->ch[c].W);
        memset(s->ch[c].Y, 0, sizeof s->ch[c].Y);
    }
    if (s->ps) aac_ps_reset(s->ps);
}

void aac_sbr_apply(struct aac_sbr *s, int id_aac, float *L, float *R, int ps_mode) {
    int nch = id_aac == EL_CPE ? 2 : 1;
    if (id_aac != s->id_aac) turnoff(s);
    if (s->start && !s->ready) turnoff(s);
    if (!s->pushed) {
        s->kx[0] = s->kx[1];
        s->m[0] = s->m[1];
    } else s->pushed = 0;
    if (s->start) {
        dequant(s, id_aac);
        s->ready = 0;
    }
    for (int ch = 0; ch < nch; ch++) {
        struct sbr_ch *c = &s->ch[ch];
        qmf_analysis(c, ch ? R : L, c->W[c->ypos]);
        lf_gen(s, c->W, c->ypos);
        c->ypos ^= 1;
        if (s->start) {
            inverse_filter(s);
            chirp(s, c);
            if (c->num_env > 0 && hf_gen(s, c) == 0 && mapping(s, c) == 0) {
                env_estimate(s, c);
                gain_calc(s, c);
                hf_assemble(s, c, c->Y[c->ypos]);
            } else if (c->num_env > 0) turnoff(s);
        }
        x_gen(s, s->X[ch], c->Y[1 - c->ypos], c->Y[c->ypos], ch);
    }
    if (ps_mode) {
        if (s->ps && aac_ps_started(s->ps)) aac_ps_apply(s->ps, s->X[0], s->X[1], s->kx[1] + s->m[1]);
        else memcpy(s->X[1], s->X[0], sizeof s->X[0]);
        nch = 2;
    }
    qmf_synthesis(&s->ch[0], s->X[0], L);
    if (nch == 2) qmf_synthesis(&s->ch[1], s->X[1], R);
}
