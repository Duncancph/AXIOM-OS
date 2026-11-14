/* Networking: network interfaces (NIC drivers) and the TCP/IP stack. */
#pragma once
#include "kernel.h"

struct netif {
    char adapter[48], driver[24];
    uint8_t mac[6];
    bool link;
    int  (*send)(struct netif *n, const void *frame, int len);
    void (*poll)(struct netif *n);              /* calls net_rx() for each received frame */
    bool (*link_up)(struct netif *n);
    void *priv;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};

/* NIC drivers */
struct netif *e1000_probe(struct pci_dev *p);
struct netif *rtl8168_probe(struct pci_dev *p);

/* stack */
void net_init(void);
void net_rx(struct netif *n, const uint8_t *frame, int len);
void net_thread(void *arg);
void net_task_exit(struct task *t);
bool net_present(void);
bool net_ready(void);
void net_get_info(struct dunc_netinfo *ni);
void net_set_dns(const struct ipaddr *a);           /* 0: use the network's DNS server */
void net_renew(void);
long net_ping(const struct ipaddr *ip, uint32_t timeout_ms);
int  net_resolve(const char *name, struct ipaddr *out, int max);   /* count, best first, or an error */
int  tcp_connect(const struct ipaddr *ip, uint16_t port, uint32_t timeout_ms);
long tcp_send(int h, const void *buf, uint32_t len);
long tcp_recv(int h, void *buf, uint32_t len, int64_t timeout_ms);
int  tcp_close(int h);
