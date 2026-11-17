/* axwl: AXIOM's Wayland compositor. A Linux program (running on AXIOM's Linux layer) that lets Wayland apps
 * (GTK, Qt, SDL, ...) show their windows as AXIOM windows:
 *   - each xdg_toplevel is an AXIOM window (backend.h); AXIOM draws the frame (apps are told to leave
 *     decorations to the server: xdg-decoration and KDE's server-decoration, which GTK 3 uses)
 *   - buffers are wl_shm (shared memory): copied on commit, then released; subsurfaces and popups (menus) are
 *     drawn into their toplevel's window
 *   - AXIOM's mouse and key events become wl_pointer / wl_keyboard events (evdev key codes, a US keymap)
 * Run it with WAYLAND_DISPLAY / XDG_RUNTIME_DIR as the apps will see them (AXIOM sets these for Linux programs). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "backend.h"
#include "xdg-shell-server.h"
#include "xdg-decoration-unstable-v1-server.h"
#include "server-decoration-server.h"
#include "keymap.h"

static struct wl_display *dpy;
static struct wl_event_loop *loop;
static int screen_w = 1920, screen_h = 1080;
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
#define LOG(...) do { if (getenv("AXWL_DEBUG")) fprintf(stderr, "[axwl] " __VA_ARGS__); } while (0)

/* ---------------- surfaces ---------------- */
enum { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_SUB, ROLE_CURSOR };
struct toplevel;
struct surface {
    struct wl_resource *res;
    int role;
    /* pending state (applied on commit) */
    struct wl_resource *pbuf; bool pattached; int pdx, pdy;
    struct wl_list pframes, frames;                    /* frame callbacks: pending, waiting for the next present */
    /* current contents: a copy of the last buffer (premultiplied ARGB) */
    uint32_t *img; int w, h, scale, pscale;
    /* xdg */
    struct wl_resource *xdg_surface, *role_res;        /* the xdg_surface; the xdg_toplevel / xdg_popup */
    int gx, gy, gw, gh, pgx, pgy, pgw, pgh; bool has_geom, phas_geom;
    bool configured, mapped;
    struct toplevel *top;                              /* the toplevel this surface is drawn into */
    /* popups */
    struct surface *parent; int px, py;                /* position relative to the parent's window geometry */
    bool grab;
    /* subsurfaces */
    struct wl_list subs;                               /* children: struct sub.link */
    struct sub *as_sub;
    struct wl_list link;                               /* in all_surfaces */
};
struct sub { struct surface *s, *parent; int x, y, px, py; struct wl_list link; struct wl_resource *res; };
struct toplevel {
    struct surface *s; struct bwin *win; char title[64];
    int ww, wh;                                        /* the window's size */
    struct wl_list popups;                             /* struct popup.link, oldest first */
    bool resize_pending, acked; int want_w, want_h; uint32_t cfg_serial;
    struct wl_event_source *src;
    bool dirty, frame_due;
};
struct popup { struct surface *s; struct wl_list link; };
static struct wl_list all_surfaces;

/* xdg_positioner state */
struct positioner { int w, h, ax, ay, aw, ah, anchor, gravity, ox, oy; uint32_t adjust; };

static struct surface *surf_of(struct wl_resource *r) { return r ? wl_resource_get_user_data(r) : 0; }
static void present(struct toplevel *t);
static void arm_frames(void);
static struct wl_event_source *frame_timer; static bool frames_armed;
static void send_frame_done(struct surface *s, uint32_t t) {
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frames) { wl_callback_send_done(cb, t); wl_resource_destroy(cb); }
    struct sub *c;
    wl_list_for_each(c, &s->subs, link) send_frame_done(c->s, t);
}

/* ---------------- drawing: a toplevel's window = its surface (cropped to the window geometry), its
 * subsurfaces, then its popups ---------------- */
