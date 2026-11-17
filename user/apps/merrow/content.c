/* Merrow Lake: all the tunable game content, the same numbers and words as the Unity game's Content.cs.
 * Text is Windows-1252 (AXIOM's character set). */
#include "merrow.h"

#define DASH "\x97"
#define DOT "\xB7"

const struct pump_def PUMPS[NPUMPS] = {
    { "hand",   "Hand pump",         3,     20,     8, "A lever, a hose, and your shoulders.",      0xC9B458 },
    { "gas",    "Gas pump",          25,    250,    6, "Two-stroke, loud, gets it done.",           0xD9792B },
    { "diesel", "Diesel trash pump", 200,   3000,   5, "Trailer-mounted, 4-inch intake.",           0xC94A32 },
    { "ind",    "Industrial pump",   1500,  40000,  4, "Municipal-grade centrifugal unit.",         0x4F7FAE },
    { "siphon", "Siphon array",      12000, 600000, 2, "Forty pipes over the ridge to the valley.", 0x7A5AA8 },
};

const struct spring_def SPRINGS[NSPRINGS] = {
    { "s1", "Reed spring",     .06f, 1.05f,  6,     25 },
    { "s2", "Willow spring",   .14f, 2.5f,   25,    180 },
    { "s3", "Cold spring",     .24f, -0.6f,  110,   1200 },
    { "s4", "Iron spring",     .36f, -2.3f,  450,   7500 },
    { "s5", "Deep spring",     .50f, 0.25f,  1600,  45000 },
    { "s6", "Black spring",    .66f, -1.35f, 5200,  260000 },
    { "s7", "Merrow's Throat", .80f, 2.9f,   16000, 1200000 },
};

const struct upg_def UPGRADES[NUPG] = {
    { "bucket", "Bigger bucket", 8 }, { "tank", "Storage tank", 12 }, { "pipe", "Pipeline to town", 1 },
    { "filter", "Filtration plant", 8 }, { "yoke", "Shoulder yoke", 4 }, { "boots", "Trail boots", 3 },
};

double upg_cost(int i, int n) {
    switch (i) {
    case 0: return 15 * m_powd(3, n);
    case 1: return 25 * m_powd(2.4, n);
    case 2: return 400;
    case 3: return 120 * m_powd(4, n);
    case 4: return 40 * m_powd(3.2, n);
    default: return 150 * m_powd(5, n);
    }
}

void upg_desc(int i, int n, char *out, int max) {
    char a[24];
    switch (i) {
    case 0: fmt_num(8 * m_powd(2, n + 1), a); ksnprintf(out, max, "Scoop %s gal at a time", a); break;
    case 1: fmt_num(100 * m_powd(2, n + 1), a); ksnprintf(out, max, "Hold %s gal. Pumps stop when it's full", a); break;
    case 2: ksnprintf(out, max, "Sells the tank automatically, one full tank per second"); break;
    case 3: ksnprintf(out, max, "Cleaner water sells for 30%% more"); break;
    case 4: ksnprintf(out, max, "Carry %d scoops per trip to the tank", 3 + 3 * (n + 1)); break;
    default: ksnprintf(out, max, "Walk, run and swim %d%% faster", 15 * (n + 1)); break;
    }
}

