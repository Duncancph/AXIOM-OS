/* Merrow Lake Drainage for AXIOM (installed with: clan install merrow).
 * A native remake of Duncan Interactive's Unity game: drain a mountain lake by bucket, tank and pumps, shut the creeks,
 * cap the springs, find what the water hid, pull the plug, and follow the Merrow Gate through three valleys.
 *
 * Input: AXIOM reports key presses but not releases (USB keyboards don't repeat either, PS/2 ones do), so the keys
 * work by taps: W starts and stops walking (Shift+W runs), S stops or steps back, A/D turn in smooth steps.
 * Holding the left mouse button walks for as long as it is held (real press/release) and the mouse steers.
 * If the kernel ever sends EV_KEY_UP (event type 12), held keys work the usual way instead. */
#include "merrow.h"
#include "hud.h"

#define WIN_W 960
#define WIN_H 600
static struct app A;
struct player P;

static const int DETAIL_W[3] = { 240, 320, 480 }, DETAIL_H[3] = { 150, 200, 300 }, DETAIL_S[3] = { 4, 3, 2 };

/* ---------- movement keys ---------- */
#ifndef EV_KEY_UP
#define EV_KEY_UP 12          /* not in AXIOM yet: a key release, if a future kernel sends one */
#endif
enum { K_FWD, K_BACK, K_LEFT, K_RIGHT, K_COUNT };
static uint64_t lastPress[K_COUNT], backUntil;
static int keyDown[K_COUNT], haveKeyUp;
static int autoRun, running, mouseWalk, mouseLook, lastMx, lastMy;
static float turnQueue;       /* radians still to turn from taps */
static void press(int a, uint64_t now, int shift) {
    int repeating = now - lastPress[a] < 650;
    lastPress[a] = now;
    if (haveKeyUp) { keyDown[a] = 1; if (a == K_FWD) running = shift; return; }
    switch (a) {
    case K_FWD:
        if (repeating) break;                                   /* a PS/2 key held down: keep going */
        if (autoRun && running == shift) autoRun = 0;           /* tap again to stop */
        else { autoRun = 1; running = shift; }
        break;
    case K_BACK:
        if (autoRun) { autoRun = 0; break; }
        backUntil = now + (repeating ? 160 : 380);
        break;
    default:
        turnQueue += (a == K_LEFT ? -1 : 1) * (repeating ? 0.14f : 0.40f);
        turnQueue = m_clamp(turnQueue, -0.8f, 0.8f);
    }
}
static void release(int a) { haveKeyUp = 1; keyDown[a] = 0; }

void p_teleport(v3 pos, float yaw) {
    P.pos = pos; P.yaw = yaw; P.camYaw = yaw; P.vy = 0;
    if (W.scene == SCENE_OUTSIDE) P.pos.y = w_ground(pos.x, pos.z);
    else P.pos.y = 0;
    autoRun = 0; turnQueue = 0;
}

static void loading(const char *what, int pct) {
    struct canvas *c = &A.c;
    gfx_vgrad(c, 0, 0, c->w, c->h, 0x16222A, 0x070A0C);
    gfx_text_center(c, &font_big, 0, c->w, c->h / 2 - 30, 0xEDE6D6, "Merrow Lake Drainage");
    gfx_text_center(c, &font_ui, 0, c->w, c->h / 2 + 4, 0x9CA6AD, what);
    gfx_rrect(c, c->w / 2 - 160, c->h / 2 + 24, 320, 8, 4, 0x24302F, 255);
    gfx_rrect(c, c->w / 2 - 160, c->h / 2 + 24, 320 * pct / 100, 8, 4, 0xF5A524, 255);
    app_flush(&A);
}

static void set_detail(int d) {
    H.detail = d;
    r_init(DETAIL_W[d], DETAIL_H[d]);
    H.scale = DETAIL_S[d];
    H.ox = (WIN_W - R.w * H.scale) / 2; H.oy = (WIN_H - R.h * H.scale) / 2;
}

