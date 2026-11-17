/* Merrow Lake: game state, economy and player actions (the Unity game's LakeGame.cs). */
#include "merrow.h"

/* modal paragraph styles (see hud.c): green, amber, lore (italic in Unity), muted */
#define GRN "\x02"
#define AMB "\x03"
#define LORE "\x04"
#define MUT "\x05"
#define DOT "\xB7"

struct game G;
static double money_tmp;

int popcount32(uint32_t x) { int n = 0; while (x) { n += x & 1; x >>= 1; } return n; }

static int lvl(const char *id) { for (int i = 0; i < NUPG; i++) if (!strcmp(UPGRADES[i].id, id)) return G.upg[i]; return 0; }
double g_bucket(void) { return 8 * m_powd(2, lvl("bucket")) * (explored("cabin") ? 4 : 1); }
double g_capacity(void) { return 100 * m_powd(2, lvl("tank")); }
double g_carry_cap(void) { return g_bucket() * (3 + 3 * lvl("yoke")); }
float g_speed_mult(void) { return 1 + .15f * lvl("boots"); }
int g_has_pipeline(void) { return lvl("pipe") > 0; }
double g_price(void) { return 0.25 * m_powd(1.3, lvl("filter")) * (explored("lighthouse") ? 1.5 : 1); }
double g_pump_rate(void) {
    double r = 0;
    for (int i = 0; i < NPUMPS; i++) r += PUMPS[i].rate * G.pumps[i];
    if (explored("observatory")) r *= 2;
    if (explored("mill")) r += 150;
    return r;
}
float g_progress(void) {
    double d = 1 - G.water / TOTAL_GALLONS; if (d < 0) d = 0;
    float p = m_log((float)(1 + PROGRESS_K * d)) / m_log((float)(1 + PROGRESS_K));
    return p > 1 ? 1 : p < 0 ? 0 : p;
}
double g_spring_mult(void) { return G.keeper ? 1 : 1 + .5 * G.ngPlus; }
int g_spring_flowing(int i) { return (G.springsOpen >> i & 1) && !(G.springsCapped >> i & 1) && !explored("chapel"); }
double g_inflow(void) {
    if (G.water <= 0 || G.water >= TOTAL_GALLONS) return 0;
    double r = 0;
    for (int i = 0; i < NCREEKS; i++) if (G.creekState[i] == 0) r += CREEKS[i].rate;
    for (int i = 0; i < NSPRINGS; i++) if (g_spring_flowing(i)) r += SPRINGS[i].rate;
    return r * g_spring_mult();
}
double g_bottling(void) { double r = 0; for (int i = 0; i < NCREEKS; i++) if (G.creekState[i] == 2) r += CREEKS[i].rate * SPRING_WATER_PRICE; return r; }
int g_creek_have(int i) { return i == 0 ? G.stones : i == 1 ? G.planks : (G.valve ? 1 : 0); }
int g_open_springs(void) { int n = 0; for (int i = 0; i < NSPRINGS; i++) n += g_spring_flowing(i); return n; }
double g_pump_cost(int i) { double c = PUMPS[i].cost * m_powd(1.15, G.pumps[i]); double f = m_floord(c); return f < c ? f + 1 : f; }
int g_plug_exposed(void) { return !G.won && G.water <= TOTAL_GALLONS * 0.0015; }
int g_relics_found(void) { int n = 0; for (int i = 0; i < NRELICS; i++) if (found_item(item_index(RELICS[i]))) n++; return n; }

void g_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (G.nlog == LOG_MAX) G.nlog--;
    memmove(G.log[1], G.log[0], (size_t)G.nlog * LOG_LEN);
    kvsnprintf(G.log[0], LOG_LEN, fmt, ap);
    G.nlog++;
    va_end(ap);
}

static void earn(double v) { G.cash += v; G.earned += v; }

void g_reset(void) {
    G.water = TOTAL_GALLONS; G.tank = G.cash = G.earned = G.t = G.carried = 0; G.won = 0;
    memset(G.pumps, 0, sizeof G.pumps); memset(G.upg, 0, sizeof G.upg);
    G.found = G.explored = G.secrets = 0; G.nlog = 0; G.announced = 0; G.springsOpen = G.springsCapped = 0;
    memset(G.creekState, 0, sizeof G.creekState); G.stones = G.planks = G.valve = 0; G.taken = 0;
    G.relicsSet = 0; G.gateOpen = 0;
}

