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
    volatile int xfer_done[32];
    volatile uint32_t xfer_cc[32], xfer_res[32];
    uint16_t vid, pid, mps0;
    uint8_t dclass, dsub, dproto;
    char manufacturer[48], product[48];
    uint8_t config[1024]; uint16_t config_len;
    const char *driver;
    int max_dci;
};

/* Completion codes */
#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT 13

#define USB_MAX_DEVS 32
extern struct usb_dev usb_devs[USB_MAX_DEVS];

int usb_control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data);
int usb_configure_ep(struct usb_dev *d, const uint8_t *epdesc, const uint8_t *ss_comp);
int usb_queue_in(struct usb_dev *d, int dci, usb_xfer_cb cb);
const uint8_t *usb_find_endpoint(struct usb_dev *d, int iface, int type, bool in, const uint8_t **ss_comp);

void usbkbd_attach(struct usb_dev *d, int iface);
