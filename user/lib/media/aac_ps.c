/* AXIOM AAC decoder: Parametric Stereo (HE-AAC v2, ISO/IEC 14496-3 8.6.4 / 8.A).
 * Bitstream parsing (IID, ICC, IPD/OPD, 10/20/34 bands, both frame classes), hybrid analysis filterbank,
 * decorrelation (all-pass chain with transient attenuation), mixing with interpolated matrices (mixing
 * procedures A and B, IPD/OPD phase smoothing) and hybrid synthesis. Follows ffmpeg's decoder on the
 * corner cases so the outputs can be compared. */
#include "aac_int.h"

#define MAX_ENV 5
#define MAX_PAR 34
#define MAX_SSB 91
#define AP_BANDS 50
#define SLOTS 32
#define MAX_DELAY 14
#define AP_LINKS 3
#define MAX_AP_DELAY 5

extern const struct aac_hsrc aac_ps_huff_src[10];
extern const int8_t aac_ps_huff_offset[10];
extern const int8_t aac_ps_k_to_i_20[71], aac_ps_k_to_i_34[91];

enum { IID_DF1, IID_DT1, IID_DF0, IID_DT0, ICC_DF, ICC_DT, IPD_DF, IPD_DT, OPD_DF, OPD_DT };

static const int NR_PAR_BANDS[2] = { 20, 34 };
static const int NR_IPDOPD_BANDS[2] = { 11, 17 };
static const int NR_BANDS[2] = { 71, 91 };
static const int DECAY_CUTOFF[2] = { 10, 32 };
static const int NR_ALLPASS_BANDS[2] = { 30, 50 };
static const int SHORT_DELAY_BAND[2] = { 42, 62 };

static struct aac_vlc ps_vlc[10];
static float pd_re_smooth[512], pd_im_smooth[512];
static float HA[46][8][4], HB[46][8][4];
static float f20_0_8[8][8][2], f34_0_12[12][8][2], f34_1_8[8][8][2], f34_2_4[4][8][2];
static float Q_fract_allpass[2][50][3][2], phi_fract[2][50][2];
static int ps_ready;

struct aac_ps {
    /* bitstream state */
    int start, enable_iid, iid_quant, nr_iid_par, nr_ipdopd_par, enable_icc, icc_mode, nr_icc_par, enable_ext;
    int frame_class, num_env_old, num_env, enable_ipdopd;
    int border[MAX_ENV + 1];
    int8_t iid_par[MAX_ENV][MAX_PAR], icc_par[MAX_ENV][MAX_PAR], ipd_par[MAX_ENV][MAX_PAR], opd_par[MAX_ENV][MAX_PAR];
    int is34, is34_old;
    /* processing state */
    float in_buf[5][44][2];
    float delay[MAX_SSB][SLOTS + MAX_DELAY][2];
    float ap_delay[AP_BANDS][AP_LINKS][SLOTS + MAX_AP_DELAY][2];
    float peak_decay_nrg[34], power_smooth[34], peak_decay_diff_smooth[34];
    float H11[2][MAX_ENV + 1][MAX_PAR], H12[2][MAX_ENV + 1][MAX_PAR], H21[2][MAX_ENV + 1][MAX_PAR], H22[2][MAX_ENV + 1][MAX_PAR];
    float Lbuf[MAX_SSB][32][2], Rbuf[MAX_SSB][32][2];
    int8_t opd_hist[MAX_PAR], ipd_hist[MAX_PAR];
};