static void blit(uint32_t *dst, int dw, int dh, int dstride, struct surface *s, int x, int y) {
    if (!s->img) return;
    for (int j = 0; j < s->h; j++) {
        int yy = y + j; if (yy < 0 || yy >= dh) continue;
        uint32_t *d = dst + (size_t)yy * dstride, *src = s->img + (size_t)j * s->w;
        int i0 = x < 0 ? -x : 0, i1 = s->w; if (x + i1 > dw) i1 = dw - x;
        for (int i = i0; i < i1; i++) {
            uint32_t p = src[i], a = p >> 24;
            if (a == 255) d[x + i] = p & 0xFFFFFF;
            else if (a) {                                      /* premultiplied over the background */
                uint32_t q = d[x + i], ia = 255 - a;
                uint32_t r = ((p >> 16) & 255) + ((q >> 16) & 255) * ia / 255, g = ((p >> 8) & 255) + ((q >> 8) & 255) * ia / 255, b = (p & 255) + (q & 255) * ia / 255;
                d[x + i] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
        }
    }
}
static void draw_tree(uint32_t *px, int w, int h, int stride, struct surface *s, int x, int y) {
    blit(px, w, h, stride, s, x, y);
    struct sub *c;
    wl_list_for_each(c, &s->subs, link) draw_tree(px, w, h, stride, c->s, x + c->x, y + c->y);
}
static void geom(struct surface *s, int *x, int *y, int *w, int *h) {
    if (s->has_geom && s->gw > 0 && s->gh > 0) { *x = s->gx; *y = s->gy; *w = s->gw; *h = s->gh; }
    else { *x = 0; *y = 0; *w = s->w; *h = s->h; }
}
/* where a popup's surface sits in its toplevel's window */
static void popup_origin(struct surface *p, int *ox, int *oy) {
    struct surface *par = p->parent; int x = 0, y = 0;
    int pgx, pgy, pgw, pgh; geom(par, &pgx, &pgy, &pgw, &pgh);
    if (par->role == ROLE_POPUP) popup_origin(par, &x, &y);
    else { int tx, ty, tw, th; geom(par, &tx, &ty, &tw, &th); x = -tx; y = -ty; }
    int gx, gy, gw, gh; geom(p, &gx, &gy, &gw, &gh);
    *ox = x + pgx + p->px - gx; *oy = y + pgy + p->py - gy;
}
static void present(struct toplevel *t) {
    struct surface *s = t->s;
    if (!s || !s->img) return;
    int gx, gy, gw, gh; geom(s, &gx, &gy, &gw, &gh);
    if (!t->win) {
        t->win = be_create(gw, gh, t->title);
        if (!t->win) { fprintf(stderr, "axwl: couldn't make a window\n"); return; }
        t->ww = gw; t->wh = gh;
        extern void watch_window(struct toplevel *t);
        watch_window(t);
    }
    int w, h, stride; uint32_t *px = be_pixels(t->win, &w, &h, &stride);
    if (!px) return;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px[(size_t)j * stride + i] = 0x202020;
    draw_tree(px, w, h, stride, s, -gx, -gy);
    struct popup *p;
    wl_list_for_each(p, &t->popups, link) if (p->s->mapped) { int ox, oy; popup_origin(p->s, &ox, &oy); draw_tree(px, w, h, stride, p->s, ox, oy); }
    be_flush(t->win, 0, 0, w, h);
    t->dirty = false;
    t->frame_due = true; arm_frames();               /* frame callbacks go out at most 60 times a second */
}
/* the frame timer: tell each shown toplevel (and its popups) that its last picture is on screen */
static int frames_tick(void *data) {
    frames_armed = false;
    uint32_t tm = now_ms();
    struct surface *s;
    wl_list_for_each(s, &all_surfaces, link) if (s->role == ROLE_TOPLEVEL && s->top && s->top->frame_due) {
        struct toplevel *t = s->top; t->frame_due = false;
        send_frame_done(s, tm);
        struct popup *p; wl_list_for_each(p, &t->popups, link) send_frame_done(p->s, tm);
    }
    wl_display_flush_clients(dpy);
    return 0;
}
static void arm_frames(void) {
    if (frames_armed) return;
    if (!frame_timer) frame_timer = wl_event_loop_add_timer(loop, frames_tick, 0);
    if (!frame_timer) { frames_tick(0); return; }     /* (no timers: answer at once) */
    frames_armed = true;
    wl_event_source_timer_update(frame_timer, 16);
}

/* ---------------- wl_surface ---------------- */
static void surface_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    struct surface *s = surf_of(r); s->pbuf = buf; s->pattached = true; s->pdx = x; s->pdy = y;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void unlink_cb(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = surf_of(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(cb, 0, 0, unlink_cb);
    wl_list_insert(s->pframes.prev, wl_resource_get_link(cb));
}
static void surface_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) {}
static struct toplevel *top_of(struct surface *s) {
    while (s && s->role == ROLE_SUB && s->as_sub) s = s->as_sub->parent;
    while (s && s->role == ROLE_POPUP) s = s->parent;
    return s ? s->top : 0;
}
static void copy_buffer(struct surface *s) {
    struct wl_shm_buffer *b = wl_shm_buffer_get(s->pbuf);
    if (!b) { LOG("not a shm buffer\n"); return; }
    int w = wl_shm_buffer_get_width(b), h = wl_shm_buffer_get_height(b), st = wl_shm_buffer_get_stride(b);
    uint32_t fmt = wl_shm_buffer_get_format(b);
    if (w != s->w || h != s->h || !s->img) { free(s->img); s->img = malloc((size_t)w * h * 4); s->w = w; s->h = h; }
    wl_shm_buffer_begin_access(b);
    uint8_t *d = wl_shm_buffer_get_data(b);
    for (int j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)(d + (size_t)j * st), *o = s->img + (size_t)j * w;
        if (fmt == WL_SHM_FORMAT_XRGB8888) for (int i = 0; i < w; i++) o[i] = row[i] | 0xFF000000u;
        else memcpy(o, row, (size_t)w * 4);
    }
    wl_shm_buffer_end_access(b);
}
static void configure_initial(struct surface *s);
static void surface_commit(struct wl_client *c, struct wl_resource *r) {
    struct surface *s = surf_of(r);
    if (s->pattached) {
        if (s->pbuf) { copy_buffer(s); wl_buffer_send_release(s->pbuf); }
        else { free(s->img); s->img = 0; s->w = s->h = 0; if (s->role == ROLE_POPUP) s->mapped = false; }
        s->pattached = false; s->pbuf = 0;
    }
    if (s->phas_geom) { s->gx = s->pgx; s->gy = s->pgy; s->gw = s->pgw; s->gh = s->pgh; s->has_geom = true; s->phas_geom = false; }
    wl_list_insert_list(s->frames.prev, &s->pframes); wl_list_init(&s->pframes);
    if (s->as_sub) { s->as_sub->x = s->as_sub->px; s->as_sub->y = s->as_sub->py; }
    if (s->xdg_surface && !s->configured) { configure_initial(s); return; }   /* the first commit: configure first */
    if ((s->role == ROLE_TOPLEVEL || s->role == ROLE_POPUP) && s->img) s->mapped = true;
    struct toplevel *t = top_of(s);
    if (t && t->s && t->s->mapped) {
        if (t->win && t->resize_pending && t->acked && s == t->s) {   /* the app has drawn for the new size */
            if (be_resize(t->win) == 0) { t->ww = t->want_w; t->wh = t->want_h; }   /* (if it keeps to a min/max size, its
                                                                         picture is cropped or padded) */
            t->resize_pending = false;
        }
        present(t);
    } else if (!t || !t->win) send_frame_done(s, now_ms());           /* not shown yet: don't keep it waiting */
}
static void surface_set_transform(struct wl_client *c, struct wl_resource *r, int32_t t) {}
static void surface_set_scale(struct wl_client *c, struct wl_resource *r, int32_t sc) {}
static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) {}
static const struct wl_surface_interface surface_impl = {
    surface_destroy, surface_attach, surface_damage, surface_frame, surface_region, surface_region, surface_commit,
    surface_set_transform, surface_set_scale, surface_damage_buffer, surface_offset };
