/* AXIOM H.264 decoder: intra prediction (8.3). Unavailable neighbours read as 128 / substituted per spec. */
#include "h264_int.h"

#define F2(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)
#define F1(a, b) (((a) + (b) + 1) >> 1)

/* Directional prediction shared by 4x4 and 8x8: T[-1..2N-1] top row (T[-1] = top-left), L[0..N-1] left. */
static void pred_dir(uint8_t *dst, int stride, int mode, const int *T, const int *L, int N) {
    int x, y;
#define P(xx, yy) dst[(yy) * stride + (xx)]
#define LL(i) ((i) < 0 ? T[-1] : L[i])
    switch (mode) {
    case 3: /* diagonal down left */
        for (y = 0; y < N; y++) for (x = 0; x < N; x++)
            P(x, y) = (x == N - 1 && y == N - 1) ? (T[2 * N - 2] + 3 * T[2 * N - 1] + 2) >> 2
                                                 : F2(T[x + y], T[x + y + 1], T[x + y + 2]);
        break;
    case 4: /* diagonal down right */
        for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
            if (x > y) P(x, y) = F2(T[x - y - 2], T[x - y - 1], T[x - y]);
            else if (x < y) P(x, y) = F2(LL(y - x - 2), LL(y - x - 1), LL(y - x));
            else P(x, y) = F2(T[0], T[-1], L[0]);
        }
        break;
    case 5: /* vertical right */
        for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
            int z = 2 * x - y;
            if (z >= 0 && !(z & 1)) P(x, y) = F1(T[x - (y >> 1) - 1], T[x - (y >> 1)]);
            else if (z >= 0) P(x, y) = F2(T[x - (y >> 1) - 2], T[x - (y >> 1) - 1], T[x - (y >> 1)]);
            else if (z == -1) P(x, y) = F2(L[0], T[-1], T[0]);
            else P(x, y) = F2(LL(y - 2 * x - 1), LL(y - 2 * x - 2), LL(y - 2 * x - 3));
        }
        break;
    case 6: /* horizontal down */
        for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
            int z = 2 * y - x;
            if (z >= 0 && !(z & 1)) P(x, y) = F1(LL(y - (x >> 1) - 1), LL(y - (x >> 1)));
            else if (z >= 0) P(x, y) = F2(LL(y - (x >> 1) - 2), LL(y - (x >> 1) - 1), LL(y - (x >> 1)));
            else if (z == -1) P(x, y) = F2(L[0], T[-1], T[0]);
            else P(x, y) = F2(T[x - 2 * y - 1], T[x - 2 * y - 2], T[x - 2 * y - 3]);
        }
        break;
    case 7: /* vertical left */
        for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
            int i = x + (y >> 1);
            P(x, y) = (y & 1) ? F2(T[i], T[i + 1], T[i + 2]) : F1(T[i], T[i + 1]);
        }
        break;
    case 8: /* horizontal up */
        for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
            int z = x + 2 * y, i = y + (x >> 1);
            if (z > 2 * N - 3) P(x, y) = L[N - 1];
            else if (z == 2 * N - 3) P(x, y) = (L[N - 2] + 3 * L[N - 1] + 2) >> 2;
            else if (z & 1) P(x, y) = F2(L[i], L[i + 1], L[i + 2]);
            else P(x, y) = F1(L[i], L[i + 1]);
        }
        break;
    }
#undef P
#undef LL
}