static void intro_logs(void) {
    int has = 0;
    for (int i = 0; i < G.nlog; i++) { const char *s = G.log[i]; for (; *s; s++) if (!strncmp(s, "creeks feed", 11)) has = 1; }
    if (!has) g_log("Three creeks feed Merrow Lake from the hills: Arm Creek, Fern Brook and Mill Run. Follow them up to their springs to shut them off (logbook: L).");
    if (G.nlog == 1) g_log("Merrow Lake holds 5,000,000 gallons, and the county will buy every drop. Scoop at the shore (E), carry your bucket to the storage tank on the east shore, pour it in and sell. Upgrades are at the Supply Store.");
}

void g_new_game(void) {
    g_reset(); G.ngPlus = 0; G.pages = 0; G.ending = 0; G.keeper = 0;
    g_log("A new permit. Merrow Lake is full again: 5,000,000 gallons.");
    intro_logs();
    g_save();
    w_refresh();
}

void g_update(float dt) {
    if (dt > 0.25f) dt = 0.25f;
    G.t += dt;
    if (!G.won) {
        G.water += g_inflow() * dt; if (G.water > TOTAL_GALLONS) G.water = TOTAL_GALLONS;
        double pumped = g_pump_rate() * dt, room = g_capacity() - G.tank;
        if (pumped > room) pumped = room;
        if (pumped > G.water) pumped = G.water;
        if (pumped < 0) pumped = 0;
        G.water -= pumped; G.tank += pumped;
        if (g_has_pipeline()) {
            double s = g_capacity() * dt; if (s > G.tank) s = G.tank;
            G.tank -= s; earn(s * g_price());
        }
        if (G.water < 1) G.water = 0;
    }
    float p = g_progress();
    for (int i = 0; i < NITEMS; i++)
        if (!(G.announced >> i & 1) && !found_item(i) && p >= ITEMS[i].g) { G.announced |= 1u << i; g_log("Something glints on the lakebed."); }
    for (int i = 0; i < NSPRINGS; i++)
        if (!(G.springsOpen >> i & 1) && p >= SPRINGS[i].g && !G.won) {
            G.springsOpen |= 1u << i;
            if (!explored("chapel")) {
                char r[24]; fmt_num(SPRINGS[i].rate, r);
                g_log("A spring has burst open on the lakebed: the %s, pouring in %s gal/s. Walk out and cap it.", SPRINGS[i].name, r);
                hud_toast("The %s has burst open! Cap it on the lakebed.", SPRINGS[i].name);
            }
        }
    if (!(G.announced & FOUND_PLUG) && g_plug_exposed()) { G.announced |= FOUND_PLUG; g_log("At the deepest point: a giant iron ring."); }
    double b = g_bottling(); if (b > 0) earn(b * dt);
    G.saveTimer -= dt;
    if (G.saveTimer <= 0) { g_save(); G.saveTimer = 5; }
}

/* ---------- actions ---------- */
void g_scoop(v3 at) {
    if (G.water <= 0) { hud_toast("The lake is empty."); return; }
    double amt = g_bucket(), room = g_carry_cap() - G.carried;
    if (room < amt) amt = room;
    if (G.water < amt) amt = G.water;
    if (amt <= 0) { hud_toast("Your bucket is full. Carry it to the storage tank."); return; }
    G.water -= amt; G.carried += amt;
    w_splash(at);
    char n[24], s[48]; fmt_num(amt, n); ksnprintf(s, sizeof s, "+%s gal", n);
    hud_float(at, s, 0xDEF5F7);
}

static int pour(void) {
    if (G.carried <= 0) { hud_toast("Your bucket is empty. Scoop some water at the shore."); return 0; }
    double amt = G.carried, room = g_capacity() - G.tank; if (room < amt) amt = room;
    if (amt <= 0) { hud_toast("The tank is full. Sell it first."); return 0; }
    G.carried -= amt; G.tank += amt;
    char n[24], s[64]; fmt_num(amt, n); ksnprintf(s, sizeof s, "+%s gal in the tank", n);
    hud_float(v_add(W.tank, V3(0, 6, 0)), s, 0xDEF5F7);
    return 1;
}

void g_sell_all(void) {
    if (G.tank <= 0) { hud_toast("The tank is empty."); return; }
    double v = G.tank * g_price(); earn(v); G.tank = 0;
    char m[24], s[40]; fmt_money(v, m); ksnprintf(s, sizeof s, "+%s", m);
    hud_float(v_add(W.tank, V3(0, 9, 0)), s, ACCENT);
}

