/* Merrow Lake Drainage for AXIOM: shared declarations.
 * A native remake of the Unity game (same gameplay, story and numbers; simpler low-poly graphics). */
#pragma once
#include "../../lib/dunc.h"
#include "mmath.h"
#include "r3d.h"

/* ---------------- content (content.c) ---------------- */
#define NPUMPS 5
#define NSPRINGS 7
#define NUPG 6
#define NITEMS 16
#define NBUILD 6
#define NSECRETS 5
#define NCREEKS 3
#define NRELICS 6
#define NWORLDS 3
#define NPAGES 3
#define NHOUSES 6

enum { SHAPE_CAPSULE, SHAPE_CUBE, SHAPE_CYL, SHAPE_SPHERE };
struct pump_def { const char *id, *name; double rate, cost; int max; const char *desc; uint32_t color; };
struct spring_def { const char *id, *name; float g, angle; double rate, cost; };
struct upg_def { const char *id, *name; int max; };
struct item_def { const char *id, *name; float g; double val; int shape; float sx, sy, sz; uint32_t color; const char *desc, *lore, *unlock; };
struct building_def { const char *id, *name, *req; double cash; float angle, radius; const char *perk, *lore; };
struct secret_def { const char *id, *name; double cash; float angle, radius; const char *lore; };
struct creek_def { const char *id, *name; double rate; int need; const char *item; double divertCost; const char *task; };
struct world_def { const char *name, *blurb, *welcome; };
struct page_def { const char *id, *title, *where, *text; };
struct house_def { const char *name; double cash; const char *lore; };

extern const struct pump_def PUMPS[NPUMPS];
extern const struct spring_def SPRINGS[NSPRINGS];
extern const struct upg_def UPGRADES[NUPG];
extern const struct item_def ITEMS[NITEMS];
extern const struct building_def BUILDINGS[NBUILD];
extern const struct secret_def SECRETS[NSECRETS];
extern const struct creek_def CREEKS[NCREEKS];
extern const char *const RELICS[NRELICS];
extern const struct world_def WORLDS[NWORLDS];
extern const struct page_def PAGES[NPAGES];
extern const struct house_def HOUSES[NHOUSES];
extern const char *GATE_NAME, *GATE_LORE, *STILL_ROOM_PAGE, *PLUG_NAME, *PLUG_DESC, *DROWNED_CHURCH;
extern const double DROWNED_CHURCH_CASH;
#define TOTAL_GALLONS 5000000.0
#define PROGRESS_K 600.0
#define MAX_FEET 84.0f
#define SPRING_WATER_PRICE 0.8
double upg_cost(int i, int n);
void upg_desc(int i, int n, char *out, int max);
int item_index(const char *id);
int building_index(const char *id);

/* number formatting like the Unity game's Fmt */
void fmt_num(double n, char *out);      /* out: at least 24 bytes */
void fmt_money(double n, char *out);

/* ---------------- game state (game.c) ---------------- */
#define LOG_MAX 40
#define LOG_LEN 240
struct game {
    double water, tank, cash, earned, t, carried;
    int won;
    int pumps[NPUMPS], upg[NUPG];
    uint32_t found;                 /* item bits, plus bit 16 = the plug */
    uint32_t explored, secrets;     /* building bits, secret bits */
    uint32_t springsOpen, springsCapped;
    int creekState[NCREEKS];        /* 0 flowing, 1 dammed, 2 diverted to bottling */
    int stones, planks, valve;
    uint32_t taken;                 /* bits: 0..4 river stones, 5 valve, 6..11 drowned houses, 12 the church */
    uint32_t relicsSet;             /* bits by RELICS index */
    int gateOpen, ngPlus;
    uint32_t pages;                 /* bits by PAGES index */
    int ending, keeper;             /* ending: 0 none, 1 keeper, 2 home */
    char log[LOG_MAX][LOG_LEN]; int nlog;
    uint32_t announced;             /* item bits, plus bit 16 = the plug */
    float saveTimer;
};
extern struct game G;
#define FOUND_PLUG (1u << 16)
#define TAKEN_VALVE (1u << 5)
#define TAKEN_HOUSE(i) (1u << (6 + (i)))
#define TAKEN_CHURCH (1u << 12)

static inline int world_no(void) { return G.ngPlus % NWORLDS; }
static inline int is_drowned(void) { return world_no() == 1; }
static inline int other_side(void) { return world_no() == 2; }
static inline int explored(const char *id) { int i = building_index(id); return i >= 0 && (G.explored >> i & 1); }
static inline int found_item(int i) { return G.found >> i & 1; }
int popcount32(uint32_t x);