void h264_intra4x4(uint8_t *dst, int stride, int mode, int al, int at, int atr, int atl) {
    int Tb[9], L[4];
    int *T = Tb + 1;
    const uint8_t *top = dst - stride;
    if (at) {
        for (int i = 0; i < 4; i++) T[i] = top[i];
        if (atr) for (int i = 4; i < 8; i++) T[i] = top[i];
        else for (int i = 4; i < 8; i++) T[i] = top[3];
    } else for (int i = 0; i < 8; i++) T[i] = 128;
    if (al) for (int i = 0; i < 4; i++) L[i] = dst[i * stride - 1];
    else for (int i = 0; i < 4; i++) L[i] = 128;
    T[-1] = atl ? top[-1] : 128;
    switch (mode) {
    case 0:
        for (int y = 0; y < 4; y++) WR32(dst + y * stride, at ? RD32(top) : 0x80808080u);
        return;
    case 1:
        for (int y = 0; y < 4; y++) WR32(dst + y * stride, (uint32_t)L[y] * 0x01010101u);
        return;
    case 2: {
        int s;
        if (al && at) s = (T[0] + T[1] + T[2] + T[3] + L[0] + L[1] + L[2] + L[3] + 4) >> 3;
        else if (al) s = (L[0] + L[1] + L[2] + L[3] + 2) >> 2;
        else if (at) s = (T[0] + T[1] + T[2] + T[3] + 2) >> 2;
        else s = 128;
        for (int y = 0; y < 4; y++) WR32(dst + y * stride, (uint32_t)s * 0x01010101u);
        return;
    }
    default:
        if (mode > 8) mode = 2;
        pred_dir(dst, stride, mode, T, L, 4);
    }
}

void h264_intra8x8(uint8_t *dst, int stride, int mode, int al, int at, int atr, int atl) {
    int p[16], q[8], tl = 128;      /* raw top (incl. top-right), left */
    int Tb[17], L[8];
    int *T = Tb + 1;
    const uint8_t *top = dst - stride;
    if (at) {
        for (int i = 0; i < 8; i++) p[i] = top[i];
        if (atr) for (int i = 8; i < 16; i++) p[i] = top[i];
        else for (int i = 8; i < 16; i++) p[i] = top[7];
    }
    if (al) for (int i = 0; i < 8; i++) q[i] = dst[i * stride - 1];
    if (atl) tl = top[-1];
    /* reference sample filtering (8.3.2.2.1) */
    if (at) {
        T[0] = atl ? F2(tl, p[0], p[1]) : (3 * p[0] + p[1] + 2) >> 2;
        for (int i = 1; i < 15; i++) T[i] = F2(p[i - 1], p[i], p[i + 1]);
        T[15] = (p[14] + 3 * p[15] + 2) >> 2;
    } else for (int i = 0; i < 16; i++) T[i] = 128;
    if (atl) {
        if (at && al) T[-1] = F2(p[0], tl, q[0]);
        else if (at) T[-1] = (3 * tl + p[0] + 2) >> 2;
        else if (al) T[-1] = (3 * tl + q[0] + 2) >> 2;
        else T[-1] = tl;
    } else T[-1] = 128;
    if (al) {
        L[0] = atl ? F2(tl, q[0], q[1]) : (3 * q[0] + q[1] + 2) >> 2;
        for (int i = 1; i < 7; i++) L[i] = F2(q[i - 1], q[i], q[i + 1]);
        L[7] = (q[6] + 3 * q[7] + 2) >> 2;
    } else for (int i = 0; i < 8; i++) L[i] = 128;
    switch (mode) {
    case 0: {
        uint8_t row[8];
        for (int i = 0; i < 8; i++) row[i] = (uint8_t)T[i];
        for (int y = 0; y < 8; y++) memcpy(dst + y * stride, row, 8);
        return;
    }
    case 1:
        for (int y = 0; y < 8; y++) WR64(dst + y * stride, (uint64_t)L[y] * 0x0101010101010101ull);
        return;
    case 2: {
        int s = 0;
        if (al && at) { for (int i = 0; i < 8; i++) s += T[i] + L[i]; s = (s + 8) >> 4; }
        else if (al) { for (int i = 0; i < 8; i++) s += L[i]; s = (s + 4) >> 3; }
        else if (at) { for (int i = 0; i < 8; i++) s += T[i]; s = (s + 4) >> 3; }
        else s = 128;
        for (int y = 0; y < 8; y++) WR64(dst + y * stride, (uint64_t)s * 0x0101010101010101ull);
        return;
    }
    default:
        if (mode > 8) mode = 2;
        pred_dir(dst, stride, mode, T, L, 8);
    }
}