static void forget_focus(struct surface *s);
static struct popup *popup_entry(struct surface *s);
static void surface_free(struct wl_resource *r) {
    struct surface *s = surf_of(r);
    forget_focus(s);
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->pframes) wl_resource_destroy(cb);
    wl_resource_for_each_safe(cb, tmp, &s->frames) wl_resource_destroy(cb);
    struct sub *c, *ct;
    wl_list_for_each_safe(c, ct, &s->subs, link) { wl_list_remove(&c->link); wl_list_init(&c->link); c->parent = 0; }
    if (s->as_sub) { wl_list_remove(&s->as_sub->link); wl_list_init(&s->as_sub->link); s->as_sub->s = 0; }
    /* objects that still point at this surface (a client may destroy them in any order) */
    if (s->xdg_surface) wl_resource_set_user_data(s->xdg_surface, 0);
    if (s->role == ROLE_TOPLEVEL && s->top) {
        struct toplevel *t = s->top;
        if (t->src) { wl_event_source_remove(t->src); t->src = 0; }
        if (t->win) { be_destroy(t->win); t->win = 0; }
        t->s = 0;
        struct popup *p, *pt;
        wl_list_for_each_safe(p, pt, &t->popups, link) { wl_list_remove(&p->link); wl_list_init(&p->link); p->s->parent = 0; p->s->mapped = false; free(p); }
    }
    if (s->role == ROLE_POPUP) {
        struct popup *p = s->parent ? popup_entry(s) : 0;
        if (p) { wl_list_remove(&p->link); free(p); }
        if (s->role_res) wl_resource_set_user_data(s->role_res, 0);
    }
    struct surface *o;
    wl_list_for_each(o, &all_surfaces, link) if (o->parent == s) { o->parent = 0; o->mapped = false; }
    wl_list_remove(&s->link);
    free(s->img); free(s);
}

/* ---------------- wl_compositor, wl_region ---------------- */
static void region_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void region_box(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {}
static const struct wl_region_interface region_impl = { region_destroy, region_box, region_box };
static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = calloc(1, sizeof *s);
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &surface_impl, s, surface_free);
    wl_list_init(&s->pframes); wl_list_init(&s->frames); wl_list_init(&s->subs);
    s->scale = s->pscale = 1;
    wl_list_insert(&all_surfaces, &s->link);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *g = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(g, &region_impl, 0, 0);
}
static const struct wl_compositor_interface comp_impl = { comp_create_surface, comp_create_region };
static void bind_comp(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, v, id);
    wl_resource_set_implementation(r, &comp_impl, 0, 0);
}

/* ---------------- wl_subcompositor ---------------- */
static void sub_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void sub_set_position(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { struct sub *b = wl_resource_get_user_data(r); b->px = x; b->py = y; }
static void sub_place(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib) {}
static void sub_sync(struct wl_client *c, struct wl_resource *r) {}
static const struct wl_subsurface_interface sub_impl = { sub_destroy, sub_set_position, sub_place, sub_place, sub_sync, sub_sync };
static void sub_free(struct wl_resource *r) {
    struct sub *b = wl_resource_get_user_data(r);
    wl_list_remove(&b->link);
    if (b->s) { b->s->as_sub = 0; b->s->role = ROLE_NONE; }
    free(b);
}
static void subc_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void subc_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf, struct wl_resource *par) {
    struct sub *b = calloc(1, sizeof *b);
    b->s = surf_of(surf); b->parent = surf_of(par);
    b->res = wl_resource_create(c, &wl_subsurface_interface, 1, id);
    wl_resource_set_implementation(b->res, &sub_impl, b, sub_free);
    b->s->role = ROLE_SUB; b->s->as_sub = b;
    wl_list_insert(b->parent->subs.prev, &b->link);
}
static const struct wl_subcompositor_interface subc_impl = { subc_destroy, subc_get };
static void bind_subc(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, v, id);
    wl_resource_set_implementation(r, &subc_impl, 0, 0);
}

