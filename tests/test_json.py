# Checks user/lib/json.c against Python's json module: lookups, string decoding (incl. surrogates), escaping round trips.
import json, random, subprocess, sys
R = random.Random(3)
def rstr():
    pool = ["a", "Z", " ", '"', "\\", "/", "\n", "\t", "\r", "\x01", "é", "€", "ü", "日", "😀", " ", "{", "}", "[", "]", ":", ","]
    return "".join(R.choice(pool) for _ in range(R.randrange(0, 30)))
def rval(d=0):
    k = R.randrange(6 if d < 3 else 3)
    if k == 0: return rstr()
    if k == 1: return R.randrange(-10**6, 10**6)
    if k == 2: return True
    if k == 3: return {(rstr()[:5] or "k"): rval(d + 1) for _ in range(R.randrange(4))}
    if k == 4: return [rval(d + 1) for _ in range(R.randrange(4))]
    return None
cases = []
for i in range(3000):
    inner = rstr(); obj = {"type": "x", "pad": rval(), "delta": {"other": rval(), "text": inner}, "tail": rval()}
    js = json.dumps(obj, ensure_ascii=R.random() < 0.5, indent=R.choice([None, 1]))
    cases.append((js, inner))
p = subprocess.run([sys.argv[1]], input="\n".join(json.dumps([c[0], c[1]]) for c in cases).encode(), capture_output=True)
out = p.stdout.decode("utf-8", "surrogatepass").split("\n")
bad = 0
for (js, want), got in zip(cases, out):
    try: ok = json.loads(got) == want
    except Exception: ok = False
    if not ok:
        bad += 1
        if bad < 5: print("FAIL", repr(js)[:120], "\n got ", got[:80], "\n want", json.dumps(want)[:80])
print("json: %d documents, %d failures" % (len(cases), bad)); sys.exit(1 if bad else 0)