/* ---------------------------------------------------------------- tables */
static double atan_(double x) {
    int neg = x < 0;
    if (neg) x = -x;
    int inv = x > 1;
    if (inv) x = 1 / x;
    /* two argument halvings: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
    for (int i = 0; i < 2; i++) x = x / (1 + aac_sqrt(1 + x * x));
    double x2 = x * x, term = x, sum = 0;
    for (int k = 0; k < 30; k++) {
        sum += term / (2 * k + 1);
        term *= -x2;
    }
    sum *= 4;
    if (inv) sum = AAC_PI / 2 - sum;
    return neg ? -sum : sum;
}

static double atan2_(double y, double x) {
    if (x > 0) return atan_(y / x);
    if (x < 0) return y >= 0 ? atan_(y / x) + AAC_PI : atan_(y / x) - AAC_PI;
    return y > 0 ? AAC_PI / 2 : y < 0 ? -AAC_PI / 2 : 0;
}

static void make_filters(float (*filter)[8][2], const float *proto, int bands) {
    for (int q = 0; q < bands; q++)
        for (int n = 0; n < 7; n++) {
            double theta = 2 * AAC_PI * (q + 0.5) * (n - 6) / bands;
            filter[q][n][0] = (float)(proto[n] * aac_cos(theta));
            filter[q][n][1] = (float)(proto[n] * -aac_sin(theta));
        }
}

static int ps_init_tables(void) {
    if (ps_ready) return 1;
    for (int i = 0; i < 10; i++)
        if (!aac_vlc_build(&ps_vlc[i], &aac_ps_huff_src[i], i >= IPD_DF ? 5 : 9, -aac_ps_huff_offset[i])) return 0;
    const double s2 = 0.70710678118654752440;
    static const double ipdopd_sin[8] = { 0, 0.70710678118654752440, 1, 0.70710678118654752440, 0, -0.70710678118654752440, -1, -0.70710678118654752440 };
    static const double ipdopd_cos[8] = { 1, 0.70710678118654752440, 0, -0.70710678118654752440, -1, -0.70710678118654752440, 0, 0.70710678118654752440 };
    for (int p0 = 0; p0 < 8; p0++)
        for (int p1 = 0; p1 < 8; p1++)
            for (int p2 = 0; p2 < 8; p2++) {
                float re = (float)(0.25f * (float)ipdopd_cos[p0] + 0.5f * (float)ipdopd_cos[p1] + (float)ipdopd_cos[p2]);
                float im = (float)(0.25f * (float)ipdopd_sin[p0] + 0.5f * (float)ipdopd_sin[p1] + (float)ipdopd_sin[p2]);
                float mag = (float)(1 / aac_sqrt((double)im * im + (double)re * re));
                pd_re_smooth[p0 * 64 + p1 * 8 + p2] = re * mag;
                pd_im_smooth[p0 * 64 + p1 * 8 + p2] = im * mag;
            }
    static const double iid_dq[46] = {
        0.05623413251903, 0.12589254117942, 0.19952623149689, 0.31622776601684, 0.44668359215096, 0.63095734448019,
        0.79432823472428, 1, 1.25892541179417, 1.58489319246111, 2.23872113856834, 3.16227766016838,
        5.01187233627272, 7.94328234724282, 17.7827941003892,
        0.00316227766017, 0.00562341325190, 0.01, 0.01778279410039, 0.03162277660168, 0.05623413251903,
        0.07943282347243, 0.11220184543020, 0.15848931924611, 0.22387211385683, 0.31622776601684, 0.39810717055350,
        0.50118723362727, 0.63095734448019, 0.79432823472428, 1, 1.25892541179417, 1.58489319246111,
        1.99526231496888, 2.51188643150958, 3.16227766016838, 4.46683592150963, 6.30957344480193, 8.91250938133745,
        12.5892541179417, 17.7827941003892, 31.6227766016838, 56.2341325190349, 100, 177.827941003892, 316.227766016837,
    };
    static const float icc_invq[8] = { 1, 0.937f, 0.84118f, 0.60092f, 0.36764f, 0, -0.589f, -1 };
    static const float acos_icc_invq[8] = { 0, 0.35685527f, 0.57133466f, 0.92614472f, 1.1943263f, (float)(AAC_PI / 2), 2.2006171f, (float)AAC_PI };
    for (int iid = 0; iid < 46; iid++) {
        float c = (float)iid_dq[iid];
        float c1 = (float)(1.41421356237309504880 / aac_sqrt(1.0f + c * c));
        float c2 = c * c1;
        for (int icc = 0; icc < 8; icc++) {
            float alpha = 0.5f * acos_icc_invq[icc];
            float beta = alpha * (c1 - c2) * (float)s2;
            HA[iid][icc][0] = c2 * (float)aac_cos(beta + alpha);
            HA[iid][icc][1] = c1 * (float)aac_cos(beta - alpha);
            HA[iid][icc][2] = c2 * (float)aac_sin(beta + alpha);
            HA[iid][icc][3] = c1 * (float)aac_sin(beta - alpha);
            float rho = icc_invq[icc] > 0.05f ? icc_invq[icc] : 0.05f;
            float a = 0.5f * (float)atan2_(2.0f * c * rho, c * c - 1.0f);
            float mu = c + 1.0f / c;
            mu = (float)aac_sqrt(1 + (4 * rho * rho - 4) / (mu * mu));
            float gamma = (float)atan_(aac_sqrt((1.0f - mu) / (1.0f + mu)));
            if (a < 0) a += (float)(AAC_PI / 2);
            float ac = (float)aac_cos(a), as = (float)aac_sin(a), gc = (float)aac_cos(gamma), gs = (float)aac_sin(gamma);
            HB[iid][icc][0] = (float)(1.41421356237309504880 * ac * gc);
            HB[iid][icc][1] = (float)(1.41421356237309504880 * as * gc);
            HB[iid][icc][2] = (float)(-1.41421356237309504880 * as * gs);
            HB[iid][icc][3] = (float)(1.41421356237309504880 * ac * gs);
        }
    }
    static const int8_t f_center_20[10] = { -3, -1, 1, 3, 5, 7, 10, 14, 18, 22 };
    static const int8_t f_center_34[32] = {
        2, 6, 10, 14, 18, 22, 26, 30, 34, -10, -6, -2, 51, 57, 15, 21,
        27, 33, 39, 45, 54, 66, 78, 42, 102, 66, 78, 90, 102, 114, 126, 90 };
    static const float fdl[3] = { 0.43f, 0.75f, 0.347f };
    const float fdg = 0.39f;
    for (int is34 = 0; is34 < 2; is34++)
        for (int k = 0; k < NR_ALLPASS_BANDS[is34]; k++) {
            double fc;
            if (!is34) fc = k < 10 ? f_center_20[k] * 0.125 : k - 6.5f;
            else fc = k < 32 ? f_center_34[k] / 24.0 : k - 26.5f;
            for (int m = 0; m < AP_LINKS; m++) {
                double th = -AAC_PI * fdl[m] * fc;
                Q_fract_allpass[is34][k][m][0] = (float)aac_cos(th);
                Q_fract_allpass[is34][k][m][1] = (float)aac_sin(th);
            }
            double th = -AAC_PI * fdg * fc;
            phi_fract[is34][k][0] = (float)aac_cos(th);
            phi_fract[is34][k][1] = (float)aac_sin(th);
        }
    static const float g0_Q8[7] = { 0.00746082949812f, 0.02270420949825f, 0.04546865930473f, 0.07266113929591f, 0.09885108575264f, 0.11793710567217f, 0.125f };
    static const float g0_Q12[7] = { 0.04081179924692f, 0.03812810994926f, 0.05144908135699f, 0.06399831151592f, 0.07428313801106f, 0.08100347892914f, 0.08333333333333f };
    static const float g1_Q8[7] = { 0.01565675600122f, 0.03752716391991f, 0.05417891378782f, 0.08417044116767f, 0.10307344158036f, 0.12222452249753f, 0.125f };
    static const float g2_Q4[7] = { -0.05908211155639f, -0.04871498374946f, 0.0f, 0.07778723915851f, 0.16486303567403f, 0.23279856662996f, 0.25f };
    make_filters(f20_0_8, g0_Q8, 8);
    make_filters(f34_0_12, g0_Q12, 12);
    make_filters(f34_1_8, g1_Q8, 8);
    make_filters(f34_2_4, g2_Q4, 4);
    ps_ready = 1;
    return 1;
}

/* ---------------------------------------------------------------- bitstream */
static const int8_t num_env_tab[2][4] = { { 0, 1, 2, 4 }, { 1, 2, 3, 4 } };
static const int8_t nr_iidicc_par_tab[6] = { 10, 20, 34, 10, 20, 34 };
static const int8_t nr_iidopd_par_tab[6] = { 5, 11, 17, 5, 11, 17 };

/* kind: 0 iid, 1 icc, 2 ipd/opd */
static int read_par(struct aac_ps *p, struct aac_bits *b, int8_t (*par)[MAX_PAR], int table, int e, int dt, int kind) {
    int num = kind == 0 ? p->nr_iid_par : kind == 1 ? p->nr_icc_par : p->nr_ipdopd_par;
    const struct aac_vlc *v = &ps_vlc[table];
    int e_prev = e ? e - 1 : p->num_env_old - 1;
    if (e_prev < 0) e_prev = 0;
    int val = 0;
    for (int bnd = 0; bnd < num; bnd++) {
        int d = aac_vlc_get(b, v);
        if (dt) val = par[e_prev][bnd] + d;
        else val += d;
        if (kind == 2) val &= 7;
        par[e][bnd] = (int8_t)val;
        if (kind == 0 && (par[e][bnd] > 7 + 8 * p->iid_quant || par[e][bnd] < -7 - 8 * p->iid_quant)) return -1;
        if (kind == 1 && (uint8_t)par[e][bnd] > 7) return -1;
    }
    return b->err ? -1 : 0;
}

int aac_ps_parse(struct aac_ps *p, struct aac_bits *host, int bits_left) {
    struct aac_bits g = *host, *b = &g;
    if (bits_left < 0) bits_left = 0;
    if (g.end > host->pos + bits_left) g.end = host->pos + bits_left;
    int header = (int)aac_get1(b);
    if (header) {
        p->enable_iid = (int)aac_get1(b);
        if (p->enable_iid) {
            int mode = (int)aac_get(b, 3);
            if (mode > 5) goto err;
            p->nr_iid_par = nr_iidicc_par_tab[mode];
            p->iid_quant = mode > 2;
            p->nr_ipdopd_par = nr_iidopd_par_tab[mode];
        }
        p->enable_icc = (int)aac_get1(b);
        if (p->enable_icc) {
            p->icc_mode = (int)aac_get(b, 3);
            if (p->icc_mode > 5) goto err;
            p->nr_icc_par = nr_iidicc_par_tab[p->icc_mode];
        }
        p->enable_ext = (int)aac_get1(b);
    }
    p->frame_class = (int)aac_get1(b);
    p->num_env_old = p->num_env;
    p->num_env = num_env_tab[p->frame_class][aac_get(b, 2)];
    p->border[0] = -1;
    if (p->frame_class) {
        for (int e = 1; e <= p->num_env; e++) {
            p->border[e] = (int)aac_get(b, 5);
            if (p->border[e] < p->border[e - 1]) goto err;
        }
    } else {
        static const int8_t log2tab[5] = { 0, 0, 1, 1, 2 };
        for (int e = 1; e <= p->num_env; e++) p->border[e] = (e * SLOTS >> log2tab[p->num_env]) - 1;
    }
    if (p->enable_iid) {
        static const int huff_iid[4] = { IID_DF0, IID_DF1, IID_DT0, IID_DT1 };
        for (int e = 0; e < p->num_env; e++) {
            int dt = (int)aac_get1(b);
            if (read_par(p, b, p->iid_par, huff_iid[2 * dt + p->iid_quant], e, dt, 0)) goto err;
        }
    } else memset(p->iid_par, 0, sizeof p->iid_par);
    if (p->enable_icc) {
        for (int e = 0; e < p->num_env; e++) {
            int dt = (int)aac_get1(b);
            if (read_par(p, b, p->icc_par, dt ? ICC_DT : ICC_DF, e, dt, 1)) goto err;
        }
    } else memset(p->icc_par, 0, sizeof p->icc_par);
    if (p->enable_ext) {
        int cnt = (int)aac_get(b, 4);
        if (cnt == 15) cnt += (int)aac_get(b, 8);
        cnt *= 8;
        while (cnt > 7) {
            int id = (int)aac_get(b, 2);
            int start = b->pos;
            if (id == 0) {
                p->enable_ipdopd = (int)aac_get1(b);
                if (p->enable_ipdopd)
                    for (int e = 0; e < p->num_env; e++) {
                        int dt = (int)aac_get1(b);
                        read_par(p, b, p->ipd_par, dt ? IPD_DT : IPD_DF, e, dt, 2);
                        dt = (int)aac_get1(b);
                        read_par(p, b, p->opd_par, dt ? OPD_DT : OPD_DF, e, dt, 2);
                    }
                aac_get1(b);
            }
            cnt -= 2 + (b->pos - start);
            if (b->err) goto err;
        }
        if (cnt < 0) goto err;
        aac_skip(b, cnt);
    }
    /* a final envelope ending before the frame end: repeat the last one */
    if (!p->num_env || p->border[p->num_env] < SLOTS - 1) {
        int source = p->num_env ? p->num_env - 1 : p->num_env_old - 1;
        if (source >= 0 && source != p->num_env) {
            if (p->enable_iid) memcpy(p->iid_par[p->num_env], p->iid_par[source], sizeof p->iid_par[0]);
            if (p->enable_icc) memcpy(p->icc_par[p->num_env], p->icc_par[source], sizeof p->icc_par[0]);
            if (p->enable_ipdopd) {
                memcpy(p->ipd_par[p->num_env], p->ipd_par[source], sizeof p->ipd_par[0]);
                memcpy(p->opd_par[p->num_env], p->opd_par[source], sizeof p->opd_par[0]);
            }
        }
        if (p->enable_iid)
            for (int k = 0; k < p->nr_iid_par; k++) {
                int v = p->iid_par[p->num_env][k];
                if (v > 7 + 8 * p->iid_quant || v < -7 - 8 * p->iid_quant) goto err;
            }
        if (p->enable_icc)
            for (int k = 0; k < p->nr_iid_par; k++)
                if ((uint8_t)p->icc_par[p->num_env][k] > 7) goto err;
        p->num_env++;
        p->border[p->num_env] = SLOTS - 1;
    }
    p->is34_old = p->is34;
    if (p->enable_iid || p->enable_icc)
        p->is34 = (p->enable_iid && p->nr_iid_par == 34) || (p->enable_icc && p->nr_icc_par == 34);
    if (!p->enable_ipdopd) {
        memset(p->ipd_par, 0, sizeof p->ipd_par);
        memset(p->opd_par, 0, sizeof p->opd_par);
    }
    if (header) p->start = 1;
    if (!b->err) {
        int used = b->pos - host->pos;
        host->pos = b->pos;
        return used;
    }
err:
    p->start = 0;
    aac_skip(host, bits_left);
    memset(p->iid_par, 0, sizeof p->iid_par);
    memset(p->icc_par, 0, sizeof p->icc_par);
    memset(p->ipd_par, 0, sizeof p->ipd_par);
    memset(p->opd_par, 0, sizeof p->opd_par);
    return bits_left;
}

/* ---------------------------------------------------------------- hybrid filterbank */
static void hybrid_filter(float (*out)[2], float (*in)[2], const float (*filter)[8][2], int stride, int n) {
    float inre0[6], inre1[6], inim0[6], inim1[6];
    for (int j = 0; j < 6; j++) {
        inre0[j] = in[j][0] + in[12 - j][0];
        inre1[j] = in[j][1] - in[12 - j][1];
        inim0[j] = in[j][1] + in[12 - j][1];
        inim1[j] = in[j][0] - in[12 - j][0];
    }
    for (int i = 0; i < n; i++) {
        float sr = filter[i][6][0] * in[6][0], si = filter[i][6][0] * in[6][1];
        for (int j = 0; j < 6; j++) {
            sr += filter[i][j][0] * inre0[j] - filter[i][j][1] * inre1[j];
            si += filter[i][j][0] * inim0[j] + filter[i][j][1] * inim1[j];
        }
        out[i * stride][0] = sr;
        out[i * stride][1] = si;
    }
}

static void hybrid2_re(float (*in)[2], float (*out)[32][2], int len, int reverse) {
    static const float g1_Q2[7] = { 0.0f, 0.01899487526049f, 0.0f, -0.07293139167538f, 0.0f, 0.30596630545168f, 0.5f };
    for (int i = 0; i < len; i++, in++) {
        float re_in = g1_Q2[6] * in[6][0], im_in = g1_Q2[6] * in[6][1];
        float re_op = 0, im_op = 0;
        for (int j = 0; j < 6; j += 2) {
            re_op += g1_Q2[j + 1] * (in[j + 1][0] + in[12 - j - 1][0]);
            im_op += g1_Q2[j + 1] * (in[j + 1][1] + in[12 - j - 1][1]);
        }
        out[reverse][i][0] = re_in + re_op;
        out[reverse][i][1] = im_in + im_op;
        out[!reverse][i][0] = re_in - re_op;
        out[!reverse][i][1] = im_in - im_op;
    }
}

static void hybrid_analysis(struct aac_ps *p, float out[MAX_SSB][32][2], float L[2][38][64], int is34) {
    float (*in)[44][2] = p->in_buf;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 38; j++) {
            in[i][j + 6][0] = L[0][j][i];
            in[i][j + 6][1] = L[1][j][i];
        }
    if (is34) {
        for (int i = 0; i < 32; i++) {
            hybrid_filter(out[0] + i, in[0] + i, (const float (*)[8][2])f34_0_12, 32, 12);
            hybrid_filter(out[12] + i, in[1] + i, (const float (*)[8][2])f34_1_8, 32, 8);
            hybrid_filter(out[20] + i, in[2] + i, (const float (*)[8][2])f34_2_4, 32, 4);
            hybrid_filter(out[24] + i, in[3] + i, (const float (*)[8][2])f34_2_4, 32, 4);
            hybrid_filter(out[28] + i, in[4] + i, (const float (*)[8][2])f34_2_4, 32, 4);
        }
        for (int i = 5; i < 64; i++)
            for (int j = 0; j < 32; j++) { out[27 + i][j][0] = L[0][j][i]; out[27 + i][j][1] = L[1][j][i]; }
    } else {
        for (int i = 0; i < 32; i++) {
            float t[8][2];
            hybrid_filter(t, in[0] + i, (const float (*)[8][2])f20_0_8, 1, 8);
            out[0][i][0] = t[6][0]; out[0][i][1] = t[6][1];
            out[1][i][0] = t[7][0]; out[1][i][1] = t[7][1];
            out[2][i][0] = t[0][0]; out[2][i][1] = t[0][1];
            out[3][i][0] = t[1][0]; out[3][i][1] = t[1][1];
            out[4][i][0] = t[2][0] + t[5][0]; out[4][i][1] = t[2][1] + t[5][1];
            out[5][i][0] = t[3][0] + t[4][0]; out[5][i][1] = t[3][1] + t[4][1];
        }
        hybrid2_re(in[1], out + 6, 32, 1);
        hybrid2_re(in[2], out + 8, 32, 0);
        for (int i = 3; i < 64; i++)
            for (int j = 0; j < 32; j++) { out[7 + i][j][0] = L[0][j][i]; out[7 + i][j][1] = L[1][j][i]; }
    }
    for (int i = 0; i < 5; i++) memcpy(in[i], in[i] + 32, 6 * sizeof in[i][0]);
}