/* ---------------- wl_output ---------------- */
static void output_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = { output_release };
static void bind_output(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, v, id);
    wl_resource_set_implementation(r, &output_impl, 0, 0);
    wl_output_send_geometry(r, 0, 0, 520, 290, WL_OUTPUT_SUBPIXEL_UNKNOWN, "AXIOM", "Screen", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, screen_w, screen_h, 60000);
    if (v >= 2) wl_output_send_scale(r, 1);
    if (v >= 4) { wl_output_send_name(r, "AXIOM-1"); wl_output_send_description(r, "AXIOM screen"); }
    if (v >= 2) wl_output_send_done(r);
}

/* ---------------- seat: pointer and keyboard ---------------- */
static struct wl_list pointers, keyboards;                 /* resources (all clients) */
static struct surface *ptr_focus, *kbd_focus; static double ptr_x, ptr_y;
static uint32_t mods_depressed;
#define EACH_FOR(list, surf, r) wl_resource_for_each(r, list) if (surf && wl_resource_get_client(r) == wl_resource_get_client((surf)->res))
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *s, int32_t x, int32_t y) {
    if (s) surf_of(s)->role = ROLE_CURSOR;               /* (AXIOM draws its own pointer) */
}
static void res_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = { ptr_set_cursor, res_release };
static const struct wl_keyboard_interface keyboard_impl = { res_release };
static void unlink_res(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void send_keymap(struct wl_resource *k) {
    size_t n = sizeof keymap_us;
    int fd = memfd_create("axwl-keymap", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)n) < 0) { if (fd >= 0) close(fd); return; }
    void *m = mmap(0, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m != MAP_FAILED) { memcpy(m, keymap_us, n); munmap(m, n); }
    wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)n);
    close(fd);
}
static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &pointer_impl, 0, unlink_res);
    wl_list_insert(&pointers, wl_resource_get_link(p));
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(k, &keyboard_impl, 0, unlink_res);
    wl_list_insert(&keyboards, wl_resource_get_link(k));
    send_keymap(k);
    if (wl_resource_get_version(k) >= 4) wl_keyboard_send_repeat_info(k, 25, 600);
    if (kbd_focus && wl_resource_get_client(kbd_focus->res) == c) {
        struct wl_array keys; wl_array_init(&keys);
        wl_keyboard_send_enter(k, wl_display_next_serial(dpy), kbd_focus->res, &keys);
        wl_array_release(&keys);
    }
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) {}
static const struct wl_seat_interface seat_impl = { seat_get_pointer, seat_get_keyboard, seat_get_touch, res_release };
static void bind_seat(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);
    wl_resource_set_implementation(r, &seat_impl, 0, 0);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (v >= 2) wl_seat_send_name(r, "seat0");
}
static void ptr_frame(struct surface *s) { struct wl_resource *p; EACH_FOR(&pointers, s, p) if (wl_resource_get_version(p) >= 5) wl_pointer_send_frame(p); }
static void set_pointer_focus(struct surface *s, double sx, double sy) {
    if (s == ptr_focus) return;
    struct wl_resource *p;
    if (ptr_focus) { EACH_FOR(&pointers, ptr_focus, p) wl_pointer_send_leave(p, wl_display_next_serial(dpy), ptr_focus->res); ptr_frame(ptr_focus); }
    ptr_focus = s;
    if (s) { EACH_FOR(&pointers, s, p) wl_pointer_send_enter(p, wl_display_next_serial(dpy), s->res, wl_fixed_from_double(sx), wl_fixed_from_double(sy)); ptr_frame(s); }
}
static void send_modifiers(struct surface *s) {
    struct wl_resource *k;
    EACH_FOR(&keyboards, s, k) wl_keyboard_send_modifiers(k, wl_display_next_serial(dpy), mods_depressed, 0, 0, 0);
}
static void set_keyboard_focus(struct surface *s) {
    if (s == kbd_focus) return;
    struct wl_resource *k;
    if (kbd_focus) EACH_FOR(&keyboards, kbd_focus, k) wl_keyboard_send_leave(k, wl_display_next_serial(dpy), kbd_focus->res);
    kbd_focus = s;
    if (s) {
        struct wl_array keys; wl_array_init(&keys);
        EACH_FOR(&keyboards, s, k) wl_keyboard_send_enter(k, wl_display_next_serial(dpy), s->res, &keys);
        wl_array_release(&keys);
        send_modifiers(s);
        extern void clipboard_sync_focus(void); clipboard_sync_focus();
    }
}
static void forget_focus(struct surface *s) { if (ptr_focus == s) ptr_focus = 0; if (kbd_focus == s) kbd_focus = 0; }

