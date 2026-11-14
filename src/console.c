/* Text console drawn on the firmware framebuffer (UEFI/BIOS graphics),
 * with a fallback to legacy VGA text mode. Output is mirrored to COM1. */
#include "kernel.h"
#include "font8x16.h"

#define MAX_COLS 480
#define MAX_ROWS 160

bool console_has_fb;
static volatile uint8_t *fb;
static uint32_t pitch, width, height, bpp;
static int scale = 1;
static int cols = 80, rows = 25, cx, cy, fg = C_LGRAY, bg = C_BLACK;
static uint8_t grid_ch[MAX_ROWS][MAX_COLS], grid_fg[MAX_ROWS][MAX_COLS];
static uint32_t pal[16];
static bool cursor_on;

static const uint8_t rgb[16][3] = {
    {16,16,20},{40,80,200},{40,170,80},{40,160,170},{200,50,50},{160,60,170},{170,110,40},{200,200,205},
    {100,100,110},{90,140,255},{90,230,120},{90,220,230},{255,100,100},{230,120,240},{255,210,80},{255,255,255}};

static uint32_t pack(const uint8_t c[3]) {
    struct bootinfo *b = &bootinfo;
    return ((uint32_t)(c[0] >> (8 - b->r_size)) << b->r_pos) |
           ((uint32_t)(c[1] >> (8 - b->g_size)) << b->g_pos) |
           ((uint32_t)(c[2] >> (8 - b->b_size)) << b->b_pos);
}

static inline void px(uint32_t x, uint32_t y, uint32_t c) {
    volatile uint8_t *p = fb + y * pitch + x * (bpp / 8);
    if (bpp == 32) *(volatile uint32_t *)p = c;
    else if (bpp == 24) { p[0] = c; p[1] = c >> 8; p[2] = c >> 16; }
    else if (bpp == 16) *(volatile uint16_t *)p = (uint16_t)c;
}

static void draw_cell(int x, int y, bool inverse) {
    uint8_t ch = grid_ch[y][x];
    if (!console_has_fb) {
        if (fb) ((volatile uint16_t *)fb)[y * 80 + x] = ch | ((inverse ? (grid_fg[y][x] << 4) : grid_fg[y][x]) << 8);
        return;
    }
    static const uint8_t blank[16];
    const uint8_t *g = (ch >= FONT_FIRST && ch <= FONT_LAST) ? font8x16[ch - FONT_FIRST] : blank;
    uint32_t f = pal[grid_fg[y][x]], b = pal[bg];
    if (inverse) { uint32_t t = f; f = b; b = t; }
    uint32_t ox = x * 8 * scale, oy = y * 16 * scale;
    for (int r = 0; r < 16 * scale; r++) {
        uint8_t bits = g[r / scale];
        for (int c = 0; c < 8 * scale; c++)
            px(ox + c, oy + r, (bits & (0x80 >> (c / scale))) ? f : b);
    }
}

static void redraw_all(void) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) draw_cell(x, y, false);
}

void console_init(void) {
    struct bootinfo *b = &bootinfo;
    if (b->fb_addr && b->fb_type == 1 && (b->fb_bpp == 32 || b->fb_bpp == 24 || b->fb_bpp == 16)) {
        console_has_fb = true;
        fb = map_mmio(b->fb_addr, (uint64_t)b->fb_pitch * b->fb_h);
        pitch = b->fb_pitch; width = b->fb_w; height = b->fb_h; bpp = b->fb_bpp;
        scale = width >= 2560 ? 2 : 1;
        cols = width / (8 * scale); rows = height / (16 * scale);
        if (cols > MAX_COLS) cols = MAX_COLS;
        if (rows > MAX_ROWS) rows = MAX_ROWS;
        for (int i = 0; i < 16; i++) pal[i] = pack(rgb[i]);
    } else {
        fb = (volatile uint8_t *)(b->fb_addr ? b->fb_addr : 0xB8000);
        cols = 80; rows = 25;
    }
    console_clear();
}

void console_info(char *buf, size_t n) {
    if (console_has_fb) ksnprintf(buf, n, "%ux%u %u-bit framebuffer, %dx%d text", width, height, bpp, cols, rows);
    else ksnprintf(buf, n, "VGA text mode 80x25");
}

void console_clear(void) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) { grid_ch[y][x] = ' '; grid_fg[y][x] = fg; }
    if (console_has_fb) {
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) px(x, y, pal[bg]);
    } else redraw_all();
    cx = cy = 0;
}

void console_set_color(int c) { fg = c & 15; }

static void scroll(void) {
    memmove(grid_ch[0], grid_ch[1], sizeof(grid_ch[0]) * (rows - 1));
    memmove(grid_fg[0], grid_fg[1], sizeof(grid_fg[0]) * (rows - 1));
    for (int x = 0; x < cols; x++) { grid_ch[rows - 1][x] = ' '; grid_fg[rows - 1][x] = fg; }
    /* Only redraw cells that changed compared to the line below them was drawn as. Simple: redraw all. */
    redraw_all();
    cy = rows - 1;
}

void console_cursor(bool on) {
    cursor_on = on;
    if (cy < rows && cx < cols) draw_cell(cx, cy, on);
}

static void putc_raw(char c) {
    if (cursor_on) draw_cell(cx, cy, false);
    if (c == '\n') { cx = 0; cy++; }
    else if (c == '\r') cx = 0;
    else if (c == '\b') { if (cx > 0) cx--; else if (cy > 0) { cy--; cx = cols - 1; } }
    else if (c == '\t') { do putc_raw(' '); while (cx % 4); return; }
    else {
        grid_ch[cy][cx] = (uint8_t)c; grid_fg[cy][cx] = fg;
        draw_cell(cx, cy, false);
        if (++cx >= cols) { cx = 0; cy++; }
    }
    if (cy >= rows) scroll();
    if (cursor_on) draw_cell(cx, cy, true);
}

/* Kernel log ring buffer (shown by the 'log' command) */
static char logbuf[32768]; static size_t log_pos; static bool log_wrapped;
static bool gui;

void console_gui_mode(void) { gui = true; }

void console_log_read(char *buf, size_t n) {
    size_t k = 0;
    if (log_wrapped) for (size_t i = log_pos; i < sizeof logbuf && k + 1 < n; i++) buf[k++] = logbuf[i];
    for (size_t i = 0; i < log_pos && k + 1 < n; i++) buf[k++] = logbuf[i];
    buf[k] = 0;
}

void console_putc(char c) {
    serial_putc(c);
    logbuf[log_pos++] = c;
    if (log_pos == sizeof logbuf) { log_pos = 0; log_wrapped = true; }
    if (current && current->capture) {
        if (current->cap_len < current->cap_max) current->capture[current->cap_len++] = c;
        return;
    }
    if (!gui) putc_raw(c);
}
void console_write(const char *s) { while (*s) console_putc(*s++); }
