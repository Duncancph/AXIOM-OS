/* xHCI (USB 3.x) host controller driver, polled. Handles every USB port speed
 * (1.1 / 2.0 / 3.x) on devices attached directly to the controller's root ports. */
#include "usb.h"

#define RING_SIZE 256
#define EVT_SIZE 256

struct xhci {
    struct pci_dev *pci;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    uint32_t max_slots, max_ports, ctx_size;
    uint64_t *dcbaa;
    struct ring cmd;
    trb_t *evt; uint32_t evt_idx, evt_cycle;
    volatile int cmd_done; trb_t cmd_evt;
    uint8_t port_proto[256];     /* USB major revision per port (2 or 3) */
    struct usb_dev *slot_dev[256];
};

static struct xhci ctrls[4];
static int nctrl;
struct usb_dev usb_devs[USB_MAX_DEVS];

static inline uint32_t op_r(struct xhci *x, uint32_t o) { return mmio_r32(x->op + o); }
static inline void op_w(struct xhci *x, uint32_t o, uint32_t v) { mmio_w32(x->op + o, v); }
#define PORTSC(p) (0x400 + 0x10 * ((p) - 1))

static bool wait_until(volatile uint8_t *reg, uint32_t mask, uint32_t want, uint32_t ms) {
    uint64_t end = uptime_ms() + ms;
    while ((mmio_r32(reg) & mask) != want) { if (uptime_ms() > end) return false; cpu_relax(); }
    return true;
}

/* ---------------- rings ---------------- */

static void ring_init(struct ring *r, uint32_t size) {
    r->trbs = page_alloc((size * 16 + 4095) / 4096);
    r->size = size; r->idx = 0; r->cycle = 1;
}

static void ring_put(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    trb_t *t = &r->trbs[r->idx];
    t->d[0] = d0; t->d[1] = d1; t->d[2] = d2;
    mb();
    t->d[3] = (d3 & ~1u) | r->cycle;
    if (++r->idx == r->size - 1) {
        trb_t *l = &r->trbs[r->size - 1];
        uint64_t a = (uint64_t)r->trbs;
        l->d[0] = (uint32_t)a; l->d[1] = a >> 32; l->d[2] = 0;
        mb();
        l->d[3] = (6u << 10) | (1u << 1) | r->cycle;     /* Link TRB, toggle cycle */
        r->idx = 0; r->cycle ^= 1;
    }
}

/* Make sure the next n TRBs are contiguous (a TD must not straddle the link TRB). */
static void ring_reserve(struct ring *r, uint32_t n, uint32_t noop_type) {
    while (r->idx + n > r->size - 1) ring_put(r, 0, 0, 0, noop_type << 10);
}

/* ---------------- events ---------------- */

static void handle_event(struct xhci *x, trb_t *e) {
    uint32_t type = (e->d[3] >> 10) & 63;
    if (type == 33) {                    /* Command Completion */
        x->cmd_evt = *e; x->cmd_done = 1;
    } else if (type == 32) {             /* Transfer Event */
        int slot = e->d[3] >> 24, dci = (e->d[3] >> 16) & 31;
        struct usb_dev *d = x->slot_dev[slot];
        if (!d) return;
        uint32_t cc = e->d[2] >> 24, res = e->d[2] & 0xFFFFFF;
        if (d->ep_cb[dci]) {
            usb_xfer_cb cb = d->ep_cb[dci];
            d->ep_cb[dci] = 0;
            cb(d, dci, cc, d->ep_len[dci] > res ? d->ep_len[dci] - res : 0);
        } else {
            d->xfer_cc[dci] = cc; d->xfer_res[dci] = res; d->xfer_done[dci] = 1;
        }
    }
    /* Port status change (34) and others: ignored for now (no hot-plug yet). */
}

static void poll_ctrl(struct xhci *x) {
    bool any = false;
    for (;;) {
        trb_t *e = &x->evt[x->evt_idx];
        if ((e->d[3] & 1) != x->evt_cycle) break;
        mb();
        trb_t copy = *e;
        if (++x->evt_idx == EVT_SIZE) { x->evt_idx = 0; x->evt_cycle ^= 1; }
        any = true;
        handle_event(x, &copy);
    }
    if (any) mmio_w64(x->rt + 0x20 + 0x18, (uint64_t)&x->evt[x->evt_idx] | (1 << 3));
}