/* AXIOM key events -> evdev key codes on a US layout (shift / ctrl added as needed) */
enum { K_ESC = 1, K_BACKSPACE = 14, K_TAB = 15, K_ENTER = 28, K_LCTRL = 29, K_LSHIFT = 42, K_LALT = 56, K_SPACE = 57, K_LMETA = 125 };
static const char *row_keys[] = { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "\\zxcvbnm,./" };
static const char *row_shift[] = { "!@#$%^&*()_+", "QWERTYUIOP{}", "ASDFGHJKL:\"~", "|ZXCVBNM<>?" };
static const int row_base[] = { 2, 16, 30, 43 };
static int char_key(int ch, bool *shift) {
    *shift = false;
    if (ch == ' ') return K_SPACE;
    if (ch == '\n' || ch == '\r') return K_ENTER;
    if (ch == '\t') return K_TAB;
    if (ch == '\b') return K_BACKSPACE;
    if (ch == 27) return K_ESC;
    for (int r = 0; r < 4; r++) {
        const char *p = strchr(row_keys[r], ch); if (p && ch) return row_base[r] + (int)(p - row_keys[r]);
        p = strchr(row_shift[r], ch); if (p && ch) { *shift = true; return row_base[r] + (int)(p - row_shift[r]); }
    }
    return 0;
}
static int special_key(int code) {
    static const int map[] = { 103, 108, 105, 106, 102, 107, 111, 104, 109, 110, 99, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88, 125 };
    int i = code - AXWIN_KEY_UP;
    return i >= 0 && i < (int)(sizeof map / sizeof map[0]) ? map[i] : 0;
}
static void key_event(struct surface *s, int axkey) {
    if (!s) return;
    int code = axkey & 0xFFF, mods = axkey & 0xF000; bool shift = false; int ctrl = mods & AXWIN_MOD_CTRL, alt = mods & AXWIN_MOD_ALT;
    int k;
    if (code >= 0x100) { k = special_key(code); shift = mods & AXWIN_MOD_SHIFT; }
    else if (code >= 1 && code <= 26 && code != '\t' && code != '\n' && code != '\b' && code != '\r') { k = row_base[0]; ctrl = 1; k = char_key('a' + code - 1, &shift); shift = false; }
    else k = char_key(code, &shift);
    if (!k) { LOG("no key for %d\n", axkey); return; }
    struct wl_resource *kb; uint32_t t = now_ms();
    uint32_t m = (shift ? 1u : 0) | (ctrl ? 4u : 0) | (alt ? 8u : 0);   /* Shift, Control, Mod1 */
    mods_depressed = m; send_modifiers(s);
    EACH_FOR(&keyboards, s, kb) {
        if (shift) wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, K_LSHIFT, 1);
        if (ctrl) wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, K_LCTRL, 1);
        if (alt) wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, K_LALT, 1);
        wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, (uint32_t)k, 1);
        wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, (uint32_t)k, 0);
        if (alt) wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, K_LALT, 0);
        if (ctrl) wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, K_LCTRL, 0);
        if (shift) wl_keyboard_send_key(kb, wl_display_next_serial(dpy), t, K_LSHIFT, 0);
    }
    mods_depressed = 0; send_modifiers(s);
}

/* ---------------- xdg_wm_base: toplevels and popups ---------------- */
static void send_toplevel_configure(struct toplevel *t, int w, int h) {
    struct wl_array st; wl_array_init(&st);
    uint32_t *a = wl_array_add(&st, sizeof *a); *a = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(t->s->role_res, w, h, &st);
    wl_array_release(&st);
    t->cfg_serial = wl_display_next_serial(dpy);
    xdg_surface_send_configure(t->s->xdg_surface, t->cfg_serial);
}
static void configure_initial(struct surface *s) {
    s->configured = true;
    if (s->role == ROLE_TOPLEVEL) send_toplevel_configure(s->top, 0, 0);
    else if (s->role == ROLE_POPUP) {
        int gx, gy, gw, gh; geom(s, &gx, &gy, &gw, &gh);
        xdg_popup_send_configure(s->role_res, s->px, s->py, s->gw ? s->gw : gw, s->gh ? s->gh : gh);
        xdg_surface_send_configure(s->xdg_surface, wl_display_next_serial(dpy));
    } else xdg_surface_send_configure(s->xdg_surface, wl_display_next_serial(dpy));
}
/* toplevel requests */
static void tl_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void tl_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) {}
static void tl_set_title(struct wl_client *c, struct wl_resource *r, const char *title) {
    struct toplevel *t = wl_resource_get_user_data(r);
    strncpy(t->title, title, sizeof t->title - 1);
    if (t->win) be_title(t->win, t->title);
}
static void tl_set_app_id(struct wl_client *c, struct wl_resource *r, const char *id) {}
static void tl_show_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {}
static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {}
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {}
static void tl_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {}
static void tl_noarg(struct wl_client *c, struct wl_resource *r) {}
static void tl_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) {}
static const struct xdg_toplevel_interface toplevel_impl = { tl_destroy, tl_set_parent, tl_set_title, tl_set_app_id, tl_show_menu, tl_move, tl_resize,
    tl_size, tl_size, tl_noarg, tl_noarg, tl_fullscreen, tl_noarg, tl_noarg };