void g_tank_action(void) {
    if (G.carried > 0 && G.tank < g_capacity() - .01) pour();
    else if (G.tank > 0) g_sell_all();
    else hud_toast("The tank is empty. Bring water from the lake.");
}

void g_tank_prompt(char *out, int n) {
    char a[24];
    double room = g_capacity() - G.tank;
    if (G.carried > 0 && G.tank < g_capacity() - .01) { fmt_num(G.carried < room ? G.carried : room, a); ksnprintf(out, n, "[E]  Pour %s gal into the tank", a); }
    else if (G.tank > 0) { fmt_money(G.tank * g_price(), a); ksnprintf(out, n, "[E]  Sell the tank (%s)", a); }
    else ksnprintf(out, n, "Storage tank (empty)");
}

void g_buy_pump(int i) {
    double c = g_pump_cost(i);
    if (G.cash < c || G.pumps[i] >= PUMPS[i].max) return;
    G.cash -= c; G.pumps[i]++;
    if (G.pumps[i] == 1) {
        char nm[48]; strncpy0(nm, PUMPS[i].name, sizeof nm);
        for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        g_log("Installed a %s.", nm);
    }
}

void g_buy_upgrade(int i) {
    int n = G.upg[i];
    if (n >= UPGRADES[i].max || G.cash < upg_cost(i, n)) return;
    G.cash -= upg_cost(i, n); G.upg[i]++;
    g_log("Upgrade: %s (level %d).", UPGRADES[i].name, n + 1);
}

static char body[2048];

void g_collect_item(int i) {
    if (found_item(i)) return;
    const struct item_def *it = &ITEMS[i];
    G.found |= 1u << i; earn(it->val);
    char m[24]; fmt_money(it->val, m);
    if (it->val > 0) g_log("Found: %s (%s).", it->name, m); else g_log("Found: %s.", it->name);
    char s[40]; ksnprintf(s, sizeof s, "+%s", m);
    hud_float(v_add(W.item[i], V3(0, 3, 0)), it->val > 0 ? s : "Found!", ACCENT);
    int b = it->unlock ? building_index(it->unlock) : -1;
    int o = ksnprintf(body, sizeof body, "%s", it->desc);
    if (it->lore) o += ksnprintf(body + o, sizeof body - o, "\n\n" LORE "%s", it->lore);
    if (it->val > 0) o += ksnprintf(body + o, sizeof body - o, "\n\n" GRN "Sold to a collector for %s", m);
    if (b >= 0) o += ksnprintf(body + o, sizeof body - o, "\n\n" GRN "Unlocks: %s", BUILDINGS[b].name);
    hud_modal(it->name, body, 1, "Keep draining", (modal_fn)0);
}

static int pendingBuilding;
static void do_explore(void) {
    const struct building_def *b = &BUILDINGS[pendingBuilding];
    G.explored |= 1u << pendingBuilding; earn(b->cash);
    g_log("Explored the %s. %s", b->name, b->perk);
    int o = ksnprintf(body, sizeof body, LORE "%s", b->lore);
    char m[24]; fmt_money(b->cash, m);
    if (b->cash > 0) o += ksnprintf(body + o, sizeof body - o, "\n\nFound %s inside.", m);
    ksnprintf(body + o, sizeof body - o, "\n\n" GRN "%s", b->perk);
    hud_modal(b->name, body, 1, "Excellent", (modal_fn)0);
    g_save();
}
void g_explore_building(int i) {
    const struct building_def *b = &BUILDINGS[i];
    if (G.explored >> i & 1) {
        ksnprintf(body, sizeof body, LORE "%s\n\n" GRN "Active: %s", b->lore, b->perk);
        hud_modal(b->name, body, 1, "Close", (modal_fn)0); return;
    }
    int need = b->req ? item_index(b->req) : -1;
    if (need >= 0 && !found_item(need)) {
        ksnprintf(body, sizeof body, "Locked. You'll need the %s. It's somewhere on the lakebed, about %d%% of the way down.",
                  ITEMS[need].name, (int)(ITEMS[need].g * 100 + 0.5f));
        hud_modal(b->name, body, 1, "Back to work", (modal_fn)0); return;
    }
    pendingBuilding = i;
    if (need >= 0) ksnprintf(body, sizeof body, "You have the %s. Go inside?", ITEMS[need].name);
    else ksnprintf(body, sizeof body, "The door is unlocked. Go inside?");
    hud_modal(b->name, body, 2, "Explore", do_explore, "Later", (modal_fn)0);
}