/* ---------- the player ---------- */
static void update_player(float dt, uint64_t now) {
    int blocked = hud_blocking();
    float turn = 0, move = 0;
    if (blocked) { autoRun = 0; turnQueue = 0; memset(keyDown, 0, sizeof keyDown); }
    else if (haveKeyUp) {
        turn = (float)(keyDown[K_RIGHT] - keyDown[K_LEFT]);
        if (keyDown[K_FWD] || autoRun || mouseWalk) move = 1;
        if (keyDown[K_BACK]) { move = -0.6f; autoRun = 0; }
        P.yaw += turn * 2.4f * dt;
    } else {
        float step = 3.0f * dt;
        if (m_abs(turnQueue) <= step) { P.yaw += turnQueue; turn = turnQueue != 0; turnQueue = 0; }
        else { float sg = turnQueue > 0 ? 1.0f : -1.0f; P.yaw += sg * step; turnQueue -= sg * step; turn = 1; }
        if (autoRun || mouseWalk) move = 1;
        if (now < backUntil) move = -0.6f;
    }
    float speed = (running && move > 0 && !mouseWalk ? 9.5f : 5.0f) * g_speed_mult();
    float lv = w_level();
    if (P.swimming) speed *= 0.55f;
    v3 np = P.pos;
    np.x += m_sin(P.yaw) * move * speed * dt;
    np.z += m_cos(P.yaw) * move * speed * dt;
    w_collide(&np);
    if (W.scene == SCENE_OUTSIDE) {
        /* don't walk up cliffs */
        float g0 = w_ground(P.pos.x, P.pos.z), g1 = w_ground(np.x, np.z);
        float step = m_sqrt((np.x - P.pos.x) * (np.x - P.pos.x) + (np.z - P.pos.z) * (np.z - P.pos.z));
        if (step > 1e-4f && (g1 - g0) / step > 1.6f && g1 > P.pos.y + 0.3f) np = P.pos;
        P.pos.x = np.x; P.pos.z = np.z;
        float ground = w_ground(P.pos.x, P.pos.z);
        float depth = (G.won ? -99 : lv) - ground;
        if (depth > 1.4f) {                    /* swimming: float at the surface */
            P.swimming = 1; P.onGround = 0; P.vy = 0;
            P.pos.y = m_lerp(P.pos.y, lv - 1.45f, m_clamp01(dt * 6));
        } else {
            P.swimming = 0;
            P.vy -= 24 * dt;
            P.pos.y += P.vy * dt;
            if (P.pos.y <= ground) { P.pos.y = ground; P.vy = 0; P.onGround = 1; } else P.onGround = 0;
        }
    } else {
        P.pos.x = np.x; P.pos.z = np.z; P.swimming = 0;
        P.vy -= 24 * dt; P.pos.y += P.vy * dt;
        if (P.pos.y <= 0) { P.pos.y = 0; P.vy = 0; P.onGround = 1; }
    }
    /* camera: eases back behind the player as you walk */
    float off = P.camYaw - P.yaw;
    off = m_repeat(off + M_PI_F, M_TAU_F) - M_PI_F;
    if (!mouseLook && (move != 0 || turn != 0)) off *= 1 - m_clamp01(dt * 2.5f);
    P.camYaw = P.yaw + off;
}

static void place_camera(void) {
    v3 target = v_add(P.pos, V3(0, 1.7f, 0));
    float pitch = P.camPitch, dist = P.camDist;
    if (W.scene != SCENE_OUTSIDE) dist = m_min(dist, 6);
    v3 fw = V3(m_sin(P.camYaw) * m_cos(pitch), -m_sin(pitch), m_cos(P.camYaw) * m_cos(pitch));
    v3 cam = v_sub(target, v_mul(fw, dist));
    if (W.scene == SCENE_OUTSIDE) {
        /* pull the camera in front of walls between it and the player */
        for (float d = 1.0f; d <= dist; d += 0.5f) {
            v3 q = v_sub(target, v_mul(fw, d));
            if (w_blocked(q)) { cam = v_sub(target, v_mul(fw, m_max(0.8f, d - 0.6f))); break; }
        }
        float g = w_ground(cam.x, cam.z) + 0.6f;
        if (cam.y < g) cam.y = g;
        float lv = w_level();
        if (!G.won && cam.y < lv + 0.4f && cam.y > lv - 3) cam.y = lv + 0.4f;   /* keep the camera out of the water */
    } else {
        cam.y = m_clamp(cam.y, 0.5f, W.scene == SCENE_CAVE ? 10 : 8.5f);
        if (W.scene == SCENE_CAVE) { cam.x = m_clamp(cam.x, -11.5f, 11.5f); cam.z = m_clamp(cam.z, -18.5f, 18.5f); }
        else { float r = m_sqrt(cam.x * cam.x + cam.z * cam.z); if (r > 8) { cam.x *= 8 / r; cam.z *= 8 / r; } }
    }
    /* look at the target from wherever the camera ended up */
    v3 d = v_sub(target, cam);
    float yaw = m_atan2(d.x, d.z), pt = m_atan2(-d.y, m_sqrt(d.x * d.x + d.z * d.z));
    r_camera(cam, yaw, pt, 62);
}

