/* ---------------- clipboard: wl_data_device_manager <-> the AXIOM clipboard ----------------
 * The AXIOM clipboard is the one copy: when an app sets the selection, its text is read (through a pipe) and put
 * on the AXIOM clipboard; the focused app is always offered the AXIOM clipboard's text. Drag and drop isn't
 * supported yet. */
static const char *text_types[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT" };
static struct wl_list devices;                         /* wl_data_device resources */
static uint64_t offered_seq = ~0ull; static struct wl_client *offered_to;
struct reader { int fd; char *buf; long n, cap; struct wl_event_source *src; };
struct writer { int fd; char *buf; long n, off; struct wl_event_source *src; };
static int reader_ready(int fd, uint32_t mask, void *data) {
    struct reader *r = data;
    for (;;) {
        if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 4096; if (r->cap > (1 << 22)) break; r->buf = realloc(r->buf, r->cap); }
        long k = read(fd, r->buf + r->n, r->cap - r->n);
        if (k > 0) { r->n += k; continue; }
        if (k < 0 && errno == EAGAIN) return 0;
        break;                                         /* end (or an error): done */
    }
    if (r->n) { be_clip_set(r->buf, r->n); uint64_t sq; be_clip_get(0, 0, &sq); offered_seq = sq; }
    wl_event_source_remove(r->src); close(fd); free(r->buf); free(r);
    return 0;
}
static int writer_ready(int fd, uint32_t mask, void *data) {
    struct writer *w = data;
    while (w->off < w->n) {
        long k = write(fd, w->buf + w->off, w->n - w->off);
        if (k > 0) { w->off += k; continue; }
        if (k < 0 && errno == EAGAIN) return 0;
        break;
    }
    wl_event_source_remove(w->src); close(fd); free(w->buf); free(w);
    return 0;
}
/* data_offer (what we give apps) */
static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime) {}
static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd) {
    long n = be_clip_get(0, 0, 0);
    struct writer *w = calloc(1, sizeof *w);
    w->buf = malloc(n > 0 ? n : 1); w->n = n > 0 ? be_clip_get(w->buf, n, 0) : 0; w->fd = fd;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    w->src = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, writer_ready, w);
}
static void offer_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void offer_finish(struct wl_client *c, struct wl_resource *r) {}
static void offer_actions(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t p) {}
static const struct wl_data_offer_interface offer_impl = { offer_accept, offer_receive, offer_destroy, offer_finish, offer_actions };
static void send_selection(struct wl_client *c) {
    struct wl_resource *d;
    long n = be_clip_get(0, 0, 0);
    wl_resource_for_each(d, &devices) if (wl_resource_get_client(d) == c) {
        if (n <= 0) { wl_data_device_send_selection(d, 0); continue; }
        struct wl_resource *o = wl_resource_create(c, &wl_data_offer_interface, wl_resource_get_version(d), 0);
        wl_resource_set_implementation(o, &offer_impl, 0, 0);
        wl_data_device_send_data_offer(d, o);
        for (unsigned i = 0; i < sizeof text_types / sizeof *text_types; i++) wl_data_offer_send_offer(o, text_types[i]);
        wl_data_device_send_selection(d, o);
    }
}
/* called when the keyboard focus changes, and after input: offer the clipboard again if it changed */
static void clipboard_sync(struct surface *focus) {
    if (!focus) return;
    struct wl_client *c = wl_resource_get_client(focus->res);
    uint64_t sq = 0; be_clip_get(0, 0, &sq);
    if (c == offered_to && sq == offered_seq) return;
    offered_to = c; offered_seq = sq;
    send_selection(c);
}
/* data_source (what apps give us) */
struct source { struct wl_resource *res; char mime[64]; };
static struct source *cur_source;
static void source_free(struct wl_resource *r) { struct source *s = wl_resource_get_user_data(r); if (cur_source == s) cur_source = 0; free(s); }
static void src_offer(struct wl_client *c, struct wl_resource *r, const char *mime) {
    struct source *s = wl_resource_get_user_data(r);
    for (unsigned i = 0; i < sizeof text_types / sizeof *text_types; i++)
        if (!strcmp(mime, text_types[i]) && (!s->mime[0] || i == 0)) strncpy(s->mime, mime, sizeof s->mime - 1);
}
static void src_destroy(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static void src_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) {}
static const struct wl_data_source_interface source_impl = { src_offer, src_destroy, src_actions };
static void dm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct source *s = calloc(1, sizeof *s);
    s->res = wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &source_impl, s, source_free);
}
static void dd_start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, struct wl_resource *origin, struct wl_resource *icon, uint32_t serial) {
    if (src) wl_data_source_send_cancelled(src);
}
static void dd_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src, uint32_t serial) {
    if (!src) return;
    struct source *s = wl_resource_get_user_data(src);
    if (!s->mime[0]) { wl_data_source_send_cancelled(src); return; }    /* not text: AXIOM's clipboard holds text */
    int p[2]; if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) return;
    wl_data_source_send_send(src, s->mime, p[1]); close(p[1]);
    struct reader *rd = calloc(1, sizeof *rd); rd->fd = p[0];
    rd->src = wl_event_loop_add_fd(loop, p[0], WL_EVENT_READABLE, reader_ready, rd);
    offered_to = c;                                    /* the app knows its own selection */
    if (cur_source && cur_source != s) wl_data_source_send_cancelled(cur_source->res);   /* replaced */
    cur_source = s;
}
static void dd_release(struct wl_client *c, struct wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_data_device_interface device_impl = { dd_start_drag, dd_set_selection, dd_release };
static void dm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(d, &device_impl, 0, unlink_res);
    wl_list_insert(&devices, wl_resource_get_link(d));
    if (kbd_focus && wl_resource_get_client(kbd_focus->res) == c) { offered_to = 0; clipboard_sync(kbd_focus); }
}
static const struct wl_data_device_manager_interface dm_impl = { dm_create_source, dm_get_device };
static void bind_dm(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, v, id);
    wl_resource_set_implementation(r, &dm_impl, 0, 0);
}
