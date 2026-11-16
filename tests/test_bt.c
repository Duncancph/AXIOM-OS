/* Host-side test for the Bluetooth driver: a fake USB Bluetooth controller that
 * answers HCI commands and streams events back in 16-byte USB packets. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include "../src/usb.h"

static usb_xfer_cb evt_cb; static struct usb_dev dev; static uint8_t epbuf[64];
static uint8_t pending[1024][260]; static int np, rp; static int sent_cmds[4096], ncmds;
static int justworks; static uint8_t peer[6]; static int iocap = -1, authreq = -1, used_stored_key, declined, answer = 1;

static void q(const uint8_t *e, int n) { memcpy(pending[np++], e, n); }
static void cc(uint16_t op, const uint8_t *ret, int n) {
    uint8_t e[260] = {0x0E, (uint8_t)(3 + n), 1, op & 0xFF, op >> 8};
    memcpy(e + 5, ret, n); q(e, 5 + n);
}
static void cs(uint16_t op) { uint8_t e[] = {0x0F, 4, 0, 1, op & 0xFF, op >> 8}; q(e, 6); }

int usb_control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data) {
    uint8_t *p = data; uint16_t op = p[0] | p[1] << 8;
    sent_cmds[ncmds++] = op;
    if (rt != 0x20 || req != 0 || p[2] != len - 3) { printf("FAIL: bad command framing\n"); return -1; }
    switch (op) {
    case 0x1001: { uint8_t r[] = {0, 9, 0x34, 0x12, 9, 93, 0, 0x61, 0x87}; cc(op, r, sizeof r); break; }
    case 0x1009: { uint8_t r[] = {0, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}; cc(op, r, sizeof r); break; }
    case 0x0C14: { uint8_t r[249] = {0}; strcpy((char *)r + 1, "TestAdapter"); cc(op, r, sizeof r); break; }
    case 0x1003: { uint8_t r[9] = {0, 0xFF, 0xFF, 0x8F, 0xFE, 0xDB, 0xFF, 0x5B, 0x87}; cc(op, r, sizeof r); break; }
    case 0x0401: {
        cs(op);
        /* Extended Inquiry Result: name "Test Phone", Phone class, RSSI -48 */
        uint8_t e[257] = {0x2F, 255, 1, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 1, 0, 0x0C, 0x02, 0x5A, 0x34, 0x12, (uint8_t)-48};
        const char *nm = "Test Phone"; e[17] = strlen(nm) + 1; e[18] = 0x09; memcpy(e + 19, nm, strlen(nm));
        q(e, 257);
        /* Inquiry Result with RSSI, no name (needs remote name request), Audio class */
        uint8_t r[] = {0x22, 15, 1, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 1, 0, 0x04, 0x04, 0x24, 0x00, 0x10, (uint8_t)-70};
        q(r, sizeof r);
        /* a keyboard (major class 5, minor 0x40) */
        uint8_t kb[] = {0x22, 15, 1, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 1, 0, 0x40, 0x25, 0x00, 0x10, (uint8_t)-55};
        q(kb, sizeof kb);
        /* LE advertising report: name "Fitness Band", RSSI -60 */
        uint8_t le[64] = {0x3E, 0, 0x02, 1, 0x00, 0x01, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
        const char *ln = "Fitness Band"; int dl = 3 + 2 + strlen(ln);
        le[12] = dl; uint8_t *ad = le + 13;
        ad[0] = 2; ad[1] = 0x01; ad[2] = 0x06; ad[3] = strlen(ln) + 1; ad[4] = 0x09; memcpy(ad + 5, ln, strlen(ln));
        le[13 + dl] = (uint8_t)-60; le[1] = 13 + dl + 1 - 2;
        q(le, 13 + dl + 1);
        uint8_t done[] = {0x01, 1, 0}; q(done, 3);
        break;
    }
    /* ---- a phone that pairs with Secure Simple Pairing (numeric comparison) ---- */
    case 0x0405: {                                         /* Create_Connection -> Connection_Complete */
        cs(op);
        uint8_t e[] = {0x03, 11, 0, 0x42, 0x00, p[3], p[4], p[5], p[6], p[7], p[8], 1, 0}; q(e, sizeof e);
        memcpy(peer, p + 3, 6);
        break;
    }
    case 0x0411: { cs(op); uint8_t e[8] = {0x17, 6}; memcpy(e + 2, peer, 6); q(e, 8); break; }        /* -> Link_Key_Request */
    case 0x040C: {                                         /* no key -> IO_Capability_Request */
        uint8_t r[7] = {0}; memcpy(r + 1, peer, 6); cc(op, r, 7);
        uint8_t e[8] = {0x31, 6}; memcpy(e + 2, peer, 6); q(e, 8);
        break;
    }
    case 0x040B: {                                         /* stored key accepted -> Authentication_Complete */
        uint8_t r[7] = {0}; memcpy(r + 1, peer, 6); cc(op, r, 7);
        used_stored_key = 1;
        uint8_t e[] = {0x06, 3, 0, 0x42, 0x00}; q(e, sizeof e);
        break;
    }
    case 0x042B: {                                         /* our IO capabilities -> theirs, then the number */
        iocap = p[9]; authreq = p[11];
        uint8_t r[7] = {0}; memcpy(r + 1, peer, 6); cc(op, r, 7);
        uint8_t e1[11] = {0x32, 9}; memcpy(e1 + 2, peer, 6); e1[8] = justworks ? 3 : 1; e1[9] = 0; e1[10] = justworks ? 0 : 3; q(e1, 11);
        uint8_t e2[12] = {0x33, 10}; memcpy(e2 + 2, peer, 6); e2[8] = 0x40; e2[9] = 0xE2; e2[10] = 0x01; e2[11] = 0; q(e2, 12);   /* 123456 */
        break;
    }
    case 0x042C: {                                         /* confirmed -> complete, link key, authenticated */
        uint8_t r[7] = {0}; memcpy(r + 1, peer, 6); cc(op, r, 7);
        uint8_t e1[9] = {0x36, 7, 0}; memcpy(e1 + 3, peer, 6); q(e1, 9);
        uint8_t e2[25] = {0x18, 23}; memcpy(e2 + 2, peer, 6); for (int i = 0; i < 16; i++) e2[8 + i] = 0xA0 + i; e2[24] = 4; q(e2, 25);
        uint8_t e3[] = {0x06, 3, 0, 0x42, 0x00}; q(e3, sizeof e3);
        break;
    }
    case 0x042D: { uint8_t r[7] = {0}; memcpy(r + 1, peer, 6); cc(op, r, 7); declined = 1; break; }
    case 0x0406: { cs(op); uint8_t e[] = {0x05, 4, 0, 0x42, 0x00, 0x16}; q(e, sizeof e); break; }
    case 0x0419: {
        cs(op);
        uint8_t e[257] = {0x07, 255, 0}; memcpy(e + 3, p + 3, 6); strcpy((char *)e + 9, "Car Stereo"); q(e, 257);
        break;
    }
    default: cc(op, (uint8_t[]){0}, 1);
    }
    return 0;
}

