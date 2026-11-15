/* stub */
#include "aac_int.h"
struct aac_ps { int started; };
struct aac_ps *aac_ps_new(void) { return calloc(1, sizeof(struct aac_ps)); }
void aac_ps_free(struct aac_ps *p) { free(p); }
void aac_ps_reset(struct aac_ps *p) { }
int aac_ps_parse(struct aac_ps *p, struct aac_bits *b, int bits_left) { aac_skip(b, bits_left); return bits_left; }
int aac_ps_started(struct aac_ps *p) { return 0; }
void aac_ps_apply(struct aac_ps *p, float L[2][38][64], float R[2][38][64], int top) { memcpy(R, L, sizeof(float) * 2 * 38 * 64); }