const struct item_def ITEMS[NITEMS] = {
    { "lure",    "Fishing lure",        .05f, 15,     SHAPE_CAPSULE, .6f, .9f, .6f,   0xE04A3A, "Hand-painted and still sharp. Someone lost a big one here.", 0, 0 },
    { "boot",    "Old wader boot",      .09f, 5,      SHAPE_CUBE,    1, 1.6f, 2,      0x3D4A2E, "Size 11. The other one is probably nearby. It is not.", 0, 0 },
    { "bottle",  "Message in a bottle", .13f, 40,     SHAPE_CAPSULE, .6f, 1, .6f,     0x6FBF8A, "The cork is waxed shut.", "\"If you are reading this, keep going. The plug is real.\" " DASH " E. Merrow, 1961", 0 },
    { "ckey",    "Cabin key",           .17f, 0,      SHAPE_CUBE,    .4f, .3f, 1.6f,  0xD4A93C, "The tag reads \"Merrow cabin, spare.\" It should open the cabin on the west shore.", 0, "cabin" },
    { "coins",   "Wishing coins",       .21f, 250,    SHAPE_CYL,     1.8f, .15f, 1.8f,0xC0C4C8, "Decades of wishes. Mostly pennies, a few silver dimes.", 0, 0 },
    { "bike",    "Bicycle",             .26f, 400,    SHAPE_CUBE,    .3f, 1.6f, 3,    0x2F6FB0, "A 1970s ten-speed. The bell still works.", 0, 0 },
    { "gear",    "Mill gear",           .31f, 0,      SHAPE_CYL,     2.2f, .3f, 2.2f, 0x5A5550, "A cast-iron cog the size of a dinner plate. It belongs in the old mill.", 0, "mill" },
    { "cart",    "Shopping cart",       .36f, 800,    SHAPE_CUBE,    1.6f, 1.6f, 2.4f,0x9AA3A8, "From a grocery store that closed in 1994. Nobody knows how it got here.", 0, 0 },
    { "canoe",   "Sunken canoe",        .42f, 2000,   SHAPE_CAPSULE, 1.2f, 4, 1,      0x9C5B2E, "Cedar strip, hand-built. Restorers will pay well.", 0, 0 },
    { "lens",    "Brass lens",          .48f, 0,      SHAPE_SPHERE,  1.6f, 1.6f, .6f, 0xE6C35A, "A heavy Fresnel lens panel. The lighthouse has been dark since it went missing.", 0, "lighthouse" },
    { "watch",   "Pocket watch",        .54f, 5000,   SHAPE_CYL,     1, .2f, 1,       0xD8B24A, "Engraved \"E.M.\" Stopped at 4:47.", 0, 0 },
    { "clapper", "Bell clapper",        .61f, 0,      SHAPE_CAPSULE, .7f, 1.4f, .7f,  0x8A6A3A, "The chapel bell has hung silent without it for sixty years.", 0, "chapel" },
    { "car",     "1957 sedan",          .68f, 30000,  SHAPE_CUBE,    3, 2, 6,         0x5FA8A0, "Two-tone paint under the silt. A collector is already on the phone.", 0, 0 },
    { "eyepiece","Telescope eyepiece",  .75f, 0,      SHAPE_CYL,     .6f, 1, .6f,     0xB08A3A, "Ground glass in a brass barrel. It fits the observatory on the ridge.", 0, "observatory" },
    { "chest",   "Strongbox",           .83f, 120000, SHAPE_CUBE,    2.4f, 1.6f, 1.6f,0x5B3A1E, "Gold certificates, bundled and sealed in wax paper. Bone dry.", 0, 0 },
    { "tusk",    "Mammoth tusk",        .90f, 250000, SHAPE_CAPSULE, .8f, 4, .8f,     0xEFE6CF, "Ice Age ivory. The state museum wants it badly.", 0, 0 },
};

const struct building_def BUILDINGS[NBUILD] = {
    { "boathouse",   "Boathouse",         0,          50,    -2.05f, 1.07f, "Diver's chart: you can see what lies under the water.", "Rope, rot, and a chart of the lakebed pinned to the wall. Someone marked it with sixteen small X's." },
    { "cabin",       "Merrow Cabin",      "ckey",     500,   -2.55f, 1.30f, "Grandpa's bucket: your scoops hold 4x more.", "A journal on the table: \"Father sealed the lake in 1888 with a plug of oak and iron. One day someone ought to pull it.\"" },
    { "chapel",      "Hill Chapel",       "clapper",  20000, -2.95f, 1.45f, "The lakebed springs go quiet. (The hillside creeks still need damming.)", "You hang the clapper and ring the bell once. Across the lake, the bubbling springs fall still." },
    { "mill",        "Old Mill",          "gear",     2000,  -0.35f, 1.22f, "The water wheel turns: +150 gal/s of free pumping.", "The gear drops into place with a clunk. The wheel creaks, then turns, lifting water up the old flume." },
    { "lighthouse",  "Lighthouse",        "lens",     8000,  -0.95f, 1.12f, "The beacon draws buyers from three counties: water sells for 1.5x.", "The lamp catches and the beam sweeps the valley for the first time since 1961." },
    { "observatory", "Ridge Observatory", "eyepiece", 60000, -1.57f, 2.35f, "Star charts mark the deep channels: all pumps run 2x faster.", "Taped to the telescope is a hand-drawn map of the lakebed, with one spot circled at the very bottom." },
};