void g_cap_spring(int i) {
    const struct spring_def *s = &SPRINGS[i];
    if (!g_spring_flowing(i)) { if (G.springsCapped >> i & 1) hud_toast("The %s is capped.", s->name); else hud_toast("The springs are silent."); return; }
    char m[24]; fmt_money(s->cost, m);
    if (G.cash < s->cost) { hud_toast("Capping the %s costs %s in iron and cement.", s->name, m); return; }
    G.cash -= s->cost; G.springsCapped |= 1u << i;
    char r[24]; fmt_num(s->rate, r);
    g_log("Capped the %s (%s). %s gal/s less flowing back in.", s->name, m, r);
    hud_toast("%s capped!", s->name);
}

/* ---------- creek missions ---------- */
void g_creek_prompt(int i, char *out, int n) {
    const struct creek_def *c = &CREEKS[i];
    char m[24];
    if (G.creekState[i] == 1) { ksnprintf(out, n, "%s (dammed)", c->name); return; }
    if (G.creekState[i] == 2) { fmt_money(c->rate * SPRING_WATER_PRICE, m); ksnprintf(out, n, "%s bottling works (+%s/s)", c->name, m); return; }
    int have = g_creek_have(i);
    if (have >= c->need) ksnprintf(out, n, "[E]  Dam or divert %s", c->name);
    else ksnprintf(out, n, "[E]  %s: %d/%d %s", c->name, have, c->need, c->item);
}

static int pendingCreek;
static void finish_creek(int i, int state) {
    const struct creek_def *c = &CREEKS[i];
    if (i == 0) G.stones -= c->need; else if (i == 1) G.planks -= c->need; else G.valve = 0;
    if (state == 2) G.cash -= c->divertCost;
    G.creekState[i] = state;
    char r[24], m[24]; fmt_num(c->rate, r); fmt_money(c->rate * SPRING_WATER_PRICE, m);
    if (state == 1) g_log("Dammed %s. %s gal/s less flowing into the lake.", c->name, r);
    else g_log("Diverted %s to a bottling works: %s gal/s less in the lake, +%s/s in spring water sales.", c->name, r, m);
    hud_toast(state == 1 ? "%s dammed!" : "%s bottling works open!", c->name);
    g_save();
}
static void dam_it(void) { finish_creek(pendingCreek, 1); }
static void divert_it(void) {
    const struct creek_def *c = &CREEKS[pendingCreek];
    char m[24]; fmt_money(c->divertCost, m);
    if (G.cash < c->divertCost) hud_toast("The bottling works costs %s.", m); else finish_creek(pendingCreek, 2);
}
void g_creek_action(int i) {
    const struct creek_def *c = &CREEKS[i]; int have = g_creek_have(i);
    char r[24], m[24], d[24]; fmt_num(c->rate, r); fmt_money(c->rate * SPRING_WATER_PRICE, m); fmt_money(SPRING_WATER_PRICE, d);
    if (G.creekState[i] == 1) { hud_modal(c->name, "Dammed. The water pools behind it and soaks back into the hill.", 1, "Close", (modal_fn)0); return; }
    if (G.creekState[i] == 2) {
        char t[64]; ksnprintf(t, sizeof t, "%s bottling works", c->name);
        ksnprintf(body, sizeof body, "Clean spring water, piped to the bottling shed and sold for %s a gallon:\n\n" GRN "+%s every second", d, m);
        hud_modal(t, body, 1, "Close", (modal_fn)0); return;
    }
    if (have < c->need) {
        ksnprintf(body, sizeof body, "%s gal/s pour from this spring into Merrow Lake.\n\nMission: %s\n\nProgress: %d / %d %s.", r, c->task, have, c->need, c->item);
        hud_modal(c->name, body, 1, "On it", (modal_fn)0); return;
    }
    pendingCreek = i;
    ksnprintf(body, sizeof body, "%s gal/s pour from this spring into Merrow Lake.\n\nYou have the %s. Dam it for good, or pipe this clean water to a bottling works: "
              "it stops flowing into the lake and sells for %s a gallon.\n\n" GRN "Bottling: +%s/s", r, c->item, d, m);
    static char lbl[48]; char dc[24]; fmt_money(c->divertCost, dc); ksnprintf(lbl, sizeof lbl, "Divert (%s)", dc);
    hud_modal(c->name, body, 3, "Dam it", dam_it, lbl, divert_it, "Later", (modal_fn)0);
}