static void title_camera(float t) {
    float a = t * 0.05f + 0.5f;
    v3 cam = V3(m_cos(a) * 95, 28, m_sin(a) * 75);
    cam.y = m_max(cam.y, w_ground(cam.x, cam.z) + 12);
    v3 d = v_sub(V3(0, -2, 0), cam);
    r_camera(cam, m_atan2(d.x, d.z), m_atan2(-d.y, m_sqrt(d.x * d.x + d.z * d.z)), 62);
}

static int debugMode;

void w_debug_teleport(void);
static int move_key(int k, int *shift) {
    int kc = KEY_CODE(k);
    *shift = (k & KEYMOD_SHIFT) != 0 || k == 'W';
    if (k == 'w' || k == 'W' || kc == KEY_UP) return K_FWD;
    if (k == 's' || k == 'S' || kc == KEY_DOWN) return K_BACK;
    if (k == 'a' || k == 'A' || kc == KEY_LEFT) return K_LEFT;
    if (k == 'd' || k == 'D' || kc == KEY_RIGHT) return K_RIGHT;
    return -1;
}
static void handle_key(int k, uint64_t now) {
    if (debugMode && !hud_blocking()) {
        if (k == KEY_F5) { G.cash += 1000000; G.earned += 1000000; hud_toast("debug: +$1M"); return; }
        if (k == KEY_F6) { G.water -= TOTAL_GALLONS * 0.1; if (G.water < 0) G.water = 0; hud_toast("debug: drained"); return; }
        if (k == KEY_F7) { w_debug_teleport(); return; }
        if (k == KEY_F8) { W.hour = m_repeat(W.hour + 3, 24); return; }
        if (k == KEY_F9) { W.weatherTimer = 0; return; }
        if (k == KEY_F10) { for (int i = 0; i < NITEMS; i++) G.found |= 1u << i; G.pages |= 3; hud_toast("debug: all finds"); return; }
        if (k == KEY_F11) { G.water = 0; return; }
        if (k == KEY_F12) { G.won = 1; G.water = 0; W.scene = SCENE_CAVE; p_teleport(V3(0, 0, -11), 0); return; }
    }
    if (hud_key(k)) return;
    int shift, a = move_key(k, &shift);
    if (a >= 0) press(a, now, shift);
    else if (k == 'r' || k == 'R') { autoRun = !autoRun; running = 0; }
    else if (k == ' ') { if (P.onGround) { P.vy = 8; P.onGround = 0; } }
    else if (k == 'e' || k == 'E' || k == 'f') w_interact();
    else if (k == '+' || k == '=') P.camDist = m_max(3, P.camDist - 1);
    else if (k == '-') P.camDist = m_min(14, P.camDist + 1);
}