const struct secret_def SECRETS[NSECRETS] = {
    { "mailbox", "Rusty mailbox", 10,  -2.30f, 1.38f, "A letter from 1961: \"FINAL NOTICE. Merrow Lake Drainage Co. Permit renewed in perpetuity.\"" },
    { "well",    "Stone well",    200, -1.75f, 1.22f, "Coins glint at the bottom, and the well hums like the lake is breathing." },
    { "tree",    "Hollow ash",    25,  2.95f,  1.28f, "Carved inside the hollow trunk: \"E + M, 1923\". A tin of old bills is tucked into the hollow." },
    { "grave",   "Gravestone",    0,   -2.78f, 1.55f, "\"Elias Merrow, Keeper of the Plug. 1888-1961. He held it shut so they would not have to.\"" },
    { "cairn",   "Rock cairn",    500, -1.20f, 1.75f, "Under the top stone, a surveyor's pin and a pouch of silver dollars." },
};

const struct creek_def CREEKS[NCREEKS] = {
    { "arm",     "Arm Creek",  1.5, 5, "river stones", 300,  "Gather glinting river stones along its banks, then build a stone dam at the spring." },
    { "fern",    "Fern Brook", 4,   4, "planks",       1500, "Carry planks from the plank stack beside Merrow Cabin, then build a sluice gate at the spring." },
    { "millrun", "Mill Run",   10,  1, "valve wheel",  6000, "The old spring house has lost its valve wheel. Find it somewhere near the south shore and fit it." },
};

const char *const RELICS[NRELICS] = { "tusk", "gear", "lens", "clapper", "eyepiece", "watch" };
const char *GATE_NAME = "The Merrow Gate";
const char *GATE_LORE =
    "A ring of carved stone standing in a cavern under the lakebed. It is far older than the Merrows: the carvings are worn smooth, "
    "and moss-black runes circle six empty sockets. Elias's last page, folded under the plug: \"I found it in '88 when we dug the well. "
    "It had been buried for centuries, and it was not dead. So I flooded the valley over it. If you are reading this, you were stubborn enough "
    "to drain it. The sockets want the old things. Put them back and see where it goes.\"";

const struct world_def WORLDS[NWORLDS] = {
    { "Merrow Lake", "Five million gallons, one bucket, and a plug nobody has touched since 1888.", "Home again. Merrow Lake is full." },
    { "The Drowned Merrow", "Grey rain, and a whole village under the water. The church spire still shows. Drain it and see what surfaces.", "Welcome to the Drowned Merrow. There's a village down there." },
    { "The Other Merrow", "Endless dusk, a green sky, and a lake that wants draining.", "Welcome to the Other Merrow. Drain it." },
};

const struct page_def PAGES[NPAGES] = {
    { "elias", "Elias Merrow, 1888", "Merrow Lake: folded under the plug",
      "\"I found it in '88 when we dug the well. It had been buried for centuries, and it was not dead. So I flooded the valley over it. "
      "If you are reading this, you were stubborn enough to drain it. The sockets want the old things. Put them back and see where it goes.\"" },
    { "ada", "Ada Merrow, 1891", "The Drowned Merrow: the pulpit of the sunken church",
      "\"Nobody plugged the Gate in this valley. It opened by itself in the spring of '88, and the water came up through the well for three days. "
      "We rang the bell until the tower went under. Elias says there are other valleys, other Merrows, and that the water has to go somewhere. "
      "I am leaving this for whichever of us comes through next. It is always one of us. Find the room where the water goes.\"" },
    { "dusk", "Unsigned, undated", "The Other Merrow: a lectern beside the Gate",
      "\"Here the Gate was never closed, so the sun never finished setting. I have walked through four valleys now. Every one is Merrow Lake, "
      "and every one has a Gate at the bottom, and they all drain to the same place. Carry the three pages to any open Gate. "
      "It will show you the way down. " DASH " E.\"" },
};