static void hybrid_synthesis(float out[2][38][64], float in[MAX_SSB][32][2], int is34) {
    if (is34) {
        for (int n = 0; n < 32; n++) {
            for (int c = 0; c < 5; c++) out[0][n][c] = out[1][n][c] = 0;
            for (int i = 0; i < 12; i++) { out[0][n][0] += in[i][n][0]; out[1][n][0] += in[i][n][1]; }
            for (int i = 0; i < 8; i++) { out[0][n][1] += in[12 + i][n][0]; out[1][n][1] += in[12 + i][n][1]; }
            for (int i = 0; i < 4; i++) {
                out[0][n][2] += in[20 + i][n][0]; out[1][n][2] += in[20 + i][n][1];
                out[0][n][3] += in[24 + i][n][0]; out[1][n][3] += in[24 + i][n][1];
                out[0][n][4] += in[28 + i][n][0]; out[1][n][4] += in[28 + i][n][1];
            }
        }
        for (int i = 5; i < 64; i++)
            for (int n = 0; n < 32; n++) { out[0][n][i] = in[27 + i][n][0]; out[1][n][i] = in[27 + i][n][1]; }
    } else {
        for (int n = 0; n < 32; n++) {
            out[0][n][0] = in[0][n][0] + in[1][n][0] + in[2][n][0] + in[3][n][0] + in[4][n][0] + in[5][n][0];
            out[1][n][0] = in[0][n][1] + in[1][n][1] + in[2][n][1] + in[3][n][1] + in[4][n][1] + in[5][n][1];
            out[0][n][1] = in[6][n][0] + in[7][n][0];
            out[1][n][1] = in[6][n][1] + in[7][n][1];
            out[0][n][2] = in[8][n][0] + in[9][n][0];
            out[1][n][2] = in[8][n][1] + in[9][n][1];
        }
        for (int i = 3; i < 64; i++)
            for (int n = 0; n < 32; n++) { out[0][n][i] = in[7 + i][n][0]; out[1][n][i] = in[7 + i][n][1]; }
    }
}