void g_take_stone(int k) {
    if (G.taken >> k & 1) return;
    G.taken |= 1u << k; G.stones++;
    hud_toast("River stone (%d/5). Take them to the Arm Creek spring.", G.stones);
}
void g_take_plank(void) {
    if (G.planks >= 4) { hud_toast("Your arms are full. Carry the planks to the Fern Brook spring."); return; }
    G.planks++;
    hud_toast("Plank (%d/4). Take them to the Fern Brook spring.", G.planks);
}
void g_take_valve(void) {
    if (G.taken & TAKEN_VALVE) return;
    G.taken |= TAKEN_VALVE; G.valve = 1;
    hud_modal("Old valve wheel", "A cast-iron valve wheel, stamped MERROW SPRING HOUSE 1890. It belongs on the spring house where Mill Run rises, in the hills to the east.", 1, "Take it", (modal_fn)0);
}

void g_find_secret(int i) {
    const struct secret_def *s = &SECRETS[i];
    if (G.secrets >> i & 1) { ksnprintf(body, sizeof body, LORE "%s", s->lore); hud_modal(s->name, body, 1, "Close", (modal_fn)0); return; }
    G.secrets |= 1u << i; earn(s->cash);
    char m[24]; fmt_money(s->cash, m);
    if (s->cash > 0) g_log("Shore secret: %s (%s).", s->name, m); else g_log("Shore secret: %s.", s->name);
    int o = ksnprintf(body, sizeof body, LORE "%s", s->lore);
    if (s->cash > 0) o += ksnprintf(body + o, sizeof body - o, "\n\n" GRN "+%s", m);
    ksnprintf(body + o, sizeof body - o, "\n\nShore secrets %d / %d", popcount32(G.secrets), NSECRETS);
    hud_modal(s->name, body, 1, "Nice", (modal_fn)0);
}

static void pull_plug(void) {
    G.won = 1; G.water = 0; G.found |= FOUND_PLUG; earn(1000000);
    g_log("You pulled the plug. Merrow Lake is drained.");
    g_log("Under the plug: an old stone doorway leading down into a cave.");
    hud_modal("Lake drained",
        "The chain goes taut, the oak groans, and with one long gulp the last of Merrow Lake spirals away into the dark.\n\n"
        "When the mud settles, the hole the plug was hiding isn't a drain at all. It's a doorway: worn stone steps leading down into the dark, "
        "and from somewhere far below, a faint blue light.", 1, "Go and look", (modal_fn)0);
    g_save();
}
void g_ask_pull_plug(void) {
    ksnprintf(body, sizeof body, "%s\n\nPull it?", PLUG_DESC);
    hud_modal(PLUG_NAME, body, 2, "Pull the plug", pull_plug, "Not yet", (modal_fn)0);
}

/* ---------- the Merrow Gate ---------- */
void g_gate_prompt(char *out, int n) {
    if (G.gateOpen) { ksnprintf(out, n, "[E]  Step through the Gate"); return; }
    int ready = 0;
    for (int i = 0; i < NRELICS; i++) if (found_item(item_index(RELICS[i])) && !(G.relicsSet >> i & 1)) ready++;
    if (ready > 0) ksnprintf(out, n, "[E]  Set %d relic%s in the Gate (%d / %d)", ready, ready > 1 ? "s" : "", popcount32(G.relicsSet), NRELICS);
    else ksnprintf(out, n, "[E]  Read the Gate  " DOT "  %d / %d sockets filled", popcount32(G.relicsSet), NRELICS);
}

static void gate_opened(void) {
    hud_modal("The Gate opens",
        "The last relic clicks home. One by one the runes catch light, the ground hums through your boots, and the empty ring fills with something "
        "that moves like water but isn't. Through it, faintly: the same valley under a green sky, and a lake that is full.", 1, "Step closer", (modal_fn)0);
}

static const char *GLIMPSE[NWORLDS] = {
    "Through the Gate: blue sky, birdsong, your own valley. And, somehow, a full lake again.",
    "Through the Gate: grey rain on grey water, and sticking up out of the lake, a church spire.",
    "On the far side it is always dusk. Green light ripples across the sky, and the lake is full to the brim.",
};