void xhci_poll(void) { for (int i = 0; i < nctrl; i++) poll_ctrl(&ctrls[i]); }

static int xhci_cmd(struct xhci *x, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, trb_t *out) {
    x->cmd_done = 0;
    ring_reserve(&x->cmd, 1, 23);
    ring_put(&x->cmd, d0, d1, d2, d3);
    mb();
    mmio_w32(&x->db[0], 0);
    uint64_t end = uptime_ms() + 2000;
    while (!x->cmd_done) { poll_ctrl(x); if (uptime_ms() > end) return -1; cpu_relax(); }
    if (out) *out = x->cmd_evt;
    return x->cmd_evt.d[2] >> 24;
}

/* ---------------- contexts ---------------- */

static inline uint32_t *in_ctrl(struct usb_dev *d) { return (uint32_t *)d->in_ctx; }
static inline uint32_t *in_slot(struct usb_dev *d) { return (uint32_t *)(d->in_ctx + d->xc->ctx_size); }
static inline uint32_t *in_ep(struct usb_dev *d, int dci) { return (uint32_t *)(d->in_ctx + d->xc->ctx_size * (1 + dci)); }
static inline uint32_t *out_slot(struct usb_dev *d) { return (uint32_t *)d->out_ctx; }

static void ring_doorbell(struct usb_dev *d, int dci) { mb(); mmio_w32(&d->xc->db[d->slot], dci); }

static int wait_xfer(struct usb_dev *d, int dci, uint32_t ms) {
    uint64_t end = uptime_ms() + ms;
    while (!d->xfer_done[dci]) { xhci_poll(); if (uptime_ms() > end) return -1; cpu_relax(); }
    return d->xfer_cc[dci];
}

/* Recover a halted endpoint: Reset Endpoint + Set TR Dequeue Pointer. */
static void reset_endpoint(struct usb_dev *d, int dci) {
    struct xhci *x = d->xc;
    xhci_cmd(x, 0, 0, 0, (14u << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), 0);
    struct ring *r = &d->rings[dci];
    uint64_t p = (uint64_t)&r->trbs[r->idx] | r->cycle;
    xhci_cmd(x, (uint32_t)p, p >> 32, 0, (16u << 10) | ((uint32_t)dci << 16) | ((uint32_t)d->slot << 24), 0);
}

int usb_control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data) {
    struct ring *r = &d->rings[1];
    bool in = rt & 0x80;
    if (len > 4096) return -1;
    if (!in && len) memcpy(d->ctrl_buf, data, len);
    ring_reserve(r, 3, 8);
    uint32_t trt = len == 0 ? 0 : in ? 3 : 2;
    ring_put(r, rt | (req << 8) | ((uint32_t)val << 16), idx | ((uint32_t)len << 16), 8,
             (2u << 10) | (1u << 6) | (trt << 16));
    if (len) {
        uint64_t a = (uint64_t)d->ctrl_buf;
        ring_put(r, (uint32_t)a, a >> 32, len, (3u << 10) | (in ? 1u << 16 : 0));
    }
    ring_put(r, 0, 0, 0, (4u << 10) | (1u << 5) | ((len && in) ? 0 : 1u << 16));
    d->xfer_done[1] = 0;
    ring_doorbell(d, 1);
    int cc = wait_xfer(d, 1, 1000);
    if (cc == CC_STALL || cc < 0) { reset_endpoint(d, 1); return -1; }
    if (cc != CC_SUCCESS && cc != CC_SHORT) return -1;
    if (in && len) memcpy(data, d->ctrl_buf, len);
    return 0;
}

int usb_queue_in(struct usb_dev *d, int dci, usb_xfer_cb cb) {
    struct ring *r = &d->rings[dci];
    if (!r->trbs) return -1;
    ring_reserve(r, 1, 8);
    d->ep_cb[dci] = cb;
    uint64_t a = (uint64_t)d->ep_buf[dci];
    ring_put(r, (uint32_t)a, a >> 32, d->ep_len[dci], (1u << 10) | (1u << 5) | (1u << 2));  /* Normal, IOC, ISP */
    ring_doorbell(d, dci);
    return 0;
}

/* ---------------- descriptors ---------------- */