/* ---------------------------------------------------------------- decorrelation */
static void decorrelate(float (*out)[2], float (*delay)[2], float (*ap_delay)[SLOTS + MAX_AP_DELAY][2],
                        const float phi[2], const float (*Q)[2], const float *tg, float g_decay_slope) {
    static const float a[3] = { 0.65143905753106f, 0.56471812200776f, 0.48954165955695f };
    float ag[3];
    for (int m = 0; m < 3; m++) ag[m] = a[m] * g_decay_slope;
    for (int n = 0; n < 32; n++) {
        float in_re = delay[n][0] * phi[0] - delay[n][1] * phi[1];
        float in_im = delay[n][0] * phi[1] + delay[n][1] * phi[0];
        for (int m = 0; m < 3; m++) {
            float a_re = ag[m] * in_re, a_im = ag[m] * in_im;
            float ld_re = ap_delay[m][n + 2 - m][0], ld_im = ap_delay[m][n + 2 - m][1];
            float apd_re = in_re, apd_im = in_im;
            in_re = ld_re * Q[m][0] - ld_im * Q[m][1];
            in_re -= a_re;
            in_im = ld_re * Q[m][1] + ld_im * Q[m][0];
            in_im -= a_im;
            ap_delay[m][n + 5][0] = apd_re + ag[m] * in_re;
            ap_delay[m][n + 5][1] = apd_im + ag[m] * in_im;
        }
        out[n][0] = tg[n] * in_re;
        out[n][1] = tg[n] * in_im;
    }
}

