/* Joining a Wi-Fi network as a station (802.11 "MLME" + data path), independent of the card: authentication
 * (open system), association, the WPA2-PSK handshake (user/lib/crypto/wpa.c), then Ethernet frames in and out
 * as 802.11 data frames, encrypted with CCMP. The card driver gives it received frames and sends what it
 * produces; tests/test_wifi_sta.py drives it against a simulated access point. */
#pragma once
#include <stdint.h>
#include "../user/lib/crypto/wpa.h"

enum { WS_IDLE, WS_AUTH, WS_ASSOC, WS_HANDSHAKE, WS_CONNECTED, WS_FAILED };
struct wifi_sta {
    int state;
    uint8_t me[6], bssid[6];
    char ssid[33];
    uint8_t secure;                    /* WPA2-PSK (else an open network); not bool: see wpa.h */
    struct wpa_sup sup;
    uint8_t tk[16], gtk[32]; int gtk_idx; uint8_t keys;
    uint64_t tx_pn, rx_pn, rx_gpn;
    uint16_t seq, aid;
    int tries; uint64_t deadline;
    char why[96];                      /* why it failed / disconnected */
    /* the driver's side */
    void (*send)(void *ctx, const uint8_t *frame, int len);   /* an 802.11 frame to transmit (no FCS) */
    void (*deliver)(void *ctx, const uint8_t *eth, int len);  /* a received Ethernet frame */
    void *ctx;
};
/* start joining; pmk is NULL for an open network. snonce: 32 random bytes */
void wsta_join(struct wifi_sta *s, const uint8_t me[6], const uint8_t bssid[6], const char *ssid, const uint8_t *pmk, const uint8_t snonce[32], uint64_t now);
void wsta_rx(struct wifi_sta *s, const uint8_t *f, int len, uint64_t now);    /* a received 802.11 frame (no FCS) */
void wsta_tick(struct wifi_sta *s, uint64_t now);                              /* retries and time-outs (ms) */
int  wsta_send_eth(struct wifi_sta *s, const uint8_t *eth, int len);           /* an Ethernet frame out: 0 ok */
void wsta_leave(struct wifi_sta *s);