static void step_through_ex(const char *title, const char *footer) {
    int keep[NUPG]; memcpy(keep, G.upg, sizeof keep);
    int crossings = G.ngPlus + 1;
    g_reset();
    memcpy(G.upg, keep, sizeof keep); G.ngPlus = crossings;
    if (world_no() == 0) g_log(G.keeper ? "You came home through the Gate. Merrow Lake is full again, and the springs are quiet."
                                        : "You came home through the Gate. Merrow Lake is full again, and the springs are stronger than you remember.");
    else if (world_no() == 1) g_log("You stepped through the Merrow Gate into the Drowned Merrow: grey rain, and a village under the lake. The church spire still shows above the water.");
    else g_log("You stepped through the Merrow Gate into the Other Merrow: endless dusk, a green sky, and a full lake that wants draining.");
    g_save();
    w_refresh();
    hud_credits(crossings, title, footer);
}
static void step_through(void) { step_through_ex(0, 0); }
static void go_down(void) { W.scene = SCENE_STILL; p_teleport(V3(0, 0, -2.5f), 0); }

static void ask_step_through(void) {
    int finds = popcount32(G.found & 0xFFFF);
    int next = (G.ngPlus + 1) % NWORLDS;
    char e[24]; fmt_money(G.earned, e);
    int o = ksnprintf(body, sizeof body, "%s\n\nStep through to start again in %s. You keep your upgrades and journal pages%s\n\nJournal pages: %d / %d",
                      GLIMPSE[next], WORLDS[next].name, G.keeper ? "." : "; the springs push back harder.", popcount32(G.pages), NPAGES);
    if (popcount32(G.pages) >= NPAGES) o += ksnprintf(body + o, sizeof body - o, "\n\n" AMB "The three pages are warm in your pocket. Under the swirl, the Gate is showing you a second way: down.");
    ksnprintf(body + o, sizeof body - o, "\n\n" GRN "This lake: %d min " DOT " %d / %d finds " DOT " %d / %d secrets " DOT " %s earned",
              (int)(G.t / 60), finds, NITEMS, popcount32(G.secrets), NSECRETS, e);
    if (popcount32(G.pages) >= NPAGES) hud_modal(WORLDS[next].name, body, 3, "Step through", step_through, "Go down", go_down, "Not yet", (modal_fn)0);
    else hud_modal(WORLDS[next].name, body, 2, "Step through", step_through, "Not yet", (modal_fn)0);
}

static void open_gate_now(void) { gate_opened(); }