static void decorrelation(struct aac_ps *p, float (*out)[32][2], float (*s)[32][2], int is34) {
    float power[34][32], tgain[34][32];
    const int8_t *k_to_i = is34 ? aac_ps_k_to_i_34 : aac_ps_k_to_i_20;
    const float peak_decay_factor = 0.76592833836465f, a_smooth = 0.25f, transient_impact = 1.5f;
    memset(power, 0, sizeof power);
    if (is34 != p->is34_old) {
        memset(p->peak_decay_nrg, 0, sizeof p->peak_decay_nrg);
        memset(p->power_smooth, 0, sizeof p->power_smooth);
        memset(p->peak_decay_diff_smooth, 0, sizeof p->peak_decay_diff_smooth);
        memset(p->delay, 0, sizeof p->delay);
        memset(p->ap_delay, 0, sizeof p->ap_delay);
    }
    for (int k = 0; k < NR_BANDS[is34]; k++) {
        int i = k_to_i[k];
        for (int n = 0; n < 32; n++) power[i][n] += s[k][n][0] * s[k][n][0] + s[k][n][1] * s[k][n][1];
    }
    for (int i = 0; i < NR_PAR_BANDS[is34]; i++)
        for (int n = 0; n < 32; n++) {
            float dp = peak_decay_factor * p->peak_decay_nrg[i];
            p->peak_decay_nrg[i] = dp > power[i][n] ? dp : power[i][n];
            p->power_smooth[i] += a_smooth * (power[i][n] - p->power_smooth[i]);
            p->peak_decay_diff_smooth[i] += a_smooth * (p->peak_decay_nrg[i] - power[i][n] - p->peak_decay_diff_smooth[i]);
            float denom = transient_impact * p->peak_decay_diff_smooth[i];
            tgain[i][n] = denom > p->power_smooth[i] ? p->power_smooth[i] / denom : 1.0f;
        }
    int k;
    for (k = 0; k < NR_ALLPASS_BANDS[is34]; k++) {
        int b = k_to_i[k];
        float gds = 1.f - 0.05f * (float)(k - DECAY_CUTOFF[is34]);
        gds = gds < 0 ? 0 : gds > 1 ? 1 : gds;
        memcpy(p->delay[k], p->delay[k] + 32, MAX_DELAY * sizeof p->delay[k][0]);
        memcpy(p->delay[k] + MAX_DELAY, s[k], 32 * sizeof p->delay[k][0]);
        for (int m = 0; m < AP_LINKS; m++)
            memcpy(p->ap_delay[k][m], p->ap_delay[k][m] + 32, 5 * sizeof p->ap_delay[k][m][0]);
        decorrelate(out[k], p->delay[k] + MAX_DELAY - 2, p->ap_delay[k], phi_fract[is34][k],
                    (const float (*)[2])Q_fract_allpass[is34][k], tgain[b], gds);
    }
    for (; k < NR_BANDS[is34]; k++) {
        int i = k_to_i[k];
        int d = k < SHORT_DELAY_BAND[is34] ? 14 : 1;
        memcpy(p->delay[k], p->delay[k] + 32, MAX_DELAY * sizeof p->delay[k][0]);
        memcpy(p->delay[k] + MAX_DELAY, s[k], 32 * sizeof p->delay[k][0]);
        float (*src)[2] = p->delay[k] + MAX_DELAY - d;
        for (int n = 0; n < 32; n++) {
            out[k][n][0] = src[n][0] * tgain[i][n];
            out[k][n][1] = src[n][1] * tgain[i][n];
        }
    }
}

