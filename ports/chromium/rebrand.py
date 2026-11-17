# Rebrand Chromium's product strings as Vertex, keeping the translations: the .xtb files key each translation
# by a fingerprint of the English text, so each message's id is worked out before and after (with grit itself)
# and the translations are moved to the new ids, with the name changed in them too.
import os, re, sys
SRC = sys.argv[1]
sys.path.insert(0, os.path.join(SRC, 'tools/grit'))
from grit import grd_reader

KEEP = re.compile(r'Chromium (Authors|OS|project)|ChromiumOS')
def rename(text):
    out, i = [], 0
    for m in re.finditer(r'Chromium', text):
        if KEEP.match(text, m.start()) or text[m.start():m.start()+10] == 'ChromiumOS':
            continue
        out.append(text[i:m.start()]); out.append('Vertex'); i = m.end()
    out.append(text[i:])
    return ''.join(out)

def parse(path):
    defs = {}
    for _ in range(200):
        try:
            return grd_reader.Parse(path, defines=defs, target_platform='linux')
        except AssertionError as e:
            m = re.search(r'undefined Grit variable found: (\S+)', str(e))
            if not m: raise
            defs[m.group(1)] = False
    raise RuntimeError('too many defines')

def ids(res):
    return [node.GetCliques()[0].GetMessage().GetId() for node in res.Preorder() if node.name == 'message']

def rebrand_grd(rel):
    path = os.path.join(SRC, rel)
    text = open(path, encoding='utf-8').read()
    old_ids = ids(parse(path))
    # the .grd and the parts it includes (.grdp)
    files = [path] + [os.path.join(os.path.dirname(path), f) for f in re.findall(r'<part file="([^"]+)"', text)]
    for f in files:
        t = open(f, encoding='utf-8').read()
        t = re.sub(r'(<message\b(?:[^>"]|"[^"]*")*>)(.*?)(</message>)', lambda m: m.group(1) + rename(m.group(2)) + m.group(3), t, flags=re.S)
        open(f, 'w', encoding='utf-8').write(t)
    new_ids = ids(parse(path))
    assert len(old_ids) == len(new_ids)
    remap = {}
    for o, n in zip(old_ids, new_ids):
        if o != n:
            assert remap.get(o, n) == n, 'one English text became two'
            remap[o] = n
    xtbs = re.findall(r'<file path="([^"]+\.xtb)"', text)
    moved = 0
    for x in xtbs:
        xp = os.path.join(os.path.dirname(path), x)
        if not os.path.exists(xp): continue
        xt = open(xp, encoding='utf-8').read()
        def fix(m):
            nonlocal moved
            tid, body = m.group(1), m.group(2)
            if tid in remap:
                moved += 1
                return '<translation id="%s">%s</translation>' % (remap[tid], rename(body))
            return m.group(0)
        xt = re.sub(r'<translation id="(\d+)">(.*?)</translation>', fix, xt, flags=re.S)
        open(xp, 'w', encoding='utf-8').write(xt)
    print(f'{rel}: {len(remap)} messages renamed, {moved} translations moved, {len(xtbs)} languages, {len(files)} files')

for g in ['chrome/app/chromium_strings.grd', 'components/components_chromium_strings.grd']:
    rebrand_grd(g)

b = os.path.join(SRC, 'chrome/app/theme/chromium/BRANDING')
t = open(b).read()
for k, v in [('PRODUCT_FULLNAME', 'Vertex'), ('PRODUCT_SHORTNAME', 'Vertex'),
             ('PRODUCT_INSTALLER_FULLNAME', 'Vertex Installer'), ('PRODUCT_INSTALLER_SHORTNAME', 'Vertex Installer')]:
    t = re.sub(r'^%s=.*$' % k, '%s=%s' % (k, v), t, flags=re.M)
open(b, 'w').write(t)
print(t)
