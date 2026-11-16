/* Host test for the SBC encoder: writes test tones as an .sbc file; tests/test_sbc.py decodes it with ffmpeg and
 * measures how close the result is to the input. Usage: test_sbc out.sbc freq_left freq_right amplitude rate */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/sbc.h"
int main(int argc, char **argv) {
    if (argc < 6) return 2;
    double fl = atof(argv[2]), fr = atof(argv[3]), amp = atof(argv[4]); int rate = atoi(argv[5]);
    struct sbc_enc e; sbc_init(&e, rate, 53);
    FILE *f = fopen(argv[1], "wb");
    int16_t pcm[256]; uint8_t out[256]; long t = 0;
    for (int fr_i = 0; fr_i < 400; fr_i++) {
        for (int i = 0; i < 128; i++, t++) {
            pcm[2 * i] = (int16_t)lrint(amp * sin(2 * M_PI * fl * t / rate));
            pcm[2 * i + 1] = (int16_t)lrint(amp * sin(2 * M_PI * fr * t / rate));
        }
        int n = sbc_encode(&e, pcm, out);
        if (n != sbc_frame_len(&e)) { fprintf(stderr, "frame length %d, expected %d\n", n, sbc_frame_len(&e)); return 1; }
        fwrite(out, 1, n, f);
    }
    fclose(f);
    return 0;
}
