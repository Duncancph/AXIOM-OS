# Regenerates src/font8x16.h (character codes 32..255 in Windows-1252, the DuncOS text encoding) from GNU Unifont's unifont.hex,
# in the layout of the DuncOS font8x16.h.
import sys
hexfile = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/unifont/unifont.hex"
glyphs = {}
for line in open(hexfile):
    cp, data = line.strip().split(":")
    cp = int(cp, 16)
    if len(data) == 32: glyphs[cp] = [int(data[i:i+2], 16) for i in range(0, 32, 2)]
def uni(c):   # Windows-1252 byte -> Unicode code point ('?' for the five unused codes, space for DEL/NBSP)
    try: u = ord(bytes([c]).decode("cp1252"))
    except UnicodeDecodeError: u = ord("?")
    return 32 if c in (127, 160) else u
def name(c):
    ch = chr(uni(c))
    if ch == "\\": return "'backslash'"
    if ch == "'": return '"\'"'
    return "'%s'" % ch
out = ["/* font8x16.h - 8x16 bitmap font, character codes 32..255 (Windows-1252).",
       " * Generated from GNU Unifont (GPLv2+ with font embedding exception / OFL). */",
       "#ifndef FONT8X16_H", "#define FONT8X16_H", "#include <stdint.h>",
       "#define FONT_W 8", "#define FONT_H 16", "#define FONT_FIRST 32", "#define FONT_LAST 255",
       "static const uint8_t font8x16[FONT_LAST - FONT_FIRST + 1][FONT_H] = {"]
for c in range(32, 256):
    g = glyphs.get(uni(c), glyphs[ord("?")])
    out.append("    { " + ", ".join("0x%02X" % b for b in g) + " }, /* %d %s */" % (c, name(c).replace("*/", "* /")))
out += ["};", "#endif"]
open(sys.argv[2] if len(sys.argv) > 2 else "src/font8x16.h", "w").write("\n".join(out) + "\n")
