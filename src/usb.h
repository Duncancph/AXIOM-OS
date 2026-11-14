#pragma once
#include "kernel.h"

typedef struct { uint32_t d[4]; } trb_t;

struct ring { trb_t *trbs; uint32_t idx, cycle, size; };

struct xhci;
struct usb_dev;
typedef void (*usb_xfer_cb)(struct usb_dev *dev, int dci, int cc, uint32_t len);

struct usb_dev {
    bool used;
    struct xhci *xc;
    int slot, port, speed;
    uint8_t *in_ctx, *out_ctx;
    struct ring rings[32];           /* transfer rings indexed by DCI */
    uint8_t *ctrl_buf;               /* DMA bounce buffer for control transfers */
    uint8_t *ep_buf[32];             /* DMA buffers for interrupt/bulk endpoints */
    uint32_t ep_len[32];
    usb_xfer_cb ep_cb[32];
    usb_xfer_cb recover_cb[32];      /* endpoints waiting to be reset after a transfer error */
    int recoveries;
    volatile int xfer_done[32];
    volatile uint32_t xfer_cc[32], xfer_res[32];
    volatile uint64_t xfer_ptr[32];
    uint16_t vid, pid, mps0;
    uint8_t dclass, dsub, dproto;
    char manufacturer[48], product[48];
    uint8_t config[1024]; uint16_t config_len;
    const char *driver;
    int max_dci;
    /* topology */
    int root_port, depth; uint32_t route;
    struct usb_dev *parent; int parent_port;
    int tt_slot, tt_port; bool mtt;
    /* hub */
    int hub_ports, hub_dci; volatile uint32_t hub_pending;
    /* drivers */
    void (*detach)(struct usb_dev *d);
    void *drv;
    volatile int lock_owner; int lock_depth;
};

/* Completion codes */
#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT 13
/* after a failed interrupt transfer: reset the endpoint and carry on polling (done by xhci_service) */
void usb_recover_in(struct usb_dev *d, int dci, int cc, usb_xfer_cb cb);

#define USB_MAX_DEVS 32
extern struct usb_dev usb_devs[USB_MAX_DEVS];

int usb_control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data);
/* Synchronous bulk transfer into/out of a physically contiguous buffer. Returns bytes moved, or -1 / -2 (stall). */
long usb_bulk(struct usb_dev *d, int dci, void *buf, uint32_t len, uint32_t timeout_ms);
void usb_clear_halt(struct usb_dev *d, int dci, uint8_t ep_addr);
void usb_lock(struct usb_dev *d);
void usb_unlock(struct usb_dev *d);
void xhci_service(void);            /* hot-plug: called regularly by the USB thread */
void msc_attach(struct usb_dev *d, int iface);
int usb_configure_ep(struct usb_dev *d, const uint8_t *epdesc, const uint8_t *ss_comp);
int usb_queue_in(struct usb_dev *d, int dci, usb_xfer_cb cb);
const uint8_t *usb_find_endpoint(struct usb_dev *d, int iface, int type, bool in, const uint8_t **ss_comp);

void usbkbd_attach(struct usb_dev *d, int iface);
void usbnet_attach(struct usb_dev *d, int ctrl_if, uint8_t cls, uint8_t sub, uint8_t proto);
void usb_get_string(struct usb_dev *d, uint8_t idx, char *out, int n);
void usbmouse_attach(struct usb_dev *d, int iface);
