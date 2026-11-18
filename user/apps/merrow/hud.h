/* Merrow Lake: HUD state shared between hud.c and main.c */
#pragma once
#include "merrow.h"

enum { SCR_TITLE, SCR_GAME, SCR_SHOP, SCR_LOG, SCR_MAP, SCR_PAUSE, SCR_CREDITS };
struct modal { char title[64]; char body[2400]; int nbtn; char btn[3][40]; modal_fn fn[3]; int sel; int bx[3], by[3], bw; };
struct floater { v3 p; float t; uint32_t col; char s[48]; };
struct hud {
    int screen;
    struct modal modals[4]; int nmodal;
    char toast[160]; float toastT;
    struct floater floats[16];
    char prompt[160];
    int mx, my;                 /* mouse in window coordinates */
    int shopSel, pauseSel, titleSel, logScroll;
    float fadeT; void (*fadeMid)(void); int fadeDone;
    float creditsT; int crossings; char credTitle[96], credFooter[300];
    int detail, detailChanged, hasSave, quit, showFps;
    int scale, ox, oy;          /* how the 3D picture sits in the window */
    int flushMs;
};
extern struct hud H;
void hud_update(float dt);
void hud_draw(struct canvas *c, float fps);
int  hud_key(int k);
int  hud_click(struct canvas *c, int x, int y);
int  hud_blocking(void);
int  w_find_interact(char *prompt, int n);
void w_interact(void);
void w_collide(v3 *p);
int  g_start(void);   /* 1 if a saved game was loaded */
int  g_drowned_searched(void);