const uint8_t *usb_find_endpoint(struct usb_dev *d, int iface, int type, bool in, const uint8_t **ss_comp) {
    int cur = -1;
    for (int i = 0; i + 1 < d->config_len && d->config[i]; i += d->config[i]) {
        const uint8_t *p = &d->config[i];
        if (p[1] == 4) cur = p[2];
        if (p[1] == 5 && cur == iface && (p[3] & 3) == type && (!!(p[2] & 0x80)) == in) {
            if (ss_comp) {
                const uint8_t *n = p + p[0];
                *ss_comp = (n < d->config + d->config_len && n[1] == 0x30) ? n : 0;
            }
            return p;
        }
    }
    return 0;
}

int usb_configure_ep(struct usb_dev *d, const uint8_t *ep, const uint8_t *ss) {
    int num = ep[2] & 15; bool in = ep[2] & 0x80; int xtype = ep[3] & 3;
    int dci = num * 2 + (in ? 1 : 0);
    uint32_t mps = (ep[4] | ep[5] << 8) & 0x7FF;
    uint32_t burst = ss ? ss[2] : (d->speed == 3 && (xtype & 1) ? ((ep[5] >> 3) & 3) : 0);
    uint32_t interval = 0;
    if (xtype == 3 || xtype == 1) {
        if (d->speed >= 3) interval = ep[6] ? ep[6] - 1 : 0;
        else { uint32_t f = ep[6] ? ep[6] * 8 : 8; interval = 0; while ((2u << interval) <= f) interval++; if (interval < 3) interval = 3; }
        if (interval > 15) interval = 15;
    }
    uint32_t eptype = (in ? 4 : 0) + xtype;   /* 1 iso out,2 bulk out,3 int out,5 iso in,6 bulk in,7 int in */
    struct ring *r = &d->rings[dci];
    if (!r->trbs) ring_init(r, RING_SIZE);
    if (!d->ep_buf[dci]) d->ep_buf[dci] = page_alloc(1);
    d->ep_len[dci] = xtype == 2 ? 4096 : (mps * (burst + 1) > 1024 ? 1024 : mps * (burst + 1));

    memset(d->in_ctx, 0, d->xc->ctx_size * 33);
    memcpy(in_slot(d), out_slot(d), d->xc->ctx_size);
    in_ctrl(d)[1] = 1 | (1u << dci);
    if (dci > d->max_dci) d->max_dci = dci;
    in_slot(d)[0] = (in_slot(d)[0] & ~(31u << 27)) | ((uint32_t)d->max_dci << 27);
    uint32_t *c = in_ep(d, dci);
    uint32_t esit = mps * (burst + 1);
    c[0] = (interval << 16) | ((esit >> 16) << 24);
    c[1] = (3u << 1) | (eptype << 3) | (burst << 8) | (mps << 16);
    uint64_t tr = (uint64_t)r->trbs | r->cycle;
    c[2] = (uint32_t)tr; c[3] = tr >> 32;
    c[4] = (xtype == 2 ? 1024 : mps) | ((esit & 0xFFFF) << 16);
    int cc = xhci_cmd(d->xc, (uint32_t)(uint64_t)d->in_ctx, (uint64_t)d->in_ctx >> 32, 0,
                      (12u << 10) | ((uint32_t)d->slot << 24), 0);
    return cc == CC_SUCCESS ? dci : -1;
}

static void get_string(struct usb_dev *d, uint8_t idx, char *out, int n) {
    out[0] = 0;
    if (!idx) return;
    uint8_t buf[256];
    if (usb_control(d, 0x80, 6, 0x0300 | idx, 0x0409, 255, buf) < 0) return;
    int len = buf[0], j = 0;
    for (int i = 2; i + 1 < len && j < n - 1; i += 2) out[j++] = (buf[i] >= 32 && buf[i] < 127 && !buf[i + 1]) ? buf[i] : '?';
    out[j] = 0;
    while (j > 0 && out[j - 1] == ' ') out[--j] = 0;
}

/* ---------------- device enumeration ---------------- */

static const char *speed_name(int s) {
    switch (s) { case 1: return "12 Mb/s"; case 2: return "1.5 Mb/s"; case 3: return "480 Mb/s"; case 4: return "5 Gb/s"; case 5: return "10 Gb/s"; }
    return "?";
}

