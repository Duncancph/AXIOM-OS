/* Where axwl's windows go: AXIOM windows (backend_axiom.c) or, for testing on Linux, memory with snapshots
 * and scripted input (backend_test.c). Events use AXIOM's window events (common/axiom_window.h). */
#pragma once
#include <stdint.h>
#include "axiom_window.h"
struct bwin;
int  be_init(void);                                            /* 0 ok */
void be_screen(int *w, int *h);
struct bwin *be_create(int w, int h, const char *title);
void be_destroy(struct bwin *b);
uint32_t *be_pixels(struct bwin *b, int *w, int *h, int *stride_px);   /* XRGB, may change after be_resize */
void be_flush(struct bwin *b, int x, int y, int w, int h);
void be_title(struct bwin *b, const char *t);
int  be_fd(struct bwin *b);                                    /* readable when events wait (-1: none) */
int  be_event(struct bwin *b, struct axwin_event *e);          /* 1 if one was read */
int  be_resize(struct bwin *b);                                /* take the size the last RESIZE event asked for */
/* the clipboard (UTF-8) */
long be_clip_get(char *buf, long max, uint64_t *seq);          /* length (whole), copies what fits */
void be_clip_set(const char *s, long n);
/* the test backend's own control channel (-1 on AXIOM) */
int  be_control_fd(void);
void be_control(void);