/* ---------------------------------------------------------------- parameter mapping */
static void map_idx_10_to_20(int8_t *pm, const int8_t *par, int full) {
    int b;
    if (full) b = 9;
    else { b = 4; pm[10] = 0; }
    for (; b >= 0; b--) pm[2 * b + 1] = pm[2 * b] = par[b];
}

static void map_idx_34_to_20(int8_t *pm, const int8_t *par, int full) {
    pm[0] = (int8_t)((2 * par[0] + par[1]) / 3);
    pm[1] = (int8_t)((par[1] + 2 * par[2]) / 3);
    pm[2] = (int8_t)((2 * par[3] + par[4]) / 3);
    pm[3] = (int8_t)((par[4] + 2 * par[5]) / 3);
    pm[4] = (int8_t)((par[6] + par[7]) / 2);
    pm[5] = (int8_t)((par[8] + par[9]) / 2);
    pm[6] = par[10];
    pm[7] = par[11];
    pm[8] = (int8_t)((par[12] + par[13]) / 2);
    pm[9] = (int8_t)((par[14] + par[15]) / 2);
    pm[10] = par[16];
    if (full) {
        pm[11] = par[17];
        pm[12] = par[18];
        pm[13] = par[19];
        pm[14] = (int8_t)((par[20] + par[21]) / 2);
        pm[15] = (int8_t)((par[22] + par[23]) / 2);
        pm[16] = (int8_t)((par[24] + par[25]) / 2);
        pm[17] = (int8_t)((par[26] + par[27]) / 2);
        pm[18] = (int8_t)((par[28] + par[29] + par[30] + par[31]) / 4);
        pm[19] = (int8_t)((par[32] + par[33]) / 2);
    }
}

static void map_val_34_to_20(float *par) {
    par[0] = (2 * par[0] + par[1]) * 0.33333333f;
    par[1] = (par[1] + 2 * par[2]) * 0.33333333f;
    par[2] = (2 * par[3] + par[4]) * 0.33333333f;
    par[3] = (par[4] + 2 * par[5]) * 0.33333333f;
    par[4] = (par[6] + par[7]) * 0.5f;
    par[5] = (par[8] + par[9]) * 0.5f;
    par[6] = par[10];
    par[7] = par[11];
    par[8] = (par[12] + par[13]) * 0.5f;
    par[9] = (par[14] + par[15]) * 0.5f;
    par[10] = par[16];
    par[11] = par[17];
    par[12] = par[18];
    par[13] = par[19];
    par[14] = (par[20] + par[21]) * 0.5f;
    par[15] = (par[22] + par[23]) * 0.5f;
    par[16] = (par[24] + par[25]) * 0.5f;
    par[17] = (par[26] + par[27]) * 0.5f;
    par[18] = (par[28] + par[29] + par[30] + par[31]) * 0.25f;
    par[19] = (par[32] + par[33]) * 0.5f;
}

static void map_idx_10_to_34(int8_t *pm, const int8_t *par, int full) {
    static const int8_t src[34] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9 };
    for (int i = 0; i < 34; i++) {
        if (i >= 16 && !full) { if (i == 16) pm[16] = 0; continue; }
        pm[i] = par[src[i]];
    }
}

static void map_idx_20_to_34(int8_t *pm, const int8_t *par, int full) {
    if (full) {
        static const int8_t src[34] = { 0, 0, 1, 2, 0, 3, 4, 4, 5, 5, 6, 7, 8, 8, 9, 9, 10, 11, 12, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 18, 18, 19, 19 };
        for (int i = 17; i < 34; i++) pm[i] = par[src[i]];
    }
    pm[16] = par[10];
    pm[15] = par[9];
    pm[14] = par[9];
    pm[13] = par[8];
    pm[12] = par[8];
    pm[11] = par[7];
    pm[10] = par[6];
    pm[9] = par[5];
    pm[8] = par[5];
    pm[7] = par[4];
    pm[6] = par[4];
    pm[5] = par[3];
    pm[4] = (int8_t)((par[2] + par[3]) / 2);
    pm[3] = par[2];
    pm[2] = par[1];
    pm[1] = (int8_t)((par[0] + par[1]) / 2);
    pm[0] = par[0];
}

