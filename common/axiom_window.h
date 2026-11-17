/* AXIOM windows for Linux programs (the Linux compatibility layer, src/linux.c).
 *
 * A Linux program running on AXIOM opens a window as a file:
 *
 *     int fd = open("/dev/axiom/window", O_RDWR | O_CLOEXEC);
 *     struct axwin_create c = { .w = 800, .h = 600, .flags = AXWIN_RESIZABLE, .title = "Hello" };
 *     ioctl(fd, AXWIN_CREATE, &c);                              // the window appears
 *     struct axwin_info i; ioctl(fd, AXWIN_INFO, &i);
 *     uint32_t *px = mmap(0, i.stride * i.h, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
 *     ... draw: px[y * i.stride / 4 + x] = 0x00RRGGBB (XRGB8888; in memory B, G, R, X: Skia's BGRA) ...
 *     struct axwin_rect r = { 0, 0, i.w, i.h }; ioctl(fd, AXWIN_FLUSH, &r);   // show that part
 *
 * Input: read(fd) gives whole struct axwin_event records (poll/epoll: readable when some are waiting).
 * Resizing (only with AXWIN_RESIZABLE): an AXWIN_EV_RESIZE event asks for a new size; the program
 * munmaps its pixels, calls AXWIN_RESIZE (fails with EBUSY while they're still mapped), then
 * AXWIN_INFO and mmap again. The window goes away when the fd and every mapping of it are gone.
 *
 * This header is plain C with no AXIOM dependencies: copy it into ported programs as it is. */
#ifndef AXIOM_WINDOW_H
#define AXIOM_WINDOW_H
#include <stdint.h>

#define AXWIN_RESIZABLE   1u
#define AXWIN_POPUP       2u   /* a menu, list or tooltip: no frame, above the other windows, never takes the keyboard;
                                  placed with AXWIN_MOVE (screen coordinates), shown and hidden with AXWIN_SHOW */
#define AXWIN_MOVE_EVENTS 4u   /* send AXWIN_EV_MOVE whenever the window's content moves on the screen */
#define AXWIN_HIDDEN      8u   /* made hidden; AXWIN_SHOW shows it */

struct axwin_create { int32_t w, h; uint32_t flags; char title[64]; };
struct axwin_info { int32_t id, w, h, stride; int32_t screen_w, screen_h; };   /* stride in bytes */
struct axwin_rect { int32_t x, y, w, h; };

/* ioctl requests ('A' 'X' then a number) */
#define AXWIN_CREATE 0x41580001u   /* struct axwin_create *: make the window (once per fd) */
#define AXWIN_INFO   0x41580002u   /* struct axwin_info *: its size and the screen's (before AXWIN_CREATE: id, w, h 0) */
#define AXWIN_FLUSH  0x41580003u   /* struct axwin_rect *: show what was drawn there */
#define AXWIN_TITLE  0x41580004u   /* char[64]: a new title */
#define AXWIN_RESIZE 0x41580005u   /* (no argument): take the size the last AXWIN_EV_RESIZE asked for */
#define AXWIN_MOVE   0x41580006u   /* struct axwin_rect * (x, y): put the content there on the screen; before
                                      AXWIN_CREATE: where it will appear (w, h are ignored) */
#define AXWIN_SHOW   0x41580007u   /* int *: 1 show, 0 hide */
#define AXWIN_CURSOR 0x41580008u   /* int *: the pointer's shape over the window (AXWIN_CURSOR_*) */
#define AXWIN_POS    0x41580009u   /* struct axwin_rect * (out): where the content is on the screen, and its size */

enum { AXWIN_CURSOR_ARROW, AXWIN_CURSOR_TEXT, AXWIN_CURSOR_HAND, AXWIN_CURSOR_WAIT, AXWIN_CURSOR_CROSSHAIR,
       AXWIN_CURSOR_MOVE, AXWIN_CURSOR_RESIZE_EW, AXWIN_CURSOR_RESIZE_NS, AXWIN_CURSOR_RESIZE_NWSE,
       AXWIN_CURSOR_RESIZE_NESW, AXWIN_CURSOR_NOT_ALLOWED, AXWIN_CURSOR_NONE };
/* The clipboard (shared with every AXIOM app), as UTF-8 text. Works on any open /dev/axiom/window fd, with or
 * without a window. seq goes up whenever anything is copied (by any app), so a program can tell whether
 * the clipboard changed since it last looked. */
struct axwin_clip { uint64_t data; uint64_t len; uint64_t seq; };   /* data: a pointer, as a number */
#define AXWIN_CLIP_SET 0x4158000Au /* struct axwin_clip *: copy len bytes from data (at most 1 MiB); seq comes back */
#define AXWIN_CLIP_GET 0x4158000Bu /* struct axwin_clip *: data, len = room there; comes back with len = the whole
                                      text's length (copied: as much as fits) and seq. data 0: just len and seq */
#define AXWIN_APP    0x4158000Cu   /* int *: the process this window is really for (a compositor showing an app's
                                      window): the top bar and the dock show it as that app */

/* events: the same layout as AXIOM's own (struct dunc_event) */
struct axwin_event { int32_t type, win, x, y, key, button; };
enum {
    AXWIN_EV_KEY = 1,          /* key: a character 1..255 (Windows-1252; Ctrl+A..Z = 1..26) or AXWIN_KEY_*,
                                  plus AXWIN_MOD_* bits for modifiers held with special keys; the character is
                                  from the keyboard layout chosen in AXIOM's Settings (accents typed with dead
                                  keys arrive composed). button: the physical key as a USB HID usage code
                                  (0x04 = A, the key left of Caps Lock's row...), 0 if not known */
    AXWIN_EV_MOUSE_DOWN,       /* x, y in the window's pixels; button: 1 left, 2 right */
    AXWIN_EV_MOUSE_UP,
    AXWIN_EV_MOUSE_MOVE,
    AXWIN_EV_CLOSE,            /* the close button: the program decides */
    AXWIN_EV_FOCUS,
    AXWIN_EV_BLUR,
    AXWIN_EV_MOUSE_LEAVE,
    AXWIN_EV_WHEEL,            /* x, y: the pointer; key: steps, positive = down (reversed with natural scrolling) */
    AXWIN_EV_RESIZE,           /* x, y: the size asked for (see above) */
    AXWIN_EV_MOVE = 12,        /* x, y: where the content is now on the screen (with AXWIN_MOVE_EVENTS) */
};
enum { AXWIN_KEY_UP = 0x100, AXWIN_KEY_DOWN, AXWIN_KEY_LEFT, AXWIN_KEY_RIGHT, AXWIN_KEY_HOME, AXWIN_KEY_END,
       AXWIN_KEY_DEL, AXWIN_KEY_PGUP, AXWIN_KEY_PGDN, AXWIN_KEY_INS, AXWIN_KEY_PRTSC,
       AXWIN_KEY_F1, AXWIN_KEY_F2, AXWIN_KEY_F3, AXWIN_KEY_F4, AXWIN_KEY_F5, AXWIN_KEY_F6, AXWIN_KEY_F7,
       AXWIN_KEY_F8, AXWIN_KEY_F9, AXWIN_KEY_F10, AXWIN_KEY_F11, AXWIN_KEY_F12, AXWIN_KEY_SUPER };
#define AXWIN_MOD_SHIFT 0x1000
#define AXWIN_MOD_CTRL  0x2000
#define AXWIN_MOD_ALT   0x4000
#define AXWIN_MOD_SUPER 0x8000

#endif
