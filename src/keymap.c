/* Keyboard layouts. The PS/2 and USB keyboard drivers report keys as USB HID usage codes plus the
 * modifier byte; this turns them into characters (Windows-1252) or special keys for the input queue,
 * using the layout chosen in Settings. Handles Shift, Caps Lock, AltGr, Ctrl/Alt shortcuts and dead keys. */
#include "kernel.h"
#include "keymaps.h"

static const struct keymap *km = &keymaps[0];
static int dead = -1;                    /* a dead key (accent) waiting for the next letter */
static bool caps, super_used;

void kbd_set_layout(const char *id) {
    for (int i = 0; i < KM_COUNT; i++) if (!strcmp(keymaps[i].id, id)) { km = &keymaps[i]; dead = -1; return; }
}
const char *kbd_layout(void) { return km->id; }

/* "id=Name;id=Name;..." */
int kbd_layouts(char *out, int n) {
    int k = 0;
    for (int i = 0; i < KM_COUNT && k < n - 1; i++) k += ksnprintf(out + k, n - k, "%s%s=%s", i ? ";" : "", keymaps[i].id, keymaps[i].name);
    return k;
}

static int upper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0x9A || c == 0x9C || c == 0x9E) return c - 16;     /* š œ ž */
    if (c == 0xFF) return 0x9F;                                  /* ÿ */
    return c;
}
static int lower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c == 0x8A || c == 0x8C || c == 0x8E) return c + 16;
    if (c == 0x9F) return 0xFF;
    return c;
}
static bool is_letter(int c) { return upper(c) != c || lower(c) != c; }

static int compose(int accent, int base) {
    for (int i = 0; i < KM_NCOMPOSE; i++) if (km_compose[i][0] == accent && km_compose[i][1] == base) return km_compose[i][2];
    return -1;
}

void kbd_hid_key(uint8_t u, uint8_t mods) {
    bool shift = mods & 0x22, ctrl = mods & 0x11, alt = mods & 0x04, altgr = (mods & 0x40) != 0, gui = mods & 0x88;
    int m = (shift ? KEYMOD_SHIFT : 0) | (ctrl ? KEYMOD_CTRL : 0) | (alt ? KEYMOD_ALT : 0) | (gui ? KEYMOD_SUPER : 0);
    if (gui) super_used = true;
    int special = 0;
    switch (u) {
    case 0x39: caps = !caps; return;
    case 0x4F: special = KEY_RIGHT; break; case 0x50: special = KEY_LEFT; break;
    case 0x51: special = KEY_DOWN; break;  case 0x52: special = KEY_UP; break;
    case 0x4A: special = KEY_HOME; break;  case 0x4D: special = KEY_END; break;
    case 0x4C: special = KEY_DEL; break;   case 0x49: special = KEY_INS; break;
    case 0x4B: special = KEY_PGUP; break;  case 0x4E: special = KEY_PGDN; break;
    case 0x46: special = KEY_PRTSC; break;
    case 0x28: case 0x58: special = '\n'; break;
    case 0x29: special = 27; break;        case 0x2A: special = '\b'; break;
    case 0x2B: special = '\t'; break;
    }
    if (u >= 0x3A && u <= 0x45) special = KEY_F1 + (u - 0x3A);
    if (special) { dead = -1; input_push(special | ((special >= 0x100 || special == '\t' || special == '\n') ? m : 0)); return; }
    int c = 0;
    if (u == 0x2C) c = ' ';
    else if (u >= 0x54 && u <= 0x63) c = "/*-+\n1234567890."[u - 0x54];          /* keypad */
    else {
        int i = -1;
        for (int k = 0; k < KM_KEYS; k++) if (km_usages[k] == u) { i = k; break; }
        if (i < 0) return;
        int lvl = altgr && km->key[i][2] ? 2 : shift ? 1 : 0;
        if (altgr && !km->key[i][2]) return;
        c = km->key[i][lvl];
        if (!c) return;
        if (c >= KM_DEAD) {                                      /* accent key: wait for the letter */
            int a = c - KM_DEAD;
            if (dead == a) { input_push(compose(a, ' ')); dead = -1; }   /* pressed twice: the accent itself */
            else dead = a;
            return;
        }
        if (caps && lvl < 2 && is_letter(c)) c = shift ? lower(c) : upper(c);
    }
    if (dead >= 0) {
        int r = compose(dead, c);
        if (r < 0) { input_push(compose(dead, ' ')); r = c; }    /* no such letter: type the accent, then the key */
        dead = -1; c = r;
    }
    if (gui) { input_push(KEYMOD_SUPER | lower(c)); return; }
    if (ctrl && !altgr) {
        int l = lower(c);
        if (l >= 'a' && l <= 'z') input_push(shift ? (KEYMOD_CTRL | KEYMOD_SHIFT | l) : l - 'a' + 1);
        else input_push(m | c);
        return;
    }
    if (alt && !altgr) { input_push(KEYMOD_ALT | c); return; }
    input_push(c);
}

/* the Windows/Super key on its own (pressed and released with nothing else) opens the menu */
void kbd_super(bool down) {
    if (down) super_used = false;
    else if (!super_used) input_push(KEY_SUPER);
}

/* ---- PS/2 scancode set 1 to USB usage ---- */
static const uint8_t set1[0x59] = {
    0, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
    0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0, 0x04, 0x16,
    0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0, 0x31, 0x1D, 0x1B, 0x06, 0x19,
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0, 0x55, 0, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
    0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
    0x5A, 0x5B, 0x62, 0x63, 0, 0, 0x64, 0x44, 0x45 };

static uint8_t ps2_mods; static bool ps2_ext;

void kbd_ps2_scancode(uint8_t sc) {
    if (sc == 0xE0) { ps2_ext = true; return; }
    bool up = sc & 0x80, ext = ps2_ext; sc &= 0x7F; ps2_ext = false;
    uint8_t bit = 0;
    if (!ext) bit = sc == 0x1D ? 0x01 : sc == 0x2A ? 0x02 : sc == 0x38 ? 0x04 : sc == 0x36 ? 0x20 : 0;
    else bit = sc == 0x1D ? 0x10 : sc == 0x38 ? 0x40 : sc == 0x5B ? 0x08 : sc == 0x5C ? 0x80 : 0;
    if (bit) {
        if (up) ps2_mods &= ~bit; else ps2_mods |= bit;
        if (bit & 0x88) kbd_super(!up);
        return;
    }
    if (up) return;
    uint8_t u = 0;
    if (!ext) u = sc < sizeof set1 ? set1[sc] : 0;
    else switch (sc) {
        case 0x1C: u = 0x58; break; case 0x35: u = 0x54; break; case 0x37: u = 0x46; break;
        case 0x47: u = 0x4A; break; case 0x48: u = 0x52; break; case 0x49: u = 0x4B; break;
        case 0x4B: u = 0x50; break; case 0x4D: u = 0x4F; break; case 0x4F: u = 0x4D; break;
        case 0x50: u = 0x51; break; case 0x51: u = 0x4E; break; case 0x52: u = 0x49; break;
        case 0x53: u = 0x4C; break;
    }
    if (u) kbd_hid_key(u, ps2_mods);
}