int main(void) {
    if (!app_open(&A, WIN_W, WIN_H, "Merrow Lake")) return 1;
    { char a[128] = ""; sys_getarg(a, sizeof a);
      for (char *q = a; *q; q++) { if (!strncmp(q, "debug", 5)) debugMode = H.showFps = 1; if (!strncmp(q, "fps", 3)) H.showFps = 1; } }
    loading("Starting up", 0);
    set_detail(1);
    if (!R.px || !w_init(loading)) { loading("Not enough memory", 100); sys_sleep(3000); return 1; }
    H.hasSave = g_start();
    w_refresh();
    if (G.won) { /* came back with the lake drained: stand at the cave doorway */ }
    P.camDist = 7.5f; P.camPitch = 0.30f;
    H.screen = SCR_TITLE;
    uint64_t last = sys_time(), fpsT = last; int frames = 0; float fps = 0;
    for (;;) {
        struct dunc_event e;
        uint64_t now = sys_time();
        while (sys_event(&e, 0) > 0) {
            now = sys_time();
            if (e.type == EV_CLOSE) { g_save(); return 0; }
            if (e.type == EV_KEY) handle_key(e.key, now);
            else if (e.type == EV_KEY_UP) { int sh, a = move_key(e.key, &sh); if (a >= 0) release(a); }
            else if (e.type == EV_MOUSE_MOVE) {
                int dx = e.x - lastMx, dy = e.y - lastMy;
                lastMx = e.x; lastMy = e.y; H.mx = e.x; H.my = e.y;
                if (!hud_blocking()) {
                    if (mouseWalk) P.yaw += dx * 0.006f, P.camYaw += dx * 0.006f;
                    if (mouseLook) { P.camYaw += dx * 0.008f; P.camPitch = m_clamp(P.camPitch + dy * 0.005f, -0.25f, 1.2f); }
                }
            } else if (e.type == EV_MOUSE_DOWN) {
                lastMx = e.x; lastMy = e.y; H.mx = e.x; H.my = e.y;
                if (hud_click(&A.c, e.x, e.y)) continue;
                if (e.button == 1) mouseWalk = 1; else mouseLook = 1;
            } else if (e.type == EV_MOUSE_UP) { mouseWalk = 0; mouseLook = 0; }
            else if (e.type == EV_WHEEL) P.camDist = m_clamp(P.camDist + (e.y > 0 ? -1 : 1), 3, 14);
            else if (e.type == EV_BLUR || e.type == EV_MOUSE_LEAVE) { mouseWalk = 0; mouseLook = 0; }
        }
        if (H.quit) return 0;
        if (H.detailChanged) { H.detailChanged = 0; set_detail(H.detail); gfx_fill(&A.c, 0, 0, WIN_W, WIN_H, 0); }
        float dt = (now - last) / 1000.0f; last = now;
        if (dt > 0.1f) dt = 0.1f;
        int paused = H.screen == SCR_PAUSE || H.screen == SCR_TITLE;
        if (!paused) { g_update(dt); w_update(dt); update_player(dt, now); }
        else w_update(dt * 0.25f);
        hud_update(dt);
        /* what's in reach */
        H.prompt[0] = 0;
        if (H.screen == SCR_GAME && !hud_modal_open()) w_find_interact(H.prompt, sizeof H.prompt);
        R.tris = 0;
        if (H.screen == SCR_TITLE) title_camera(W.time); else place_camera();
        w_render();
        uint64_t tp0 = sys_time();
        r_present(A.c.px, A.c.w, A.c.h, A.c.stride, H.scale, H.ox, H.oy);
        uint64_t tp1 = sys_time();
        hud_draw(&A.c, fps);
        uint64_t tp2 = sys_time();
        app_flush(&A);
        { extern int w_prof[8]; w_prof[6] = (int)(tp1 - tp0); w_prof[7] = (int)(tp2 - tp1); H.flushMs = (int)(sys_time() - tp2); }
        frames++;
        if (now - fpsT >= 1000) { fps = frames * 1000.0f / (now - fpsT); frames = 0; fpsT = now; }
        /* don't spin faster than 60 frames a second */
        uint64_t spent = sys_time() - now;
        if (spent < 16) { if (sys_event(&e, 16 - spent) > 0) { /* put it back through the loop next time */
                if (e.type == EV_CLOSE) { g_save(); return 0; }
                if (e.type == EV_KEY) handle_key(e.key, sys_time());
                else if (e.type == EV_MOUSE_DOWN) { H.mx = e.x; H.my = e.y; lastMx = e.x; lastMy = e.y; if (!hud_click(&A.c, e.x, e.y)) { if (e.button == 1) mouseWalk = 1; else mouseLook = 1; } }
                else if (e.type == EV_MOUSE_UP) { mouseWalk = 0; mouseLook = 0; }
                else if (e.type == EV_MOUSE_MOVE) { H.mx = e.x; H.my = e.y; lastMx = e.x; lastMy = e.y; }
            } }
    }
}