void h264_intra16x16(uint8_t *dst, int stride, int mode, int al, int at, int atl) {
    const uint8_t *top = dst - stride;
    switch (mode) {
    case 0:
        for (int y = 0; y < 16; y++) {
            if (at) { WR64(dst + y * stride, RD64(top)); WR64(dst + y * stride + 8, RD64(top + 8)); }
            else memset(dst + y * stride, 128, 16);
        }
        return;
    case 1:
        for (int y = 0; y < 16; y++) memset(dst + y * stride, al ? dst[y * stride - 1] : 128, 16);
        return;
    case 3: {
        if (!(al && at && atl)) break;   /* invalid stream: fall back to DC */
        int H = 0, V = 0;
        for (int i = 0; i < 8; i++) {
            H += (i + 1) * (top[8 + i] - top[6 - i]);
            V += (i + 1) * (dst[(8 + i) * stride - 1] - dst[(6 - i) * stride - 1]);
        }
        int a = 16 * (dst[15 * stride - 1] + top[15]);
        int b = (5 * H + 32) >> 6, c = (5 * V + 32) >> 6;
        for (int y = 0; y < 16; y++) {
            int v = a + c * (y - 7) - 7 * b + 16;
            for (int x = 0; x < 16; x++, v += b) dst[y * stride + x] = h264_clip8(v >> 5);
        }
        return;
    }
    default: break;
    }
    int s = 0;
    if (al && at) {
        for (int i = 0; i < 16; i++) s += top[i] + dst[i * stride - 1];
        s = (s + 16) >> 5;
    } else if (al) {
        for (int i = 0; i < 16; i++) s += dst[i * stride - 1];
        s = (s + 8) >> 4;
    } else if (at) {
        for (int i = 0; i < 16; i++) s += top[i];
        s = (s + 8) >> 4;
    } else s = 128;
    for (int y = 0; y < 16; y++) memset(dst + y * stride, s, 16);
}

void h264_intra_chroma(uint8_t *dst, int stride, int mode, int al, int at, int atl) {
    const uint8_t *top = dst - stride;
    switch (mode) {
    case 1:
        for (int y = 0; y < 8; y++) WR64(dst + y * stride, (al ? dst[y * stride - 1] : 128) * 0x0101010101010101ull);
        return;
    case 2:
        for (int y = 0; y < 8; y++) WR64(dst + y * stride, at ? RD64(top) : 0x8080808080808080ull);
        return;
    case 3: {
        if (!(al && at && atl)) break;
        int H = 0, V = 0;
        for (int i = 0; i < 4; i++) {
            H += (i + 1) * (top[4 + i] - top[2 - i]);
            V += (i + 1) * (dst[(4 + i) * stride - 1] - dst[(2 - i) * stride - 1]);
        }
        int a = 16 * (dst[7 * stride - 1] + top[7]);
        int b = (34 * H + 32) >> 6, c = (34 * V + 32) >> 6;
        for (int y = 0; y < 8; y++) {
            int v = a + c * (y - 3) - 3 * b + 16;
            for (int x = 0; x < 8; x++, v += b) dst[y * stride + x] = h264_clip8(v >> 5);
        }
        return;
    }
    default: break;
    }
    /* DC per 4x4 chroma block */
    for (int by = 0; by < 2; by++)
        for (int bx = 0; bx < 2; bx++) {
            int st = 0, sl = 0, s;
            if (at) for (int i = 0; i < 4; i++) st += top[bx * 4 + i];
            if (al) for (int i = 0; i < 4; i++) sl += dst[(by * 4 + i) * stride - 1];
            if ((bx == 0 && by == 0) || (bx == 1 && by == 1)) {
                if (al && at) s = (st + sl + 4) >> 3;
                else if (al) s = (sl + 2) >> 2;
                else if (at) s = (st + 2) >> 2;
                else s = 128;
            } else if (bx == 1) {
                if (at) s = (st + 2) >> 2;
                else if (al) s = (sl + 2) >> 2;
                else s = 128;
            } else {
                if (al) s = (sl + 2) >> 2;
                else if (at) s = (st + 2) >> 2;
                else s = 128;
            }
            for (int y = 0; y < 4; y++) WR32(dst + (by * 4 + y) * stride + bx * 4, (uint32_t)s * 0x01010101u);
        }
}