static void toplevel_free(struct wl_resource *r) {
    struct toplevel *t = wl_resource_get_user_data(r);
    if (t->src) wl_event_source_remove(t->src);
    if (t->win) be_destroy(t->win);
    if (t->s) { t->s->top = 0; t->s->role_res = 0; t->s->mapped = false; }
    struct popup *p, *pt;
    wl_list_for_each_safe(p, pt, &t->popups, link) { wl_list_remove(&p->link); p->s->parent = 0; p->s->mapped = false; free(p); }
    free(t);
}
/* popup requests */
static struct popup *popup_entry(struct surface *s) {
    struct toplevel *t = top_of(s->parent); if (!t) return 0;
    struct popup *p; wl_list_for_each(p, &t->popups, link) if (p->s == s) return p;
    return 0;
}
static void pop_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void pop_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    struct surface *s = wl_resource_get_user_data(r);
    if (s) { s->grab = true; set_keyboard_focus(s); }
}
static void pop_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *pos, uint32_t token) {}
static const struct xdg_popup_interface popup_impl = { pop_destroy, pop_grab, pop_reposition };
static void popup_free(struct wl_resource *r) {
    struct surface *s = wl_resource_get_user_data(r);
    if (!s) return;
    struct toplevel *t = s->parent ? top_of(s->parent) : 0;
    struct popup *p = s->parent ? popup_entry(s) : 0;
    if (p) { wl_list_remove(&p->link); free(p); }
    if (kbd_focus == s) set_keyboard_focus(s->parent);
    if (ptr_focus == s) ptr_focus = 0;
    s->role_res = 0; s->mapped = false; s->parent = 0;
    if (t && t->s && t->s->mapped) present(t);
}
/* where a popup goes: the positioner's anchor point on the parent, then the gravity */
static void place(struct positioner *p, int *x, int *y) {
    int ax = p->ax + p->aw / 2, ay = p->ay + p->ah / 2;
    switch (p->anchor) {                               /* none top bottom left right top_left bottom_left top_right bottom_right */
    case 1: ay = p->ay; break; case 2: ay = p->ay + p->ah; break; case 3: ax = p->ax; break; case 4: ax = p->ax + p->aw; break;
    case 5: ax = p->ax; ay = p->ay; break; case 6: ax = p->ax; ay = p->ay + p->ah; break;
    case 7: ax = p->ax + p->aw; ay = p->ay; break; case 8: ax = p->ax + p->aw; ay = p->ay + p->ah; break;
    }
    int px = ax - p->w / 2, py = ay - p->h / 2;
    switch (p->gravity) {
    case 1: py = ay - p->h; break; case 2: py = ay; break; case 3: px = ax - p->w; break; case 4: px = ax; break;
    case 5: px = ax - p->w; py = ay - p->h; break; case 6: px = ax - p->w; py = ay; break;
    case 7: px = ax; py = ay - p->h; break; case 8: px = ax; py = ay; break;
    }
    *x = px + p->ox; *y = py + p->oy;
}
/* positioner */
static void pos_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void pos_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { struct positioner *p = wl_resource_get_user_data(r); p->w = w; p->h = h; }
static void pos_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { struct positioner *p = wl_resource_get_user_data(r); p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
static void pos_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) { ((struct positioner *)wl_resource_get_user_data(r))->anchor = (int)a; }
static void pos_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g) { ((struct positioner *)wl_resource_get_user_data(r))->gravity = (int)g; }
static void pos_adjust(struct wl_client *c, struct wl_resource *r, uint32_t a) { ((struct positioner *)wl_resource_get_user_data(r))->adjust = a; }
static void pos_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { struct positioner *p = wl_resource_get_user_data(r); p->ox = x; p->oy = y; }
static void pos_reactive(struct wl_client *c, struct wl_resource *r) {}
static void pos_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) {}
static void pos_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t s) {}
static const struct xdg_positioner_interface positioner_impl = { pos_destroy, pos_size, pos_anchor_rect, pos_anchor, pos_gravity, pos_adjust, pos_offset, pos_reactive, pos_parent_size, pos_parent_configure };
static void free_data(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }
/* xdg_surface */
static void xs_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = wl_resource_get_user_data(r);
    struct toplevel *t = calloc(1, sizeof *t);
    t->s = s; wl_list_init(&t->popups);
    s->role = ROLE_TOPLEVEL; s->top = t;
    s->role_res = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->role_res, &toplevel_impl, t, toplevel_free);
    if (!kbd_focus) set_keyboard_focus(s);
}
static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *pos) {
    struct surface *s = wl_resource_get_user_data(r);
    struct surface *par = parent ? wl_resource_get_user_data(parent) : 0;
    struct positioner *p = wl_resource_get_user_data(pos);
    s->role = ROLE_POPUP; s->parent = par;
    s->role_res = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->role_res, &popup_impl, s, popup_free);
    place(p, &s->px, &s->py);
    s->gw = p->w; s->gh = p->h;
    /* keep it inside the toplevel's window (popups are drawn into it) */
    struct toplevel *t = par ? top_of(par) : 0;
    if (t && t->ww && par->role == ROLE_TOPLEVEL) {
        if (s->px + p->w > t->ww) s->px = t->ww - p->w;
        if (s->py + p->h > t->wh) s->py = t->wh - p->h;
        if (s->px < 0) s->px = 0;
        if (s->py < 0) s->py = 0;
    }
    if (t) { struct popup *e = calloc(1, sizeof *e); e->s = s; wl_list_insert(t->popups.prev, &e->link); }
}
static void xs_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    struct surface *s = wl_resource_get_user_data(r); s->pgx = x; s->pgy = y; s->pgw = w; s->pgh = h; s->phas_geom = true;
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) {
    struct surface *s = wl_resource_get_user_data(r);
    if (s && s->role == ROLE_TOPLEVEL && s->top && serial == s->top->cfg_serial) s->top->acked = true;
}
static const struct xdg_surface_interface xdg_surface_impl = { xs_destroy, xs_get_toplevel, xs_get_popup, xs_geometry, xs_ack };
static void xdg_surface_free(struct wl_resource *r) { struct surface *s = wl_resource_get_user_data(r); if (s) { s->xdg_surface = 0; s->configured = false; } }
/* wm_base */
static void wm_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &positioner_impl, calloc(1, sizeof(struct positioner)), free_data);
}
static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf) {
    struct surface *s = surf_of(surf);
    s->xdg_surface = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->xdg_surface, &xdg_surface_impl, s, xdg_surface_free);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) {}