int usb_queue_in(struct usb_dev *d, int dci, usb_xfer_cb cb) { evt_cb = cb; return 0; }
static int off;
void xhci_poll(void) {
    if (!evt_cb || rp == np) return;
    uint8_t *e = pending[rp]; int total = 2 + e[1];
    int n = total - off > 16 ? 16 : total - off;   /* 16-byte max packet, like real dongles */
    memcpy(epbuf, e + off, n); off += n;
    if (off >= total) { rp++; off = 0; }
    usb_xfer_cb cb = evt_cb; evt_cb = 0;
    cb(&dev, 3, n < 16 ? CC_SHORT : CC_SUCCESS, n);
}
uint64_t uptime_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int input_poll(void) { return -1; }
void kyield(void) {                                       /* the "user" answers the number question */
    struct bt_pair st; bt_pair_status(&st);
    if (st.state == BTP_CONFIRM) {
        if (st.code != 123456) { printf("FAIL: shown number %u, expected 123456\n", st.code); exit(1); }
        bt_pair_confirm(answer);
    }
}
static char keyfile[4096]; static long keylen;
long vfs_read(const char *p, void *b, uint64_t off, uint64_t len) { long n = keylen - (long)off; if (n < 0) n = 0; if ((uint64_t)n > len) n = len; memcpy(b, keyfile + off, n); return n; }
long vfs_write(const char *p, const void *b, uint64_t len) { memcpy(keyfile, b, len); keylen = len; return len; }
void task_exit(int c) { exit(c); }
void desktop_notify(const char *t, const char *m) {}
bool sched_running;
struct task *current;
void console_set_color(int c) {}
void kprintf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); }