const char *STILL_ROOM_PAGE =
    "\"So you found the bottom of every lake.\n\n"
    "Every Merrow Lake in every valley drains down to this one room. Somebody has to sit here and hold the plug from underneath, "
    "so that each valley up there gets to keep its lake, or lose it, as it chooses. I held it for seventy-three years. "
    "The gravestone at home says I held it shut so they would not have to. That part is true.\n\n"
    "The chair is yours if you want it. If you don't, go home. The door knows the way.\n\n"
    DASH " Elias Merrow, Keeper of the Plug\"";

const struct house_def HOUSES[NHOUSES] = {
    { "The bakery", 6000, "Bread tins rusted into one lump, and a till drawer swollen shut. You pry it open: silver dollars, black with silt." },
    { "The schoolhouse", 9000, "Slates still on the desks. On the blackboard, in chalk that somehow survived: \"The water is coming up the well. Go home.\"" },
    { "The post office", 14000, "Sacks of letters that never went out. One bundle of postal orders is still good, if you know a collector." },
    { "The smithy", 22000, "The anvil is right where it was. Under it, wrapped in oilcloth: the smith's savings, and a horseshoe nailed up for luck." },
    { "The Merrow house", 40000, "Ada's house. Her coat is still on the hook. On the kitchen table, a map of the valley with the church circled, three times." },
    { "The inn", 30000, "The bar's last tab, chalked on the wall: \"Elias M. " DASH " owes for one lantern, one rope, and a lot of nerve.\"" },
};
const char *DROWNED_CHURCH = "The sunken church";
const double DROWNED_CHURCH_CASH = 60000;
const char *PLUG_NAME = "The Plug";
const char *PLUG_DESC = "An oak bung banded in iron, wide as a wagon wheel, with a chain as thick as your wrist.";

int item_index(const char *id) { for (int i = 0; i < NITEMS; i++) if (!strcmp(ITEMS[i].id, id)) return i; return -1; }
int building_index(const char *id) { for (int i = 0; i < NBUILD; i++) if (!strcmp(BUILDINGS[i].id, id)) return i; return -1; }

/* ---- number formatting ---- */
static void with_commas(long v, char *out) {
    char tmp[32]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    int neg = v < 0; if (neg) v = -v;
    int k = 0;
    while (v > 0) { if (k && k % 3 == 0) tmp[n++] = ','; tmp[n++] = (char)('0' + v % 10); v /= 10; k++; }
    int o = 0; if (neg) out[o++] = '-';
    while (n > 0) out[o++] = tmp[--n];
    out[o] = 0;
}
/* fixed decimals */
static void fixed(double v, int dec, char *out) {
    long scale = dec == 2 ? 100 : dec == 1 ? 10 : 1;
    long iv = (long)(v * scale + 0.5);
    if (dec == 0) ksnprintf(out, 24, "%ld", iv);
    else if (dec == 1) ksnprintf(out, 24, "%ld.%ld", iv / 10, iv % 10);
    else ksnprintf(out, 24, "%ld.%02ld", iv / 100, iv % 100);
}
void fmt_num(double n, char *out) {
    static const char *units[] = { "", "k", "M", "B", "T", "Qa" };
    if (n != n) { out[0] = '-'; out[1] = 0; return; }
    if (n < 0) { out[0] = '-'; fmt_num(-n, out + 1); return; }
    if (n < 1000000) {
        if (n < 10 && n != m_floord(n)) { fixed(n, 1, out); return; }
        with_commas((long)m_floord(n), out); return;
    }
    int i = 0;
    while (n >= 1000 && i < 5) { n /= 1000; i++; }
    char t[24]; fixed(n, n < 10 ? 2 : n < 100 ? 1 : 0, t);
    ksnprintf(out, 24, "%s%s", t, units[i]);
}
void fmt_money(double n, char *out) {
    out[0] = '$';
    if (n < 100 && n >= 0) fixed(n, 2, out + 1); else fmt_num(n, out + 1);
}