static const struct xdg_wm_base_interface wm_impl = { wm_destroy, wm_create_positioner, wm_get_xdg_surface, wm_pong };
static void bind_wm(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, v, id);
    wl_resource_set_implementation(r, &wm_impl, 0, 0);
}

/* ---------------- decorations: AXIOM draws the frame ---------------- */
static void deco_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void deco_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t m) { zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE); }
static void deco_unset(struct wl_client *c, struct wl_resource *r) { zxdg_toplevel_decoration_v1_send_configure(r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE); }
static const struct zxdg_toplevel_decoration_v1_interface deco_impl = { deco_destroy, deco_set_mode, deco_unset };
static void decom_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *tl) {
    struct wl_resource *d = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface, 1, id);
    wl_resource_set_implementation(d, &deco_impl, 0, 0);
    zxdg_toplevel_decoration_v1_send_configure(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static const struct zxdg_decoration_manager_v1_interface decom_impl = { deco_destroy, decom_get };
static void bind_decom(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface, v, id);
    wl_resource_set_implementation(r, &decom_impl, 0, 0);
}
static void kde_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
/* (the mode was sent when the decoration was made; answering every request again makes GTK lay out again, forever) */
static void kde_request(struct wl_client *c, struct wl_resource *r, uint32_t m) {}
static const struct org_kde_kwin_server_decoration_interface kde_impl = { kde_release, kde_request };
static void kdem_create(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *s) {
    struct wl_resource *d = wl_resource_create(c, &org_kde_kwin_server_decoration_interface, 1, id);
    wl_resource_set_implementation(d, &kde_impl, 0, 0);
    org_kde_kwin_server_decoration_send_mode(d, 2);    /* Server */
}
static const struct org_kde_kwin_server_decoration_manager_interface kdem_impl = { kdem_create };
static void bind_kdem(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &org_kde_kwin_server_decoration_manager_interface, v, id);
    wl_resource_set_implementation(r, &kdem_impl, 0, 0);
    org_kde_kwin_server_decoration_manager_send_default_mode(r, 2);
}

#include "data_device.inc"