/* what we sent over ACL (L2CAP), and what reached the keyboard and mouse */
static struct { uint16_t h, cid; uint8_t d[1200]; int n; } sent[1024]; static int nsent;
void bt_test_acl_sent(uint16_t h, uint16_t cid, const uint8_t *d, int n) {
    if (nsent < 1024) { sent[nsent].h = h; sent[nsent].cid = cid; memcpy(sent[nsent].d, d, n > 1200 ? 1200 : n); sent[nsent].n = n; nsent++; }
}
static void dev_l2(uint16_t h, uint16_t cid, const uint8_t *d, int n) {     /* the device sends an L2CAP packet */
    uint8_t b[300]; b[0] = h & 0xFF; b[1] = (h >> 8) | 0x20; b[2] = (n + 4) & 0xFF; b[3] = (n + 4) >> 8;
    b[4] = n & 0xFF; b[5] = n >> 8; b[6] = cid & 0xFF; b[7] = cid >> 8; memcpy(b + 8, d, n);
    bt_test_acl_in(b, 8 + n);
}
static int find_sig(int from, uint8_t code) { for (int i = from; i < nsent; i++) if (sent[i].cid == 1 && sent[i].d[0] == code) return i; return -1; }
/* the device side of setting up one channel that WE asked for (answer our Connection Request) */
static int dev_answer_conn(uint16_t h, int at, uint16_t their_cid) {
    int i = find_sig(at, 0x02); if (i < 0) return -1;
    uint16_t our = sent[i].d[6] | sent[i].d[7] << 8;
    uint8_t r[12] = {0x03, sent[i].d[1], 8, 0, their_cid & 0xFF, their_cid >> 8, our & 0xFF, our >> 8, 0, 0, 0, 0};
    int mark = nsent; dev_l2(h, 1, r, 12);
    int k = find_sig(mark, 0x04); if (k < 0) return -1;                    /* our Configuration Request */
    uint8_t cr[10] = {0x05, sent[k].d[1], 6, 0, our & 0xFF, our >> 8, 0, 0, 0, 0}; dev_l2(h, 1, cr, 10);
    uint8_t rq[8] = {0x04, 0x30, 4, 0, our & 0xFF, our >> 8, 0, 0}; dev_l2(h, 1, rq, 8);
    return our;
}
/* the device opens a channel to us */
static int dev_open(uint16_t h, uint16_t psm, uint16_t their_cid) {
    int mark = nsent;
    uint8_t rq[8] = {0x02, 0x20, 4, 0, psm & 0xFF, psm >> 8, their_cid & 0xFF, their_cid >> 8}; dev_l2(h, 1, rq, 8);
    int i = find_sig(mark, 0x03); if (i < 0) return -1;
    uint16_t our = sent[i].d[4] | sent[i].d[5] << 8;
    int k = find_sig(mark, 0x04); if (k < 0) return -1;
    uint8_t cr[10] = {0x05, sent[k].d[1], 6, 0, our & 0xFF, our >> 8, 0, 0, 0, 0}; dev_l2(h, 1, cr, 10);
    uint8_t c2[8] = {0x04, 0x21, 4, 0, our & 0xFF, our >> 8, 0, 0}; dev_l2(h, 1, c2, 8);
    return our;
}
static bool sent_cmd(int op) { for (int i = 0; i < ncmds; i++) if (sent_cmds[i] == op) return true; return false; }
int settings_int(const char *k) { return 100; }
void bt_test_audio_start(uint16_t h); int bt_test_audio_packet(void); int bt_test_audio_state(void);
bool bta_active(void); long bta_write(const int16_t *pcm, long frames, int in_rate);
static int last_on(uint16_t cid) { for (int i = nsent - 1; i >= 0; i--) if (sent[i].cid == cid) return i; return -1; }
/* AVDTP message from the speaker on its signalling channel */
static void av_dev(uint16_t h, uint16_t cid, uint8_t label, uint8_t type, uint8_t sig, const uint8_t *d, int n) {
    uint8_t b[64] = { (uint8_t)(label << 4 | type), sig }; memcpy(b + 2, d, n); dev_l2(h, cid, b, 2 + n);
}
/* send ~2 s of a 1 kHz / 3 kHz tone through the speaker path; check the RTP packets and save the SBC frames */
static int stream_check(uint16_t media_cid, const char *file) {
    static int16_t pcm[44100 * 2]; int bad = 0;
    for (int i = 0; i < 44100; i++) { pcm[2 * i] = (int16_t)(12000 * sin(2 * M_PI * 1000 * i / 44100.0)); pcm[2 * i + 1] = (int16_t)(8000 * sin(2 * M_PI * 3000 * i / 44100.0)); }
    FILE *f = fopen(file, "wb"); long fed = 0; int mark = nsent, last_seq = -1; uint32_t last_ts = 0;
    for (int k = 0; k < 400; k++) {                                              /* feed it all, then drain the queue */
        if (fed < 44100) fed += bta_write(pcm + fed * 2, 44100 - fed > 4096 ? 4096 : 44100 - fed, 44100);
        if (bt_test_audio_packet() <= 0) break;
        if (fed >= 44100 && k > 80) break;
    }
    int npk = 0;
    for (int i = mark; i < nsent; i++) {
        if (sent[i].cid != media_cid) continue;
        const uint8_t *d = sent[i].d; int n = sent[i].n; npk++;
        int seq = d[2] << 8 | d[3]; uint32_t ts = (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]; int nf = d[12] & 15;
        if (d[0] != 0x80 || (d[1] & 0x7F) != 96 || nf < 1) bad++;
        if (last_seq >= 0 && (seq != ((last_seq + 1) & 0xFFFF) || ts != last_ts + 128u * (uint32_t)nf)) bad++;
        if (n > 672) bad++;                                                       /* within the speaker's MTU */
        last_seq = seq; last_ts = ts;
        int o = 13;
        for (int fr = 0; fr < nf; fr++) { if (d[o] != 0x9C) bad++; int fl = 4 + 8 + (16 * d[o + 2] + 7) / 8; fwrite(d + o, 1, fl, f); o += fl; }
        if (o != n) bad++;
    }
    fclose(f);
    printf("audio: %d media packets, %d problems\n", npk, bad);
    return npk > 20 && !bad;
}
static uint8_t lastkbd[8]; static int nkbd, mdx, mdy, mbtn, nmouse;
void hid_kbd_report(uint8_t *last, const uint8_t *r, int len) { memcpy(lastkbd, r, len > 8 ? 8 : len); memcpy(last, r, len > 8 ? 8 : len); nkbd++; }
void bt_test_mouse(int dx, int dy, int b) { mdx = dx; mdy = dy; mbtn = b; nmouse++; }
void bt_test_event(uint8_t *e); void bt_test_acl_in(const uint8_t *b, int n);