double g_bucket(void);
double g_capacity(void);
double g_carry_cap(void);
float  g_speed_mult(void);
int    g_has_pipeline(void);
double g_price(void);
double g_pump_rate(void);
float  g_progress(void);
double g_inflow(void);
double g_spring_mult(void);
double g_bottling(void);
int    g_creek_have(int i);
int    g_spring_flowing(int i);
int    g_open_springs(void);
double g_pump_cost(int i);
int    g_plug_exposed(void);
int    g_relics_found(void);

void g_reset(void);
void g_new_game(void);
void g_update(float dt);
void g_log(const char *fmt, ...);
int  g_save(void);
int  g_load(void);

/* actions (each may open a modal or toast) */
void g_scoop(v3 at);
void g_tank_action(void);
void g_tank_prompt(char *out, int n);
void g_sell_all(void);
void g_buy_pump(int i);
void g_buy_upgrade(int i);
void g_collect_item(int i);
void g_explore_building(int i);
void g_cap_spring(int i);
void g_creek_prompt(int i, char *out, int n);
void g_creek_action(int i);
void g_take_stone(int k);
void g_take_plank(void);
void g_take_valve(void);
void g_find_secret(int i);
void g_ask_pull_plug(void);
void g_gate_prompt(char *out, int n);
void g_gate_action(void);
void g_read_page(int i, const char *intro);
void g_search_drowned(int i);
void g_read_last_page(void);
void g_add_page(int i);

/* ---------------- the world (world.c) ---------------- */
#define LAKE_RX 62.0f
#define LAKE_RZ 46.0f
#define LAKE_DEPTH 22.0f
#define LAKE_FULL (-0.15f)
#define WORLD_SIZE 300.0f
#define PLAYER_BOUND 138.0f
float w_shore(float a);
float w_rho(float x, float z, float *a);
void  w_planar(float angle, float rr, float *x, float *z);
v3    w_on_ellipse(float angle, float rr);
float w_h(float x, float z);               /* exact ground height (slow: noise) */
float w_ground(float x, float z);          /* fast: from the height grid, bilinear */
float w_level(void);                       /* the water height now */
float w_level_for_geo(float r);
float w_feet(void);
float w_bmin(void);
v3    w_deepest(void);

enum { SCENE_OUTSIDE, SCENE_CAVE, SCENE_STILL };
struct world {
    int scene;
    float hour, night;                     /* 0..24; 0 full day .. 1 deep night */
    float cloud, rain, mist, cloudT, rainT, mistT, weatherTimer, flash, lightningTimer;
    v3 tank, store, building[NBUILD], secret[NSECRETS], item[NITEMS], spring[NSPRINGS];
    float buildingYaw[NBUILD], storeYaw, tankYaw;
    v3 creekHead[NCREEKS]; v3 creekPath[NCREEKS][160]; int creekLen[NCREEKS];
    v3 stone[5], plankStack, valve;
    v3 church, house[NHOUSES]; float churchDoorY, houseDoorY[NHOUSES];
    v3 caveMouth;
    float time;
};
extern struct world W;

int  w_init(void (*progress)(const char *what, int pct));
void w_update(float dt);
void w_render(void);
void w_refresh(void);                      /* after a crossing / new game */
int  w_blocked(v3 p);                      /* inside a building (for the camera) */
void w_splash(v3 at);
void w_float_text(v3 at, const char *text, uint32_t col);
const char *w_weather_name(void);
void w_clock(char *out);
void w_draw_map(struct canvas *c, int x, int y, int size);
uint32_t w_sky_color(float elev);

/* ---------------- the player (main.c) ---------------- */
struct player { v3 pos; float yaw, vy; int swimming, onGround; float camYaw, camPitch, camDist; };
extern struct player P;
void p_teleport(v3 pos, float yaw);

/* ---------------- HUD and menus (hud.c) ---------------- */
typedef void (*modal_fn)(void);
struct modal_btn { const char *label; modal_fn fn; };
void hud_toast(const char *fmt, ...);
void hud_modal(const char *title, const char *body, int nbtn, ...);   /* nbtn pairs of (label, fn) */
int  hud_modal_open(void);
void hud_float(v3 at, const char *text, uint32_t col);
void hud_fade(void (*mid)(void));
void hud_credits(int crossings, const char *title, const char *footer);
void hud_open_shop(void);
extern const uint32_t ACCENT, GREEN, AMBER;