void g_gate_action(void) {
    if (world_no() == 0) g_add_page(0);
    if (G.gateOpen) { ask_step_through(); return; }
    char added[160] = ""; int nadded = 0;
    for (int i = 0; i < NRELICS; i++) {
        int it = item_index(RELICS[i]);
        if (found_item(it) && !(G.relicsSet >> i & 1)) {
            G.relicsSet |= 1u << i;
            int l = strlen(added);
            ksnprintf(added + l, sizeof added - l, "%s%s", nadded ? ", " : "", ITEMS[it].name);
            nadded++;
        }
    }
    if (nadded == 0) {
        char missing[200] = ""; int nm = 0;
        for (int i = 0; i < NRELICS; i++) if (!(G.relicsSet >> i & 1)) {
            int l = strlen(missing);
            ksnprintf(missing + l, sizeof missing - l, "%s%s", nm ? ", " : "", ITEMS[item_index(RELICS[i])].name);
            nm++;
        }
        for (char *p = missing; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (nm) ksnprintf(body, sizeof body, "%s\n\n" AMB "Still missing: %s.", GATE_LORE, missing);
        else ksnprintf(body, sizeof body, "%s", GATE_LORE);
        hud_modal(GATE_NAME, body, 1, "Close", (modal_fn)0);
        return;
    }
    g_save();
    int set = popcount32(G.relicsSet);
    if (set < NRELICS) {
        hud_toast("%s set in the Gate. %d / %d.", added, set, NRELICS);
        g_log("Set in the Gate: %s.", added);
        if (set == nadded) hud_modal(GATE_NAME, GATE_LORE, 1, "Close", (modal_fn)0);
        return;
    }
    int first = nadded == set;
    G.gateOpen = 1; g_save();
    g_log("The six sockets are full. The Merrow Gate is open.");
    if (first) { hud_modal(GATE_NAME, GATE_LORE, 1, "Set the relics", open_gate_now); return; }
    gate_opened();
}

/* ---------- journal pages and the Still Room ---------- */
void g_add_page(int i) {
    if (G.pages >> i & 1) return;
    G.pages |= 1u << i;
    g_log("Journal page: %s. (%d / %d)", PAGES[i].title, popcount32(G.pages), NPAGES);
    g_save();
    hud_toast("Journal page %d / %d: %s", popcount32(G.pages), NPAGES, PAGES[i].title);
}

void g_read_page(int i, const char *intro) {
    int isNew = !(G.pages >> i & 1);
    g_add_page(i);
    int o = 0;
    if (intro) o += ksnprintf(body + o, sizeof body - o, "%s\n\n", intro);
    o += ksnprintf(body + o, sizeof body - o, LORE "%s\n\n" MUT "%s", PAGES[i].text, PAGES[i].where);
    if (isNew) {
        o += ksnprintf(body + o, sizeof body - o, "\n\n" GRN "Journal page %d / %d", popcount32(G.pages), NPAGES);
        if (popcount32(G.pages) >= NPAGES) ksnprintf(body + o, sizeof body - o, "\n\n" AMB "All three pages. Open a Gate and follow them down.");
    }
    hud_modal(PAGES[i].title, body, 1, "Close", (modal_fn)0);
}

int g_drowned_searched(void) { return popcount32(G.taken >> 6 & 0x7F); }

void g_search_drowned(int i) {
    char m[24];
    if (i < 0) {
        int first = !(G.taken & TAKEN_CHURCH);
        G.taken |= TAKEN_CHURCH;
        fmt_money(DROWNED_CHURCH_CASH, m);
        if (first) { earn(DROWNED_CHURCH_CASH); g_log("Searched the sunken church (%s in the poor box).", m); }
        static char intro[400];
        int o = ksnprintf(intro, sizeof intro, "The pews are buried to the seat in silt. The bell lies cracked in the aisle where it fell. In the pulpit, in a jar sealed with wax, a page in a woman's hand.");
        if (first) ksnprintf(intro + o, sizeof intro - o, "\n\n" GRN "+%s from the poor box", m);
        g_read_page(1, intro);
        return;
    }
    const struct house_def *h = &HOUSES[i];
    if (G.taken & TAKEN_HOUSE(i)) { ksnprintf(body, sizeof body, LORE "%s", h->lore); hud_modal(h->name, body, 1, "Close", (modal_fn)0); return; }
    G.taken |= TAKEN_HOUSE(i);
    earn(h->cash); fmt_money(h->cash, m);
    char nm[48]; strncpy0(nm, h->name, sizeof nm); for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    g_log("Searched %s in the drowned village (%s).", nm, m);
    ksnprintf(body, sizeof body, LORE "%s\n\n" GRN "+%s\n\nDrowned village: %d / %d searched", h->lore, m, g_drowned_searched(), NHOUSES + 1);
    hud_modal(h->name, body, 1, "Nice", (modal_fn)0);
}

static void take_chair(void) {
    G.keeper = 1; G.ending = 1;
    g_log("You took the Keeper's chair. Above you, in every valley, the springs went quiet.");
    W.scene = SCENE_OUTSIDE;
    step_through_ex("You took the Keeper's chair", "The springs are quiet now, in every valley. Merrow Lake is yours to drain, at your own pace.");
}
static void go_home(void) {
    G.ending = 2;
    g_log("You left the chair empty and walked home, with the Merrows' savings: $250,000.");
    W.scene = SCENE_OUTSIDE;
    step_through_ex("You walked home", "The chair is still there, if you ever want it. The Merrows' savings came home with you: $250,000.");
    earn(250000);
    g_save();
}
void g_read_last_page(void) {
    ksnprintf(body, sizeof body, LORE "%s", STILL_ROOM_PAGE);
    hud_modal("The last page", body, 3, "Take the chair", take_chair, "Go home", go_home, "Not yet", (modal_fn)0);
}

/* ---------- save / load: /home/.merrow/save.txt, one "key value" per line ---------- */
#define SAVE_DIR "/home/.merrow"
#define SAVE_PATH "/home/.merrow/save.txt"
static char sbuf[16384];

int g_save(void) {
    int o = 0;
#define PUT(...) o += ksnprintf(sbuf + o, sizeof sbuf - o, __VA_ARGS__)
    PUT("merrow-lake 1\n");
    PUT("water %ld\ntank %ld\ncash %ld\nearned %ld\nt %ld\ncarried %ld\nwon %d\n", (long)(G.water * 1000), (long)(G.tank * 1000),
        (long)(G.cash * 1000), (long)(G.earned * 1000), (long)(G.t * 1000), (long)(G.carried * 1000), G.won);
    PUT("pumps"); for (int i = 0; i < NPUMPS; i++) PUT(" %d", G.pumps[i]); PUT("\n");
    PUT("upg"); for (int i = 0; i < NUPG; i++) PUT(" %d", G.upg[i]); PUT("\n");
    PUT("found %u\nexplored %u\nsecrets %u\nsopen %u\nscapped %u\n", G.found, G.explored, G.secrets, G.springsOpen, G.springsCapped);
    PUT("creeks %d %d %d\nstones %d\nplanks %d\nvalve %d\ntaken %u\n", G.creekState[0], G.creekState[1], G.creekState[2], G.stones, G.planks, G.valve, G.taken);
    PUT("relics %u\ngate %d\nngplus %d\npages %u\nending %d\nkeeper %d\n", G.relicsSet, G.gateOpen, G.ngPlus, G.pages, G.ending, G.keeper);
    PUT("hour %d\n", (int)(W.hour * 100));
    for (int i = G.nlog - 1; i >= 0; i--) PUT("log %s\n", G.log[i]);
#undef PUT
    fs_mkdir(SAVE_DIR);
    return fs_write(SAVE_PATH, sbuf, o) >= 0;
}

static long num(const char **p) {
    while (**p == ' ') (*p)++;
    int neg = 0; if (**p == '-') { neg = 1; (*p)++; }
    long v = 0; while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return neg ? -v : v;
}

int g_load(void) {
    long n = fs_read(SAVE_PATH, sbuf, 0, sizeof sbuf - 1);
    if (n <= 0) return 0;
    sbuf[n] = 0;
    if (strncmp(sbuf, "merrow-lake 1", 13)) return 0;
    g_reset(); G.nlog = 0;
    char *line = sbuf;
    while (*line) {
        char *e = line; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        const char *p = line;
        char key[16]; int k = 0; while (*p && *p != ' ' && k < 15) key[k++] = *p++; key[k] = 0;
#define IS(s) (!strcmp(key, s))
        if (IS("water")) G.water = num(&p) / 1000.0;
        else if (IS("tank")) G.tank = num(&p) / 1000.0;
        else if (IS("cash")) G.cash = num(&p) / 1000.0;
        else if (IS("earned")) G.earned = num(&p) / 1000.0;
        else if (IS("t")) G.t = num(&p) / 1000.0;
        else if (IS("carried")) G.carried = num(&p) / 1000.0;
        else if (IS("won")) G.won = (int)num(&p);
        else if (IS("pumps")) for (int i = 0; i < NPUMPS; i++) G.pumps[i] = (int)num(&p);
        else if (IS("upg")) for (int i = 0; i < NUPG; i++) G.upg[i] = (int)num(&p);
        else if (IS("found")) G.found = (uint32_t)num(&p);
        else if (IS("explored")) G.explored = (uint32_t)num(&p);
        else if (IS("secrets")) G.secrets = (uint32_t)num(&p);
        else if (IS("sopen")) G.springsOpen = (uint32_t)num(&p);
        else if (IS("scapped")) G.springsCapped = (uint32_t)num(&p);
        else if (IS("creeks")) for (int i = 0; i < NCREEKS; i++) G.creekState[i] = (int)num(&p);
        else if (IS("stones")) G.stones = (int)num(&p);
        else if (IS("planks")) G.planks = (int)num(&p);
        else if (IS("valve")) G.valve = (int)num(&p);
        else if (IS("taken")) G.taken = (uint32_t)num(&p);
        else if (IS("relics")) G.relicsSet = (uint32_t)num(&p);
        else if (IS("gate")) G.gateOpen = (int)num(&p);
        else if (IS("ngplus")) G.ngPlus = (int)num(&p);
        else if (IS("pages")) G.pages = (uint32_t)num(&p);
        else if (IS("ending")) G.ending = (int)num(&p);
        else if (IS("keeper")) G.keeper = (int)num(&p);
        else if (IS("hour")) W.hour = num(&p) / 100.0f;
        else if (IS("log")) { if (*p == ' ') p++; g_log("%s", p); }
#undef IS
        *e = save;
        line = *e ? e + 1 : e;
    }
    return 1;
}

int g_start(void) {
    g_reset(); G.ngPlus = 0; G.pages = 0; G.ending = 0; G.keeper = 0;
    W.hour = 10;
    int loaded = g_load();
    float p = g_progress();
    for (int i = 0; i < NITEMS; i++) if (p >= ITEMS[i].g) G.announced |= 1u << i;
    intro_logs();
    G.saveTimer = 5;
    (void)money_tmp;
    return loaded;
}