int main(void) {
    dev.ep_buf[3] = epbuf; dev.vid = 0x0BDA; dev.pid = 0xB85B;
    strcpy(dev.manufacturer, "Realtek"); strcpy(dev.product, "Bluetooth Radio");
    bt_attach(&dev, 0, 3);
    bt_info();
    bt_scan(3);
    int fails = 0;
    #define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)
    /* 1: first pairing with the phone: number shown and confirmed, key stored */
    uint8_t phone[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
    bt_pair_run(phone);
    struct bt_pair st; bt_pair_status(&st);
    printf("pair 1: %s\n", st.msg);
    CHECK(st.state == BTP_DONE, "first pairing should succeed");
    CHECK(iocap == 1 && (authreq & 1), "we should say DisplayYesNo with bonding");
    CHECK(keylen > 40 && strstr(keyfile, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf") && strstr(keyfile, "Test Phone"), "link key should be saved");
    struct bt_paired pd[4]; int npd = bt_paired(pd, 4);
    CHECK(npd == 1 && !memcmp(pd[0].addr, phone, 6), "paired list should have the phone");
    /* 2: pairing again uses the stored key, no number */
    used_stored_key = 0;
    bt_pair_run(phone); bt_pair_status(&st);
    printf("pair 2: %s\n", st.msg);
    CHECK(st.state == BTP_DONE && used_stored_key, "second pairing should use the stored key");
    /* 3: a device with no screen (headphones): paired without asking */
    justworks = 1; uint8_t phones[6] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6}; bt_forget(phones);
    bt_pair_run(phones); bt_pair_status(&st);
    printf("pair 3: %s\n", st.msg);
    CHECK(st.state == BTP_DONE, "a no-screen device should pair without a number");
    justworks = 0; bt_forget(phones);
    /* 4: the user says no to the number */
    uint8_t car[6] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6};
    answer = 0;
    bt_pair_run(car); bt_pair_status(&st);
    printf("pair 4: %s\n", st.msg);
    CHECK(st.state == BTP_FAILED && declined, "declining should send a negative reply and fail");
    /* 5: a paired phone... no: a paired KEYBOARD comes back on its own and types */
    answer = 1;
    uint8_t ev4[] = {0x04, 10, phone[0], phone[1], phone[2], phone[3], phone[4], phone[5], 0x40, 0x25, 0x00, 1};
    ncmds = 0; bt_test_event(ev4);
    CHECK(sent_cmd(0x0409), "a paired device asking to connect should be accepted");
    uint8_t ev3[] = {0x03, 11, 0, 0x43, 0x00, phone[0], phone[1], phone[2], phone[3], phone[4], phone[5], 1, 0};
    bt_test_event(ev3);
    int ctrl = dev_open(0x43, 0x11, 0x70), intr = dev_open(0x43, 0x13, 0x71);
    CHECK(ctrl > 0 && intr > 0 && ctrl != intr, "the HID channels should open");
    bool boot = false; for (int i = 0; i < nsent; i++) if (sent[i].cid == 0x70 && sent[i].d[0] == 0x70) boot = true;
    CHECK(boot, "we should ask for the boot protocol on the control channel");
    uint8_t key[] = {0xA1, 0x01, 0x02, 0, 0x04, 0, 0, 0, 0, 0};                 /* shift + 'a' */
    dev_l2(0x43, intr, key, sizeof key);
    CHECK(nkbd == 1 && lastkbd[0] == 2 && lastkbd[2] == 4, "keyboard report should reach the keyboard handler");
    uint8_t ms[] = {0xA1, 0x02, 0x01, 5, (uint8_t)-3, 0};
    dev_l2(0x43, intr, ms, sizeof ms);
    CHECK(nmouse == 1 && mdx == 5 && mdy == -3 && mbtn == 1, "mouse report should move the mouse");
    uint8_t ev5[] = {0x05, 4, 0, 0x43, 0x00, 0x13};
    bt_test_event(ev5);
    CHECK(nkbd == 2 && lastkbd[2] == 0, "disconnecting should let go of held keys");
    /* 6: an unknown device is turned away */
    uint8_t stranger[] = {0x04, 10, 9, 9, 9, 9, 9, 9, 0x40, 0x25, 0x00, 1};
    ncmds = 0; bt_test_event(stranger);
    CHECK(sent_cmd(0x040A) && !sent_cmd(0x0409), "an unpaired device should be rejected");
    /* 7: pairing a new keyboard keeps the connection and opens the HID channels */
    uint8_t kbd[6] = {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6};
    justworks = 1; ncmds = 0; int mark = nsent;
    bt_pair_run(kbd); bt_pair_status(&st);
    printf("pair 5: %s\n", st.msg);
    CHECK(st.state == BTP_DONE, "keyboard should pair");
    CHECK(sent_cmd(0x0413) && !sent_cmd(0x0406), "keyboard connection should be encrypted and kept, not dropped");
    int c1 = dev_answer_conn(0x42, mark, 0x80);
    int c2 = dev_answer_conn(0x42, mark + 1, 0x81);
    CHECK(c1 > 0 && c2 > 0, "after pairing we should open control then interrupt");
    nkbd = 0; dev_l2(0x42, c2, key, sizeof key);
    CHECK(nkbd == 1, "the newly paired keyboard should type");
    justworks = 0;
    /* 8: a paired speaker connects by itself and sets up the sound (it leads: we answer) */
    uint8_t sp4[] = {0x04, 10, phone[0], phone[1], phone[2], phone[3], phone[4], phone[5], 0x14, 0x04, 0x24, 1};
    bt_test_event(sp4);
    uint8_t sp3[] = {0x03, 11, 0, 0x44, 0x00, phone[0], phone[1], phone[2], phone[3], phone[4], phone[5], 1, 0};
    bt_test_event(sp3);
    int sig = dev_open(0x44, 0x19, 0x90);
    CHECK(sig > 0, "the speaker's AVDTP signalling channel should be accepted");
    av_dev(0x44, sig, 1, 0, 0x01, 0, 0);                                          /* DISCOVER */
    int r = last_on(0x90);
    CHECK(r >= 0 && sent[r].d[0] == 0x12 && sent[r].d[1] == 0x01 && (sent[r].d[2] >> 2) == 1 && !(sent[r].d[3] & 8), "we should answer DISCOVER with our audio source");
    uint8_t seid = 1 << 2; av_dev(0x44, sig, 2, 0, 0x02, &seid, 1);               /* GET_CAPABILITIES */
    r = last_on(0x90);
    CHECK(r >= 0 && sent[r].d[0] == 0x22 && sent[r].n >= 12 && sent[r].d[4] == 7 && sent[r].d[7] == 0 && (sent[r].d[8] & 0x22) == 0x22, "our capabilities should offer SBC 44.1 kHz stereo");
    uint8_t setc[] = { 1 << 2, 3 << 2, 1, 0, 7, 6, 0, 0, 0x22, 0x15, 2, 35 };
    av_dev(0x44, sig, 3, 0, 0x03, setc, sizeof setc);                            /* SET_CONFIGURATION */
    r = last_on(0x90); CHECK(r >= 0 && sent[r].d[0] == 0x32, "SET_CONFIGURATION should be accepted");
    av_dev(0x44, sig, 4, 0, 0x06, &seid, 1);                                      /* OPEN */
    r = last_on(0x90); CHECK(r >= 0 && sent[r].d[0] == 0x42, "OPEN should be accepted");
    int media = dev_open(0x44, 0x19, 0x91);
    CHECK(media > 0 && media != sig, "the speaker's media channel should be accepted");
    av_dev(0x44, sig, 5, 0, 0x07, &seid, 1);                                      /* START */
    r = last_on(0x90); CHECK(r >= 0 && sent[r].d[0] == 0x52, "START should be accepted");
    CHECK(bta_active(), "sound should now go to the speaker");
    CHECK(stream_check(0x91, "build/test/bt_audio1.sbc"), "the speaker should get well-formed SBC packets");
    uint8_t sp5[] = {0x05, 4, 0, 0x44, 0x00, 0x13};
    bt_test_event(sp5);
    CHECK(!bta_active(), "after the speaker disconnects, sound comes back to the computer");
    /* 9: we set up the sound with a speaker that's connected (as when the user picks it) */
    bt_test_event(sp4); uint8_t sp3b[] = {0x03, 11, 0, 0x45, 0x00, phone[0], phone[1], phone[2], phone[3], phone[4], phone[5], 1, 0}; bt_test_event(sp3b);
    mark = nsent;
    bt_test_audio_start(0x45);
    int s2 = dev_answer_conn(0x45, mark, 0xA0);
    CHECK(s2 > 0, "we should open the AVDTP signalling channel");
    r = last_on(0xA0);
    CHECK(r >= 0 && sent[r].d[1] == 0x01 && (sent[r].d[0] & 3) == 0, "we should send DISCOVER");
    uint8_t seps[] = { 2 << 2 | 2, 0x08, 4 << 2, 0x08, 5 << 2, 0x00 };          /* a busy sink, a free sink, a source */
    av_dev(0x45, s2, sent[r].d[0] >> 4, 2, 0x01, seps, sizeof seps);
    r = last_on(0xA0);
    CHECK(r >= 0 && sent[r].d[1] == 0x02 && sent[r].d[2] == (4 << 2), "we should ask the free sink (4) for its capabilities");
    uint8_t caps[] = { 1, 0, 7, 6, 0, 0, 0xFF, 0xFF, 2, 51 };
    av_dev(0x45, s2, sent[r].d[0] >> 4, 2, 0x02, caps, sizeof caps);
    r = last_on(0xA0);
    CHECK(r >= 0 && sent[r].d[1] == 0x03 && sent[r].d[2] == (4 << 2) && sent[r].d[10] == 0x22 && sent[r].d[11] == 0x15 && sent[r].d[13] == 51, "SET_CONFIGURATION: SBC 44.1 kHz stereo, 16 blocks, 8 subbands, loudness, bitpool up to 51");
    av_dev(0x45, s2, sent[r].d[0] >> 4, 2, 0x03, 0, 0);
    r = last_on(0xA0);
    CHECK(r >= 0 && sent[r].d[1] == 0x06, "we should OPEN");
    mark = nsent;
    av_dev(0x45, s2, sent[r].d[0] >> 4, 2, 0x06, 0, 0);
    int m2 = dev_answer_conn(0x45, mark, 0xA1);
    CHECK(m2 > 0, "we should open the media channel");
    r = last_on(0xA0);
    CHECK(r >= 0 && sent[r].d[1] == 0x07, "we should START");
    av_dev(0x45, s2, sent[r].d[0] >> 4, 2, 0x07, 0, 0);
    CHECK(bta_active() && bt_test_audio_state() == 7, "sound should be streaming");
    CHECK(stream_check(0xA1, "build/test/bt_audio2.sbc"), "SBC packets should flow when we set it up");
    bt_forget(kbd);
    bt_forget(phone);
    CHECK(bt_paired(pd, 4) == 0, "forget should remove the key");
    printf(fails ? "bluetooth pairing: %d failures\n" : "bluetooth pairing: all checks passed\n", fails);
    return fails != 0;
}
