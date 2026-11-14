/* IPv4 and IPv6 addresses, shared by the kernel and apps. */
#pragma once
#include <stdint.h>


/* An IPv4 or IPv6 address. Bytes are in network order; an IPv4 address uses b[0..3]. */
struct ipaddr {
    uint8_t v6;          /* 0 = IPv4, 1 = IPv6 */
    uint8_t prefix;      /* prefix length, where it matters (an interface address); otherwise 0 */
    uint8_t pad[2];
    uint8_t b[16];
};
#define IP_STRLEN 46     /* longest text form, with the terminating NUL */

int  ip_parse(const char *s, struct ipaddr *out);              /* "192.0.2.1", "2001:db8::1", "::ffff:1.2.3.4" */
int  ip_format(const struct ipaddr *a, char *out);              /* RFC 5952 form; returns length */
int  ip_eq(const struct ipaddr *a, const struct ipaddr *b);
struct ipaddr ip_v4(uint32_t net_order);                        /* from the IP4(a,b,c,d) form */
uint32_t ip_to_v4(const struct ipaddr *a);
int  ip_same_prefix(const uint8_t *a, const uint8_t *b, int bits);

/* IPv6 address kinds (RFC 4291) */
static inline int ip6_is_unspec(const uint8_t *a) { for (int i = 0; i < 16; i++) if (a[i]) return 0; return 1; }
static inline int ip6_is_multicast(const uint8_t *a) { return a[0] == 0xFF; }
static inline int ip6_is_linklocal(const uint8_t *a) { return a[0] == 0xFE && (a[1] & 0xC0) == 0x80; }
static inline int ip6_is_loopback(const uint8_t *a) { for (int i = 0; i < 15; i++) if (a[i]) return 0; return a[15] == 1; }