static void attach_drivers(struct usb_dev *d) {
    int cur_if = -1; uint8_t ic = 0, is = 0, ip = 0;
    for (int i = 0; i + 1 < d->config_len && d->config[i]; i += d->config[i]) {
        const uint8_t *p = &d->config[i];
        if (p[1] != 4) continue;
        if (p[3] != 0) continue;                  /* alternate settings: only use alt 0 */
        cur_if = p[2]; ic = p[5]; is = p[6]; ip = p[7];
        if (ic == 0x03 && is == 1 && ip == 1 && !d->driver) usbkbd_attach(d, cur_if);
        else if (ic == 0xE0 && is == 1 && ip == 1 && !d->driver) {
            const uint8_t *ss;
            const uint8_t *ep = usb_find_endpoint(d, cur_if, 3, true, &ss);
            if (ep) {
                int dci = usb_configure_ep(d, ep, ss);
                if (dci > 0) { d->driver = "bluetooth"; bt_attach(d, cur_if, dci); }
            }
        }
    }
    if (!d->driver && d->dclass == 9) d->driver = "hub (not supported yet)";
}

static void enumerate_port(struct xhci *x, int port) {
    uint32_t sc = op_r(x, PORTSC(port));
    if (!(sc & 1)) return;
    uint32_t keep = sc & ~((1u << 1) | 0x00FE0000u | (1u << 16));
    if (!(sc & 2) || x->port_proto[port] != 3) {
        /* USB2 ports must be reset to become enabled; USB3 ports enable after link training */
        op_w(x, PORTSC(port), keep | (1u << 4));
        uint64_t end = uptime_ms() + 500;
        while (uptime_ms() < end) { sc = op_r(x, PORTSC(port)); if ((sc & (1u << 21)) && (sc & 2)) break; sleep_ms(1); }
        op_w(x, PORTSC(port), (sc & ~((1u << 1) | 0x00FE0000u | (1u << 16))) | (sc & 0x00FE0000u));
        sleep_ms(20);
        sc = op_r(x, PORTSC(port));
        if (!(sc & 2)) { LOG_WARN("USB port %d: device did not enable\n", port); return; }
    }
    int speed = (sc >> 10) & 15;

    struct usb_dev *d = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++) if (!usb_devs[i].used) { d = &usb_devs[i]; break; }
    if (!d) return;

    trb_t ev;
    if (xhci_cmd(x, 0, 0, 0, 9u << 10, &ev) != CC_SUCCESS) { LOG_WARN("USB port %d: no free slot\n", port); return; }
    int slot = ev.d[3] >> 24;
    memset(d, 0, sizeof *d);
    d->used = true; d->xc = x; d->slot = slot; d->port = port; d->speed = speed; d->max_dci = 1;
    d->out_ctx = page_alloc(1);
    d->in_ctx = page_alloc(2);
    d->ctrl_buf = page_alloc(1);
    ring_init(&d->rings[1], RING_SIZE);
    x->dcbaa[slot] = (uint64_t)d->out_ctx;
    x->slot_dev[slot] = d;

    uint32_t mps = speed == 2 || speed == 1 ? 8 : speed == 3 ? 64 : 512;
    in_ctrl(d)[1] = 3;   /* A0 | A1 */
    in_slot(d)[0] = ((uint32_t)speed << 20) | (1u << 27);
    in_slot(d)[1] = (uint32_t)port << 16;
    uint32_t *e0 = in_ep(d, 1);
    e0[1] = (3u << 1) | (4u << 3) | (mps << 16);
    uint64_t tr = (uint64_t)d->rings[1].trbs | 1;
    e0[2] = (uint32_t)tr; e0[3] = tr >> 32;
    e0[4] = 8;
    int cc = xhci_cmd(x, (uint32_t)(uint64_t)d->in_ctx, (uint64_t)d->in_ctx >> 32, 0, (11u << 10) | ((uint32_t)slot << 24), 0);
    if (cc != CC_SUCCESS) { LOG_WARN("USB port %d: address device failed (cc=%d)\n", port, cc); d->used = false; return; }
    sleep_ms(10);

    uint8_t dd[18];
    if (usb_control(d, 0x80, 6, 0x0100, 0, 8, dd) < 0) { LOG_WARN("USB port %d: no response\n", port); return; }
    if (speed <= 3 && dd[7] && dd[7] != mps) {     /* fix EP0 max packet size (full speed) */
        mps = dd[7];
        memset(d->in_ctx, 0, x->ctx_size * 3);
        in_ctrl(d)[1] = 2;
        in_ep(d, 1)[1] = (3u << 1) | (4u << 3) | (mps << 16);
        xhci_cmd(x, (uint32_t)(uint64_t)d->in_ctx, (uint64_t)d->in_ctx >> 32, 0, (13u << 10) | ((uint32_t)slot << 24), 0);
    }
    d->mps0 = mps;
    if (usb_control(d, 0x80, 6, 0x0100, 0, 18, dd) < 0) return;
    d->vid = dd[8] | dd[9] << 8; d->pid = dd[10] | dd[11] << 8;
    d->dclass = dd[4]; d->dsub = dd[5]; d->dproto = dd[6];
    get_string(d, dd[14], d->manufacturer, sizeof d->manufacturer);
    get_string(d, dd[15], d->product, sizeof d->product);

    uint8_t ch[9];
    if (usb_control(d, 0x80, 6, 0x0200, 0, 9, ch) < 0) return;
    uint16_t total = ch[2] | ch[3] << 8;
    if (total > sizeof d->config) total = sizeof d->config;
    if (usb_control(d, 0x80, 6, 0x0200, 0, total, d->config) < 0) return;
    d->config_len = total;
    if (usb_control(d, 0x00, 9, d->config[5], 0, 0, 0) < 0) { LOG_WARN("USB port %d: set configuration failed\n", port); return; }

    attach_drivers(d);
    LOG_OK("USB %04x:%04x %s %s [%s]%s%s\n", d->vid, d->pid, d->manufacturer, d->product, speed_name(speed),
           d->driver ? " -> " : "", d->driver ? d->driver : "");
}