static void map_val_20_to_34(float *par) {
    par[33] = par[19];
    par[32] = par[19];
    par[31] = par[18];
    par[30] = par[18];
    par[29] = par[18];
    par[28] = par[18];
    par[27] = par[17];
    par[26] = par[17];
    par[25] = par[16];
    par[24] = par[16];
    par[23] = par[15];
    par[22] = par[15];
    par[21] = par[14];
    par[20] = par[14];
    par[19] = par[13];
    par[18] = par[12];
    par[17] = par[11];
    par[16] = par[10];
    par[15] = par[9];
    par[14] = par[9];
    par[13] = par[8];
    par[12] = par[8];
    par[11] = par[7];
    par[10] = par[6];
    par[9] = par[5];
    par[8] = par[5];
    par[7] = par[4];
    par[6] = par[4];
    par[5] = par[3];
    par[4] = (par[2] + par[3]) * 0.5f;
    par[3] = par[2];
    par[2] = par[1];
    par[1] = (par[0] + par[1]) * 0.5f;
}

/* returns the parameter rows to use (remapped into buf, or par itself) */
static int8_t (*remap(int8_t (*buf)[MAX_PAR], int8_t (*par)[MAX_PAR], int num_par, int num_env, int full, int is34))[MAX_PAR] {
    if (is34) {
        if (num_par == 20 || num_par == 11) {
            for (int e = 0; e < num_env; e++) map_idx_20_to_34(buf[e], par[e], full);
            return buf;
        }
        if (num_par == 10 || num_par == 5) {
            for (int e = 0; e < num_env; e++) map_idx_10_to_34(buf[e], par[e], full);
            return buf;
        }
        return par;
    }
    if (num_par == 34 || num_par == 17) {
        for (int e = 0; e < num_env; e++) map_idx_34_to_20(buf[e], par[e], full);
        return buf;
    }
    if (num_par == 10 || num_par == 5) {
        for (int e = 0; e < num_env; e++) map_idx_10_to_20(buf[e], par[e], full);
        return buf;
    }
    return par;
}