/* ---------------- input from AXIOM windows ---------------- */
/* the surface under (x, y) of a toplevel's window, and the point in its own coordinates */
static struct surface *hit_tree(struct surface *s, int x, int y, int ox, int oy, double *sx, double *sy) {
    struct sub *c;
    wl_list_for_each_reverse(c, &s->subs, link) { struct surface *h = hit_tree(c->s, x, y, ox + c->x, oy + c->y, sx, sy); if (h) return h; }
    if (s->img && x >= ox && y >= oy && x < ox + s->w && y < oy + s->h) { *sx = x - ox; *sy = y - oy; return s; }
    return 0;
}
static struct surface *hit(struct toplevel *t, int x, int y, double *sx, double *sy) {
    struct popup *p;
    wl_list_for_each_reverse(p, &t->popups, link) if (p->s->mapped) {
        int ox, oy; popup_origin(p->s, &ox, &oy);
        struct surface *h = hit_tree(p->s, x, y, ox, oy, sx, sy); if (h) return h;
    }
    int gx, gy, gw, gh; geom(t->s, &gx, &gy, &gw, &gh);
    return hit_tree(t->s, x, y, -gx, -gy, sx, sy);
}
static void dismiss_popups(struct toplevel *t) {             /* a click outside the menus closes them */
    struct popup *p;
    wl_list_for_each_reverse(p, &t->popups, link) if (p->s->role_res && p->s->grab) xdg_popup_send_popup_done(p->s->role_res);
}
static int window_event(int fd, uint32_t mask, void *data) {
    struct toplevel *t = data; struct axwin_event e;
    while (be_event(t->win, &e)) {
        double sx = 0, sy = 0; struct surface *s; struct wl_resource *p;
        switch (e.type) {
        case AXWIN_EV_MOUSE_MOVE:
            s = hit(t, e.x, e.y, &sx, &sy);
            set_pointer_focus(s, sx, sy);
            if (s) { EACH_FOR(&pointers, s, p) wl_pointer_send_motion(p, now_ms(), wl_fixed_from_double(sx), wl_fixed_from_double(sy)); ptr_frame(s); }
            ptr_x = e.x; ptr_y = e.y;
            break;
        case AXWIN_EV_MOUSE_DOWN: case AXWIN_EV_MOUSE_UP: {
            s = hit(t, e.x, e.y, &sx, &sy);
            if (e.type == AXWIN_EV_MOUSE_DOWN && (!s || s->role != ROLE_POPUP)) {
                bool in_popup = false; struct surface *q = s;
                while (q && q->as_sub) q = q->as_sub->parent;
                if (q && q->role == ROLE_POPUP) in_popup = true;
                if (!in_popup) dismiss_popups(t);
            }
            set_pointer_focus(s, sx, sy);
            if (!s) break;
            if (e.type == AXWIN_EV_MOUSE_DOWN) { struct surface *k = s; while (k->as_sub) k = k->as_sub->parent; if (k->role == ROLE_TOPLEVEL) set_keyboard_focus(k); }
            uint32_t btn = e.button == 2 ? 0x111 : e.button == 3 ? 0x112 : 0x110;   /* BTN_RIGHT, BTN_MIDDLE, BTN_LEFT */
            EACH_FOR(&pointers, s, p) wl_pointer_send_button(p, wl_display_next_serial(dpy), now_ms(), btn, e.type == AXWIN_EV_MOUSE_DOWN);
            ptr_frame(s);
            break;
        }
        case AXWIN_EV_WHEEL:
            s = ptr_focus;
            if (s) { EACH_FOR(&pointers, s, p) wl_pointer_send_axis(p, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(e.key * 15)); ptr_frame(s); }
            break;
        case AXWIN_EV_MOUSE_LEAVE: set_pointer_focus(0, 0, 0); break;
        case AXWIN_EV_KEY: {
            struct surface *k = kbd_focus && top_of(kbd_focus) == t ? kbd_focus : t->s;
            set_keyboard_focus(k);
            key_event(k, e.key);
            break;
        }
        case AXWIN_EV_FOCUS: set_keyboard_focus(t->s); break;
        case AXWIN_EV_CLOSE: if (t->s->role_res) xdg_toplevel_send_close(t->s->role_res); break;
        case AXWIN_EV_RESIZE:
            t->resize_pending = true; t->acked = false; t->want_w = e.x; t->want_h = e.y;
            send_toplevel_configure(t, e.x, e.y);
            break;
        }
    }
    clipboard_sync(kbd_focus);
    wl_display_flush_clients(dpy);
    return 0;
}
void clipboard_sync_focus(void) { clipboard_sync(kbd_focus); }
void watch_window(struct toplevel *t) {
    int fd = be_fd(t->win);
    if (fd >= 0) t->src = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, window_event, t);
}

/* ---------------- main ---------------- */
static int control_event(int fd, uint32_t mask, void *data) { be_control(); wl_display_flush_clients(dpy); return 0; }
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (be_init() < 0) { fprintf(stderr, "axwl: no AXIOM windows here (/dev/axiom/window)\n"); return 1; }
    be_screen(&screen_w, &screen_h);
    if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    mkdir(getenv("XDG_RUNTIME_DIR"), 0700);            /* (AXIOM's /tmp may not be there yet) */
    wl_list_init(&all_surfaces); wl_list_init(&pointers); wl_list_init(&keyboards);
    dpy = wl_display_create();
    loop = wl_display_get_event_loop(dpy);
    const char *name = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "wayland-0";
    if (wl_display_add_socket(dpy, name) < 0) { fprintf(stderr, "axwl: couldn't open the socket %s/%s: %s\n", getenv("XDG_RUNTIME_DIR"), name, strerror(errno)); return 1; }
    wl_global_create(dpy, &wl_compositor_interface, 5, 0, bind_comp);
    wl_global_create(dpy, &wl_subcompositor_interface, 1, 0, bind_subc);
    wl_global_create(dpy, &wl_output_interface, 4, 0, bind_output);
    wl_global_create(dpy, &wl_seat_interface, 7, 0, bind_seat);
    wl_global_create(dpy, &xdg_wm_base_interface, 3, 0, bind_wm);
    wl_global_create(dpy, &zxdg_decoration_manager_v1_interface, 1, 0, bind_decom);
    wl_global_create(dpy, &org_kde_kwin_server_decoration_manager_interface, 1, 0, bind_kdem);
    wl_display_init_shm(dpy);
    wl_list_init(&devices);
    wl_global_create(dpy, &wl_data_device_manager_interface, 3, 0, bind_dm);
    if (be_control_fd() >= 0) wl_event_loop_add_fd(loop, be_control_fd(), WL_EVENT_READABLE, control_event, 0);
    fprintf(stderr, "axwl: Wayland on %s/%s (%dx%d)\n", getenv("XDG_RUNTIME_DIR"), name, screen_w, screen_h);
    wl_display_run(dpy);
    return 0;
}
