/* Draw an AXIOM logo image (common/logo_data.h) onto an app's canvas in one colour. */
#pragma once
#include "dunc.h"
#include "../../common/logo_data.h"
static inline void draw_logo(struct canvas *c, int x, int y, const struct logo_img *l, uint32_t col) {
    for (int j = 0; j < l->h; j++) {
        int yy = y + j; if (yy < c->cy0 || yy >= c->cy1) continue;
        uint32_t *row = c->px + yy * c->stride;
        for (int i = 0; i < l->w; i++) {
            int xx = x + i; if (xx < c->cx0 || xx >= c->cx1) continue;
            uint32_t a = l->a[j * l->w + i]; if (!a) continue;
            uint32_t d = row[xx];
            row[xx] = (((col >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255) << 16 |
                      (((col >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255) << 8 |
                      (((col & 255) * a + (d & 255) * (255 - a)) / 255);
        }
    }
}
