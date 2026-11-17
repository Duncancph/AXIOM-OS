# The Vertex logo: a dark disc; a cream V whose two strokes meet at a bright point (a vertex).
import os, sys
from PIL import Image, ImageDraw, ImageFont
T = sys.argv[1]   # chrome/app/theme
BG, RING, CREAM, POINT = (20, 20, 19, 255), (58, 57, 52, 255), (244, 241, 222, 255), (233, 180, 76, 255)

def logo(n, mono=None):
    s = 8                                   # supersampling
    N = n * s
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if not mono:
        d.ellipse([0, 0, N - 1, N - 1], fill=BG)
        w = max(s, N // 40)
        d.ellipse([w // 2, w // 2, N - 1 - w // 2, N - 1 - w // 2], outline=RING, width=w)
    col = mono or CREAM
    top_y, bot_y = 0.27 * N, 0.74 * N
    lx, rx, cx = 0.27 * N, 0.73 * N, 0.5 * N
    sw = 0.105 * N
    d.line([(lx, top_y), (cx, bot_y)], fill=col, width=int(sw))
    d.line([(rx, top_y), (cx, bot_y)], fill=col, width=int(sw))
    r = sw / 2
    for x in (lx, rx):                      # round tops
        d.ellipse([x - r, top_y - r, x + r, top_y + r], fill=col)
    pr = 0.095 * N                          # the vertex
    d.ellipse([cx - pr, bot_y - pr, cx + pr, bot_y + pr], fill=mono or POINT)
    return im.resize((n, n), Image.LANCZOS)

def save(rel, n, mode):
    p = os.path.join(T, rel)
    im = logo(n)
    if mode == 'P':
        im = im.convert('RGBA')             # (palette files: keep RGBA, Chromium reads either)
    im.save(p)

for rel in ['chromium/product_logo_24.png', 'chromium/product_logo_48.png', 'chromium/product_logo_16.png',
            'chromium/product_logo_128.png', 'chromium/product_logo_256.png', 'chromium/product_logo_64.png',
            'default_100_percent/chromium/product_logo_16.png', 'default_100_percent/chromium/product_logo_32.png',
            'default_100_percent/chromium/linux/product_logo_16.png', 'default_100_percent/chromium/linux/product_logo_32.png',
            'chromium/linux/product_logo_24.png', 'chromium/linux/product_logo_48.png', 'chromium/linux/product_logo_128.png',
            'chromium/linux/product_logo_256.png', 'chromium/linux/product_logo_64.png',
            'default_200_percent/chromium/product_logo_16.png', 'default_200_percent/chromium/product_logo_32.png']:
    n = Image.open(os.path.join(T, rel)).size[0]
    save(rel, n, 'RGBA')
logo(22, mono=(0, 0, 0, 255)).save(os.path.join(T, 'chromium/product_logo_22_mono.png'))

# the name, next to the logo
def name_img(h, white):
    w = int(h * 97 / 22)
    s = 4
    im = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    im.paste(logo(h * s), (0, 0))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', int(h * s * 0.72))
    col = (255, 255, 255, 255) if white else (32, 33, 36, 255)
    d.text((int(h * s * 1.25), int(h * s * 0.5)), 'Vertex', font=f, fill=col, anchor='lm')
    return im.resize((w, h), Image.LANCZOS)
for scale, h in (('default_100_percent', 22), ('default_200_percent', 44)):
    name_img(h, False).save(os.path.join(T, scale, 'chromium/product_logo_name_22.png'))
    name_img(h, True).save(os.path.join(T, scale, 'chromium/product_logo_name_22_white.png'))

# the X11 window icon (XPM)
im = logo(32)
cols = {}
rows = []
for y in range(32):
    row = ''
    for x in range(32):
        r, g, b, a = im.getpixel((x, y))
        key = None if a < 128 else '#%02X%02X%02X' % (r & 0xF0, g & 0xF0, b & 0xF0)
        if key not in cols:
            cols[key] = chr(35 + len(cols)) if key else ' '
            if cols[key] in '"\\': cols[key] = chr(200 + len(cols))
        row += cols[key]
    rows.append(row)
xpm = ['/* XPM */', 'static char * linux_icon_chromium_32_xpm[] = {', '"32 32 %d 1",' % len(cols)]
for k, c in cols.items():
    xpm.append('"%s\tc %s",' % (c, k or 'None'))
xpm += ['"%s",' % r for r in rows[:-1]] + ['"%s"};' % rows[-1]]
open(os.path.join(T, 'chromium/linux/product_logo_32.xpm'), 'w', encoding='latin-1').write('\n'.join(xpm) + '\n')

svg = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <circle cx="128" cy="128" r="127" fill="#141413"/>
  <circle cx="128" cy="128" r="124" fill="none" stroke="#3A3934" stroke-width="6"/>
  <path d="M69 69 L128 189 L187 69" fill="none" stroke="#F4F1DE" stroke-width="27" stroke-linecap="round" stroke-linejoin="round"/>
  <circle cx="128" cy="189" r="24" fill="#E9B44C"/>
</svg>
'''
open(os.path.join(T, 'chromium/product_logo.svg'), 'w').write(svg)

print('done')
