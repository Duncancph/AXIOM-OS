/* SBC audio encoder for Bluetooth (A2DP): 44.1 or 48 kHz stereo, 8 subbands, 16 blocks, loudness allocation. */
#pragma once
#include <stdint.h>
struct sbc_enc { int32_t x[2][80]; int rate48, bitpool; };
void sbc_init(struct sbc_enc *e, int rate, int bitpool);   /* bitpool 2..53 (sets the bit rate: 53 is ~330 kbit/s) */
int  sbc_frame_len(const struct sbc_enc *e);               /* bytes per frame (128 stereo samples) */
int  sbc_encode(struct sbc_enc *e, const int16_t *pcm, uint8_t *out);   /* 128 interleaved L R samples -> frame */
