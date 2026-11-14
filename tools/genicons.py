# Generates the AXIOM app icons: smooth (anti-aliased) dark graphite tiles with a cream symbol
# (the AXIOM palette), in the dock size (52 px) and the start menu size (34 px).
# Output: src/icons_data.h (ARGB pixels) and, with --preview FILE, a PNG preview sheet.
import math, sys
from PIL import Image, ImageDraw, ImageFilter

SS = 8                      # supersampling
SIZES = {"dock": 52, "menu": 34}
RADIUS = 14 / 52            # corner radius as a fraction of the size (matches the Highland dock tiles)

def hexc(h, a=255): h = h.lstrip("#"); return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)

def tile(S, top, bottom):
    """rounded square with a vertical gradient and a soft top highlight"""
    grad = Image.new("RGBA", (S, S))
    t, b = hexc(top), hexc(bottom)
    for y in range(S):
        f = y / (S - 1)
        grad.paste(tuple(int(t[i] + (b[i] - t[i]) * f) for i in range(3)) + (255,), (0, y, S, y + 1))
    hl = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    hd = ImageDraw.Draw(hl)
    for y in range(S // 2):
        hd.line([(0, y), (S, y)], fill=(255, 255, 255, int(38 * (1 - y / (S / 2)))))
    grad = Image.alpha_composite(grad, hl)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * RADIUS), fill=255)
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    # a thin lighter rim along the top edge, like glass
    rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([1, 1, S - 2, S - 2], radius=int(S * RADIUS) - 1, outline=(244, 241, 222, 34), width=max(1, S // 52))
    rim_mask = Image.new("L", (S, S), 0); ImageDraw.Draw(rim_mask).rectangle([0, 0, S, S // 2], fill=255)
    out.paste(rim, (0, 0), Image.composite(rim, Image.new("RGBA", (S, S)), rim_mask).split()[3])
    return out

def with_shadow(base, glyph, S):
    """put the white glyph layer on the tile with a soft drop shadow"""
    a = glyph.split()[3]
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: v * 70 // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(S / 70))
    shifted = Image.new("RGBA", (S, S), (0, 0, 0, 0)); shifted.paste(sh, (0, S // 60))
    out = Image.alpha_composite(base, shifted)
    return Image.alpha_composite(out, glyph)

W = (255, 255, 255, 255)
CREAM = (244, 241, 222, 255)
def layer_alpha_from(im): return im.split()[3]

def g_folder(d, S, u):
    d.rounded_rectangle([u(.18), u(.27), u(.48), u(.40)], radius=u(.04), fill=(255, 255, 255, 190))
    d.rounded_rectangle([u(.18), u(.32), u(.82), u(.74)], radius=u(.06), fill=(255, 255, 255, 190))
    d.rounded_rectangle([u(.18), u(.38), u(.82), u(.76)], radius=u(.06), fill=W)

def g_terminal(d, S, u):
    w = u(.075)
    d.line([(u(.26), u(.34)), (u(.42), u(.50)), (u(.26), u(.66))], fill=(134, 239, 172, 255), width=w, joint="curve")
    for x, y in ((u(.26), u(.34)), (u(.42), u(.50)), (u(.26), u(.66))): d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=(134, 239, 172, 255))
    d.rounded_rectangle([u(.48), u(.62), u(.74), u(.70)], radius=u(.04), fill=W)

def g_monitor(d, S, u):
    for i, h in enumerate((.20, .34, .26, .42)):
        x = u(.22 + i * .15)
        d.rounded_rectangle([x, u(.76 - h), x + u(.09), u(.76)], radius=u(.03), fill=(255, 255, 255, 110))
    pts = [(u(.18), u(.56)), (u(.34), u(.56)), (u(.42), u(.32)), (u(.52), u(.70)), (u(.60), u(.46)), (u(.66), u(.56)), (u(.82), u(.56))]
    d.line(pts, fill=W, width=u(.065), joint="curve")
    for p in (pts[0], pts[-1]): d.ellipse([p[0] - u(.033), p[1] - u(.033), p[0] + u(.033), p[1] + u(.033)], fill=W)

def g_bluetooth(d, S, u):
    pts = [(u(.33), u(.36)), (u(.66), u(.64)), (u(.50), u(.79)), (u(.50), u(.21)), (u(.66), u(.36)), (u(.33), u(.64))]
    d.line(pts, fill=W, width=u(.065), joint="curve")
    for p in (pts[0], pts[-1]): d.ellipse([p[0] - u(.033), p[1] - u(.033), p[0] + u(.033), p[1] + u(.033)], fill=W)

def g_network(d, S, u):
    w = u(.055)
    d.line([(u(.50), u(.32)), (u(.50), u(.50))], fill=W, width=w)
    d.line([(u(.28), u(.66)), (u(.28), u(.50)), (u(.72), u(.50)), (u(.72), u(.66))], fill=W, width=w, joint="curve")
    d.rounded_rectangle([u(.38), u(.18), u(.62), u(.38)], radius=u(.05), fill=W)
    for x in (.28, .72):
        d.rounded_rectangle([u(x - .12), u(.60), u(x + .12), u(.80)], radius=u(.05), fill=W)

def g_calc(d, S, u):
    d.rounded_rectangle([u(.22), u(.18), u(.78), u(.34)], radius=u(.04), fill=(255, 255, 255, 230))
    for r in range(3):
        for c in range(3):
            x, y = u(.22 + c * .195), u(.42 + r * .135)
            col = (251, 146, 60, 255) if c == 2 else (255, 255, 255, 200)
            d.rounded_rectangle([x, y, x + u(.165), y + u(.105)], radius=u(.03), fill=col)

def g_shield(d, S, u):
    pts = [(.50, .16), (.78, .27), (.76, .52), (.68, .68), (.50, .82), (.32, .68), (.24, .52), (.22, .27)]
    d.polygon([(u(x), u(y)) for x, y in pts], fill=W)
    d.rounded_rectangle([u(.465), u(.32), u(.535), u(.56)], radius=u(.035), fill=(220, 38, 38, 255))
    d.ellipse([u(.465), u(.61), u(.535), u(.68)], fill=(220, 38, 38, 255))

def g_gear(d, S, u):
    cx = cy = u(.5)
    for k in range(8):
        a = k * math.pi / 4
        tooth = Image.new("L", (S, S), 0)
        ImageDraw.Draw(tooth).rounded_rectangle([cx - u(.075), cy - u(.34), cx + u(.075), cy - u(.18)], radius=u(.03), fill=255)
        tooth = tooth.rotate(math.degrees(a), resample=Image.BICUBIC, center=(cx, cy))
        d.bitmap((0, 0), tooth, fill=W)
    d.ellipse([cx - u(.25), cy - u(.25), cx + u(.25), cy + u(.25)], fill=W)

def g_gear_hole(img, S, u, col):
    ImageDraw.Draw(img).ellipse([u(.5) - u(.10), u(.5) - u(.10), u(.5) + u(.10), u(.5) + u(.10)], fill=col)

def g_photo(d, S, u):
    d.rounded_rectangle([u(.18), u(.22), u(.82), u(.78)], radius=u(.08), outline=W, width=u(.055))
    d.ellipse([u(.58), u(.32), u(.70), u(.44)], fill=W)
    d.polygon([(u(.24), u(.72)), (u(.42), u(.46)), (u(.56), u(.64)), (u(.64), u(.56)), (u(.76), u(.72))], fill=W)

def g_axiom(d, S, u):
    import json
    L = json.load(open("tools/axiom_logo_raw.json"))
    mk = [sh for sh in L["shapes"] if max(p[0] for p in sh["pts"]) < 130]
    mw = max(p[0] for sh in mk for p in sh["pts"]); k = 0.56 / max(mw, L["h"])
    ox, oy = (1 - mw * k) / 2, (1 - L["h"] * k) / 2 + .01
    for sh in mk: d.polygon([(u(ox + x * k), u(oy + y * k)) for x, y in sh["pts"]], fill=W)

def g_crown(d, S, u):
    gold = (251, 191, 36, 255)
    pts = [(.20, .70), (.20, .34), (.36, .50), (.50, .26), (.64, .50), (.80, .34), (.80, .70)]
    d.polygon([(u(x), u(y)) for x, y in pts], fill=gold)
    d.rounded_rectangle([u(.20), u(.72), u(.80), u(.80)], radius=u(.03), fill=gold)
    for x, y in ((.20, .34), (.50, .26), (.80, .34)): d.ellipse([u(x) - u(.045), u(y) - u(.045), u(x) + u(.045), u(y) + u(.045)], fill=gold)

def g_code(d, S, u):
    w = u(.07)
    d.line([(u(.34), u(.32)), (u(.18), u(.50)), (u(.34), u(.68))], fill=W, width=w, joint="curve")
    d.line([(u(.66), u(.32)), (u(.82), u(.50)), (u(.66), u(.68))], fill=W, width=w, joint="curve")
    d.line([(u(.56), u(.26)), (u(.44), u(.74))], fill=(255, 255, 255, 200), width=w)

def g_clock(d, S, u):
    d.ellipse([u(.18), u(.18), u(.82), u(.82)], fill=W)
    for k in range(12):
        a = k * math.pi / 6; r0, r1 = (.25, .29) if k % 3 else (.23, .29)
        d.line([(u(.5 + r0 * math.sin(a)), u(.5 - r0 * math.cos(a))), (u(.5 + r1 * math.sin(a)), u(.5 - r1 * math.cos(a)))], fill=(30, 41, 59, 255), width=u(.022))
    d.line([(u(.5), u(.5)), (u(.5), u(.30))], fill=(30, 41, 59, 255), width=u(.045))
    d.line([(u(.5), u(.5)), (u(.64), u(.58))], fill=(30, 41, 59, 255), width=u(.045))
    d.line([(u(.5), u(.5)), (u(.38), u(.68))], fill=(249, 115, 22, 255), width=u(.02))
    d.ellipse([u(.47), u(.47), u(.53), u(.53)], fill=(249, 115, 22, 255))

def g_globe(d, S, u):
    w = u(.045)
    d.ellipse([u(.20), u(.20), u(.80), u(.80)], outline=W, width=w)
    d.ellipse([u(.37), u(.20), u(.63), u(.80)], outline=W, width=w)
    d.line([(u(.50), u(.21)), (u(.50), u(.79))], fill=W, width=w)
    d.line([(u(.21), u(.50)), (u(.79), u(.50))], fill=W, width=w)
    d.arc([u(.24), u(.08), u(.76), u(.42)], 30, 150, fill=W, width=w)
    d.arc([u(.24), u(.58), u(.76), u(.92)], 210, 330, fill=W, width=w)

def g_chat(d, S, u):
    d.rounded_rectangle([u(.18), u(.22), u(.82), u(.70)], radius=u(.12), fill=W)
    d.polygon([(u(.30), u(.66)), (u(.30), u(.84)), (u(.48), u(.68))], fill=W)
    c = (0, 0, 0, 0); cx, cy, r = u(.50), u(.46), u(.15)             # a sparkle cut out of the bubble
    d.polygon([(cx, cy - r), (cx + r // 4, cy - r // 4), (cx + r, cy), (cx + r // 4, cy + r // 4),
               (cx, cy + r), (cx - r // 4, cy + r // 4), (cx - r, cy), (cx - r // 4, cy - r // 4)], fill=c)

ICONS = [   # name, gradient top, bottom, glyph
    ("folder", "#2C2C2C", "#141414", g_folder),
    ("terminal", "#2C2C2C", "#141414", g_terminal),
    ("monitor", "#2C2C2C", "#141414", g_monitor),
    ("bluetooth", "#2C2C2C", "#141414", g_bluetooth),
    ("network", "#2C2C2C", "#141414", g_network),
    ("calc", "#2C2C2C", "#141414", g_calc),
    ("shield", "#2C2C2C", "#141414", g_shield),
    ("gear", "#2C2C2C", "#141414", g_gear),
    ("photo", "#2C2C2C", "#141414", g_photo),
    ("crown", "#2C2C2C", "#141414", g_axiom),
    ("code", "#2C2C2C", "#141414", g_code),
    ("clock", "#2C2C2C", "#141414", g_clock),
    ("globe", "#2C2C2C", "#141414", g_globe),
    ("chat", "#2C2C2C", "#141414", g_chat),
]

def render(name, top, bottom, glyph, size):
    S = size * SS
    u = lambda f: int(round(f * S))
    base = tile(S, top, bottom)
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    glyph(ImageDraw.Draw(layer), S, u)
    if name == "gear": g_gear_hole(layer, S, u, (0, 0, 0, 0))
    # AXIOM palette: every symbol in cream; coloured details become fainter cream
    r_, g_, b_, a_ = layer.split()
    lum = Image.merge("RGB", (r_, g_, b_)).convert("L")
    a_ = Image.merge("LA", (lum, a_)).convert("RGBA")
    alpha = Image.eval(lum, lambda v: 110 + v * 145 // 255)
    from PIL import ImageChops
    layer = Image.new("RGBA", (S, S), CREAM); layer.putalpha(ImageChops.multiply(layer.split()[3].point(lambda v: 255), ImageChops.multiply(alpha, layer_alpha_from(a_))))
    out = with_shadow(base, layer, S)
    return out.resize((size, size), Image.LANCZOS)

def main():
    out = ["/* Generated by tools/genicons.py: AXIOM app icons, ARGB (straight alpha). */", "#pragma once", "#include <stdint.h>",
           "struct app_icon { const char *name; const uint32_t *dock, *menu; };"]
    imgs = {}
    for name, top, bot, glyph in ICONS:
        for kind, size in SIZES.items():
            im = render(name, top, bot, glyph, size); imgs[(name, kind)] = im
            px = list(im.getdata())
            out.append("static const uint32_t icon_%s_%s[%d] = {" % (name, kind, size * size))
            for i in range(0, len(px), 12):
                out.append("  " + ",".join("0x%02X%02X%02X%02X" % (a, r, g, b) for r, g, b, a in px[i:i + 12]) + ",")
            out.append("};")
    out.append("#define ICON_DOCK %d" % SIZES["dock"]); out.append("#define ICON_MENU %d" % SIZES["menu"])
    out.append("static const struct app_icon app_icons[] = {")
    for name, *_ in ICONS: out.append('  { "%s", icon_%s_dock, icon_%s_menu },' % (name, name, name))
    out.append("};")
    open("src/icons_data.h", "w").write("\n".join(out) + "\n")
    if len(sys.argv) > 2 and sys.argv[1] == "--preview":   # a sheet on the dock colour, 2x
        n = len(ICONS); size = SIZES["dock"]
        sheet = Image.new("RGBA", (n * (size + 12) + 12, size + 24), (14, 14, 14, 255))
        for i, (name, *_) in enumerate(ICONS): sheet.alpha_composite(imgs[(name, "dock")], (12 + i * (size + 12), 12))
        sheet.resize((sheet.width * 2, sheet.height * 2), Image.LANCZOS).save(sys.argv[2])

main()