static void stereo_processing(struct aac_ps *p, float (*l)[32][2], float (*r)[32][2], int is34) {
    float (*H11)[MAX_ENV + 1][MAX_PAR] = p->H11, (*H12)[MAX_ENV + 1][MAX_PAR] = p->H12;
    float (*H21)[MAX_ENV + 1][MAX_PAR] = p->H21, (*H22)[MAX_ENV + 1][MAX_PAR] = p->H22;
    int8_t iid_buf[MAX_ENV][MAX_PAR], icc_buf[MAX_ENV][MAX_PAR], ipd_buf[MAX_ENV][MAX_PAR], opd_buf[MAX_ENV][MAX_PAR];
    int8_t (*iid)[MAX_PAR], (*icc)[MAX_PAR], (*ipd)[MAX_PAR] = ipd_buf, (*opd)[MAX_PAR] = opd_buf;
    memset(ipd_buf, 0, sizeof ipd_buf);
    memset(opd_buf, 0, sizeof opd_buf);
    const int8_t *k_to_i = is34 ? aac_ps_k_to_i_34 : aac_ps_k_to_i_20;
    float (*H_LUT)[8][4] = p->icc_mode < 3 ? HA : HB;
    if (p->num_env_old) {
        for (int c = 0; c < 2; c++) {
            memcpy(H11[c][0], H11[c][p->num_env_old], sizeof H11[c][0]);
            memcpy(H12[c][0], H12[c][p->num_env_old], sizeof H12[c][0]);
            memcpy(H21[c][0], H21[c][p->num_env_old], sizeof H21[c][0]);
            memcpy(H22[c][0], H22[c][p->num_env_old], sizeof H22[c][0]);
        }
    }
    iid = remap(iid_buf, p->iid_par, p->nr_iid_par, p->num_env, 1, is34);
    icc = remap(icc_buf, p->icc_par, p->nr_icc_par, p->num_env, 1, is34);
    if (p->enable_ipdopd) {
        ipd = remap(ipd_buf, p->ipd_par, p->nr_ipdopd_par, p->num_env, 0, is34);
        opd = remap(opd_buf, p->opd_par, p->nr_ipdopd_par, p->num_env, 0, is34);
    }
    if (is34 && !p->is34_old) {
        for (int c = 0; c < 2; c++) {
            map_val_20_to_34(H11[c][0]);
            map_val_20_to_34(H12[c][0]);
            map_val_20_to_34(H21[c][0]);
            map_val_20_to_34(H22[c][0]);
        }
        memset(p->ipd_hist, 0, sizeof p->ipd_hist);
        memset(p->opd_hist, 0, sizeof p->opd_hist);
    } else if (!is34 && p->is34_old) {
        for (int c = 0; c < 2; c++) {
            map_val_34_to_20(H11[c][0]);
            map_val_34_to_20(H12[c][0]);
            map_val_34_to_20(H21[c][0]);
            map_val_34_to_20(H22[c][0]);
        }
        memset(p->ipd_hist, 0, sizeof p->ipd_hist);
        memset(p->opd_hist, 0, sizeof p->opd_hist);
    }
    for (int e = 0; e < p->num_env; e++) {
        for (int b = 0; b < NR_PAR_BANDS[is34]; b++) {
            const float *h = H_LUT[iid[e][b] + 7 + 23 * p->iid_quant][icc[e][b]];
            float h11 = h[0], h12 = h[1], h21 = h[2], h22 = h[3];
            if (p->enable_ipdopd && b < NR_IPDOPD_BANDS[is34]) {
                int opd_idx = p->opd_hist[b] * 8 + opd[e][b];
                int ipd_idx = p->ipd_hist[b] * 8 + ipd[e][b];
                float opd_re = pd_re_smooth[opd_idx], opd_im = pd_im_smooth[opd_idx];
                float ipd_re = pd_re_smooth[ipd_idx], ipd_im = pd_im_smooth[ipd_idx];
                p->opd_hist[b] = (int8_t)(opd_idx & 0x3f);
                p->ipd_hist[b] = (int8_t)(ipd_idx & 0x3f);
                float adj_re = opd_re * ipd_re + opd_im * ipd_im;
                float adj_im = opd_im * ipd_re - opd_re * ipd_im;
                H11[1][e + 1][b] = h11 * opd_im;
                H12[1][e + 1][b] = h12 * adj_im;
                H21[1][e + 1][b] = h21 * opd_im;
                H22[1][e + 1][b] = h22 * adj_im;
                h11 *= opd_re;
                h12 *= adj_re;
                h21 *= opd_re;
                h22 *= adj_re;
            }
            H11[0][e + 1][b] = h11;
            H12[0][e + 1][b] = h12;
            H21[0][e + 1][b] = h21;
            H22[0][e + 1][b] = h22;
        }
        for (int k = 0; k < NR_BANDS[is34]; k++) {
            float hh[2][4], hs[2][4];
            int start = p->border[e], stop = p->border[e + 1];
            float width = 1.f / (float)(stop - start ? stop - start : 1);
            int b = k_to_i[k];
            hh[0][0] = H11[0][e][b];
            hh[0][1] = H12[0][e][b];
            hh[0][2] = H21[0][e][b];
            hh[0][3] = H22[0][e][b];
            if (p->enable_ipdopd) {
                float sg = (is34 && k <= 13 && k >= 9) || (!is34 && k <= 1) ? -1.0f : 1.0f;
                hh[1][0] = sg * H11[1][e][b];
                hh[1][1] = sg * H12[1][e][b];
                hh[1][2] = sg * H21[1][e][b];
                hh[1][3] = sg * H22[1][e][b];
            }
            hs[0][0] = (H11[0][e + 1][b] - hh[0][0]) * width;
            hs[0][1] = (H12[0][e + 1][b] - hh[0][1]) * width;
            hs[0][2] = (H21[0][e + 1][b] - hh[0][2]) * width;
            hs[0][3] = (H22[0][e + 1][b] - hh[0][3]) * width;
            if (p->enable_ipdopd) {
                hs[1][0] = (H11[1][e + 1][b] - hh[1][0]) * width;
                hs[1][1] = (H12[1][e + 1][b] - hh[1][1]) * width;
                hs[1][2] = (H21[1][e + 1][b] - hh[1][2]) * width;
                hs[1][3] = (H22[1][e + 1][b] - hh[1][3]) * width;
            }
            if (stop - start <= 0) continue;
            float (*L)[2] = l[k] + 1 + start, (*R)[2] = r[k] + 1 + start;
            float h00 = hh[0][0], h01 = hh[0][1], h02 = hh[0][2], h03 = hh[0][3];
            if (!p->enable_ipdopd) {
                for (int n = 0; n < stop - start; n++) {
                    float lr = L[n][0], li = L[n][1], rr = R[n][0], ri = R[n][1];
                    h00 += hs[0][0]; h01 += hs[0][1]; h02 += hs[0][2]; h03 += hs[0][3];
                    L[n][0] = h00 * lr + h02 * rr;
                    L[n][1] = h00 * li + h02 * ri;
                    R[n][0] = h01 * lr + h03 * rr;
                    R[n][1] = h01 * li + h03 * ri;
                }
            } else {
                float h10 = hh[1][0], h11b = hh[1][1], h12b = hh[1][2], h13 = hh[1][3];
                for (int n = 0; n < stop - start; n++) {
                    float lr = L[n][0], li = L[n][1], rr = R[n][0], ri = R[n][1];
                    h00 += hs[0][0]; h01 += hs[0][1]; h02 += hs[0][2]; h03 += hs[0][3];
                    h10 += hs[1][0]; h11b += hs[1][1]; h12b += hs[1][2]; h13 += hs[1][3];
                    L[n][0] = h00 * lr + h02 * rr - h10 * li - h12b * ri;
                    L[n][1] = h00 * li + h02 * ri + h10 * lr + h12b * rr;
                    R[n][0] = h01 * lr + h03 * rr - h11b * li - h13 * ri;
                    R[n][1] = h01 * li + h03 * ri + h11b * lr + h13 * rr;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- API */
struct aac_ps *aac_ps_new(void) {
    if (!ps_init_tables()) return 0;
    return calloc(1, sizeof(struct aac_ps));
}

void aac_ps_free(struct aac_ps *p) { free(p); }

void aac_ps_reset(struct aac_ps *p) {
    if (!p) return;
    memset(p->in_buf, 0, sizeof p->in_buf);
    memset(p->delay, 0, sizeof p->delay);
    memset(p->ap_delay, 0, sizeof p->ap_delay);
    memset(p->peak_decay_nrg, 0, sizeof p->peak_decay_nrg);
    memset(p->power_smooth, 0, sizeof p->power_smooth);
    memset(p->peak_decay_diff_smooth, 0, sizeof p->peak_decay_diff_smooth);
}

int aac_ps_started(struct aac_ps *p) { return p && p->start; }

void aac_ps_apply(struct aac_ps *p, float L[2][38][64], float R[2][38][64], int top) {
    int is34 = p->is34;
    top += NR_BANDS[is34] - 64;
    if (top < 0) top = 0;
    if (top < NR_BANDS[is34]) memset(p->delay + top, 0, (size_t)(NR_BANDS[is34] - top) * sizeof p->delay[0]);
    if (top < NR_ALLPASS_BANDS[is34]) memset(p->ap_delay + top, 0, (size_t)(NR_ALLPASS_BANDS[is34] - top) * sizeof p->ap_delay[0]);
    hybrid_analysis(p, p->Lbuf, L, is34);
    decorrelation(p, p->Rbuf, p->Lbuf, is34);
    stereo_processing(p, p->Lbuf, p->Rbuf, is34);
    hybrid_synthesis(L, p->Lbuf, is34);
    hybrid_synthesis(R, p->Rbuf, is34);
}