/* ---------------- controller bring-up ---------------- */

static void intel_route_ports(struct pci_dev *p) {
    /* Older Intel chipsets (7/8/9 series) boot with ports routed to EHCI; switch them to xHCI. */
    static const uint16_t ids[] = { 0x1E31, 0x8C31, 0x9C31, 0x9CB1, 0x8CB1 };
    if (p->vendor != 0x8086) return;
    for (unsigned i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (p->device == ids[i]) {
            pci_write32(p->bus, p->dev, p->fn, 0xD8, pci_read32(p->bus, p->dev, p->fn, 0xDC));
            pci_write32(p->bus, p->dev, p->fn, 0xD0, pci_read32(p->bus, p->dev, p->fn, 0xD4));
        }
}

static void bios_handoff(struct xhci *x) {
    uint32_t off = (mmio_r32(x->cap + 0x10) >> 16) * 4;
    while (off) {
        volatile uint8_t *c = x->cap + off;
        uint32_t v = mmio_r32(c);
        if ((v & 0xFF) == 1) {           /* USB Legacy Support */
            if (v & (1u << 16)) {
                mmio_w32(c, v | (1u << 24));
                if (!wait_until(c, (1u << 16), 0, 1000)) LOG_WARN("xHCI: BIOS did not release controller\n");
            }
            uint32_t l = mmio_r32(c + 4);
            l &= (0x7u << 1) | (0xFFu << 5) | (0x7u << 17);
            l |= 0x7u << 29;
            mmio_w32(c + 4, l);
        } else if ((v & 0xFF) == 2) {    /* Supported Protocol */
            uint32_t major = v >> 24, d2 = mmio_r32(c + 8);
            uint32_t first = d2 & 0xFF, count = (d2 >> 8) & 0xFF;
            for (uint32_t p = first; p < first + count && p < 256; p++) x->port_proto[p] = major;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

static bool xhci_start(struct pci_dev *p) {
    struct xhci *x = &ctrls[nctrl];
    memset(x, 0, sizeof *x);
    x->pci = p;
    uint64_t size;
    uint64_t base = pci_bar(p, 0, &size);
    if (!base) return false;
    if (size < 0x10000) size = 0x10000;
    map_mmio(base, size);
    uint16_t cmd = pci_read16(p->bus, p->dev, p->fn, 4);
    pci_write16(p->bus, p->dev, p->fn, 4, (cmd | 0x6) & ~0x400);   /* memory + bus master, INTx off */
    intel_route_ports(p);

    x->cap = (volatile uint8_t *)base;
    x->op = x->cap + (mmio_r32(x->cap) & 0xFF);
    x->rt = x->cap + (mmio_r32(x->cap + 0x18) & ~0x1Fu);
    x->db = (volatile uint32_t *)(x->cap + (mmio_r32(x->cap + 0x14) & ~0x3u));
    uint32_t hcs1 = mmio_r32(x->cap + 4), hcs2 = mmio_r32(x->cap + 8), hcc1 = mmio_r32(x->cap + 0x10);
    x->max_slots = hcs1 & 0xFF; x->max_ports = hcs1 >> 24;
    x->ctx_size = (hcc1 & 4) ? 64 : 32;

    bios_handoff(x);

    /* halt + reset */
    op_w(x, 0, op_r(x, 0) & ~1u);
    wait_until(x->op + 4, 1, 1, 100);
    op_w(x, 0, 2);
    sleep_ms(2);
    if (!wait_until(x->op + 0, 2, 0, 1000) || !wait_until(x->op + 4, 1u << 11, 0, 1000)) {
        LOG_WARN("xHCI: controller reset timed out\n");
        return false;
    }

    op_w(x, 0x38, x->max_slots);
    x->dcbaa = page_alloc(1);
    uint32_t nscratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nscratch) {
        uint64_t *arr = page_alloc((nscratch * 8 + 4095) / 4096);
        for (uint32_t i = 0; i < nscratch; i++) arr[i] = (uint64_t)page_alloc(1);
        x->dcbaa[0] = (uint64_t)arr;
    }
    mmio_w64(x->op + 0x30, (uint64_t)x->dcbaa);
    ring_init(&x->cmd, RING_SIZE);
    mmio_w64(x->op + 0x18, (uint64_t)x->cmd.trbs | 1);

    x->evt = page_alloc((EVT_SIZE * 16 + 4095) / 4096);
    x->evt_cycle = 1;
    uint64_t *erst = page_alloc(1);
    erst[0] = (uint64_t)x->evt; erst[1] = EVT_SIZE;
    volatile uint8_t *ir = x->rt + 0x20;
    mmio_w32(ir + 0x08, 1);
    mmio_w64(ir + 0x18, (uint64_t)x->evt);
    mmio_w64(ir + 0x10, (uint64_t)erst);
    mmio_w32(ir + 0x00, mmio_r32(ir) | 1);      /* clear any pending IP */

    op_w(x, 0, 1);   /* run (no interrupts: we poll) */
    if (!wait_until(x->op + 4, 1, 0, 1000)) { LOG_WARN("xHCI: controller failed to start\n"); return false; }

    if (hcc1 & 8)   /* port power control */
        for (uint32_t i = 1; i <= x->max_ports; i++) {
            uint32_t sc = op_r(x, PORTSC(i));
            if (!(sc & (1u << 9))) op_w(x, PORTSC(i), (sc & ~((1u << 1) | 0x00FE0000u | (1u << 16))) | (1u << 9));
        }
    nctrl++;
    LOG_OK("USB 3 controller %04x:%04x at %02x:%02x.%d, %u ports\n", p->vendor, p->device, p->bus, p->dev, p->fn, x->max_ports);
    return true;
}

void xhci_init_all(void) {
    for (int i = 0; i < pci_count && nctrl < 4; i++) {
        struct pci_dev *p = &pci_devs[i];
        if (p->class_code == 0x0C && p->subclass == 0x03 && p->prog_if == 0x30) xhci_start(p);
    }
    if (!nctrl) { LOG_WARN("No USB 3 (xHCI) controller found\n"); return; }
    sleep_ms(300);   /* let devices connect / USB3 links train after the reset */
    for (int c = 0; c < nctrl; c++) {
        struct xhci *x = &ctrls[c];
        for (uint32_t port = 1; port <= x->max_ports; port++) enumerate_port(x, port);
    }
}

void usb_list(void) {
    int n = 0;
    for (int i = 0; i < USB_MAX_DEVS; i++) {
        struct usb_dev *d = &usb_devs[i];
        if (!d->used || !d->vid) continue;
        n++;
        kprintf("  port %-2d %04x:%04x %-9s class %02x  %s %s", d->port, d->vid, d->pid, speed_name(d->speed),
                d->dclass, d->manufacturer, d->product);
        if (d->driver) { console_set_color(C_LGREEN); kprintf("  [%s]", d->driver); console_set_color(C_LGRAY); }
        kprintf("\n");
    }
    if (!n) kprintf("  No USB devices found.\n");
}
