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
    void (*tick)(struct netif *n);              /* (optional) run even while another adapter is in use: Wi-Fi joining */
    void *priv;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};

/* NIC drivers */
void net_prefer(struct netif *n);               /* use this adapter now (Wi-Fi once it has joined) */
struct netif *e1000_probe(struct pci_dev *p);
struct netif *rtl8168_probe(struct pci_dev *p);

/* stack */
void net_init(void);
void net_add_netif(struct netif *n);      /* a network adapter appeared (PCI at start-up, USB any time) */
void net_remove_netif(struct netif *n);   /* it was unplugged */
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
/* sockets for Linux programs: nothing waits, linux_net_wake() is called when something changes */
struct tcp_sock_info { int state;                 /* 0 connecting, 1 connected, 2 closed, 3 we've sent FIN */
                       int err; uint32_t rlen, sfree; bool eof; struct ipaddr lip, rip; uint16_t lport, rport; };
int  tcp_sock_open(const struct ipaddr *ip, uint16_t port, const void *who);
int  tcp_sock_info(int h, const void *who, struct tcp_sock_info *i);
long tcp_sock_send(int h, const void *who, const void *buf, uint32_t len);
long tcp_sock_recv(int h, const void *who, void *buf, uint32_t len, bool peek);
void tcp_sock_shutdown(int h, const void *who);
void tcp_sock_close(int h, const void *who);
int  udp_sock_open(const void *who, uint16_t port);
int  udp_sock_port(int h, const void *who);
long udp_sock_send(int h, const void *who, const struct ipaddr *dst, uint16_t dport, const void *buf, uint32_t len);
long udp_sock_recv(int h, const void *who, void *buf, uint32_t len, struct ipaddr *from, uint16_t *fport, bool peek, uint32_t *full);
int  udp_sock_pending(int h, const void *who);
void udp_sock_close(int h, const void *who);
int  net_local_addr(const struct ipaddr *dst, struct ipaddr *out);
void linux_net_wake(void);                          /* (linux.c) */
