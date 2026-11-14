# Regenerates src/font8x16.h (printable ASCII 32..126) from GNU Unifont's unifont.hex,
# in the layout of the DuncOS font8x16.h.
import sys
hexfile = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/unifont/unifont.hex"
glyphs = {}
for line in open(hexfile):
    cp, data = line.strip().split(":")
    cp = int(cp, 16)
    if 32 <= cp <= 126: glyphs[cp] = [int(data[i:i+2], 16) for i in range(0, 32, 2)]
def name(c):
    ch = chr(c)
    if ch == "\\": return "'backslash'"
    if ch == "'": return '"\'"'
    return "'%s'" % ch
out = ["/* font8x16.h - 8x16 bitmap font, printable ASCII 32..126.",
       " * Generated from GNU Unifont (GPLv2+ with font embedding exception / OFL). */",
       "#ifndef FONT8X16_H", "#define FONT8X16_H", "#include <stdint.h>",
       "#define FONT_W 8", "#define FONT_H 16", "#define FONT_FIRST 32", "#define FONT_LAST 126",
       "static const uint8_t font8x16[FONT_LAST - FONT_FIRST + 1][FONT_H] = {"]
for c in range(32, 127):
    out.append("    { " + ", ".join("0x%02X" % b for b in glyphs[c]) + " }, /* %s */" % name(c))
out += ["};", "#endif"]
open(sys.argv[2] if len(sys.argv) > 2 else "src/font8x16.h", "w").write("\n".join(out) + "\n")
