#!/usr/bin/env python3
"""Run the AXIOM JavaScript engine test corpus against node.

Every tests/js/cases/*.js file is executed by both `node` and `jsrun`; their stdout must be
identical (stderr is ignored, exit codes must agree). tests/js/perf/*.js print `RESULT ...`
lines (compared) and `TIME name ms` lines (tabulated against node).

usage: run.py [--jsrun PATH] [--node PATH] [--perf] [--no-perf] [-v] [filter...]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
NODE_WRAPPER = os.path.join(HERE, "node_script.js")


def run(cmd, timeout):
    env = dict(os.environ, TZ="UTC", LANG="C")
    t0 = time.time()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, env=env)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace"), time.time() - t0
    except subprocess.TimeoutExpired:
        return -999, "", "TIMEOUT", time.time() - t0


def first_diff(a, b):
    al, bl = a.splitlines(), b.splitlines()
    for i in range(max(len(al), len(bl))):
        x = al[i] if i < len(al) else "<missing>"
        y = bl[i] if i < len(bl) else "<missing>"
        if x != y:
            return i + 1, x, y
    return 0, "", ""


def main():
    args = sys.argv[1:]
    jsrun = os.path.join(ROOT, "build", "test", "jsrun")
    node = "node"
    verbose = False
    perf = True
    filters = []
    while args:
        a = args.pop(0)
        if a == "--jsrun":
            jsrun = args.pop(0)
        elif a == "--node":
            node = args.pop(0)
        elif a == "-v":
            verbose = True
        elif a == "--no-perf":
            perf = False
        elif a == "--perf":
            perf = True
            filters.append("\0perfonly")
        else:
            filters.append(a)
    perf_only = "\0perfonly" in filters
    filters = [f for f in filters if f != "\0perfonly"]

    cases_dir = os.path.join(HERE, "cases")
    files = sorted(f for f in os.listdir(cases_dir) if f.endswith(".js"))
    if filters:
        files = [f for f in files if any(x in f for x in filters)]
    passed = failed = checks = 0
    failures = []
    if not perf_only:
        for f in files:
            path = os.path.join(cases_dir, f)
            nrc, nout, nerr, _ = run([node, NODE_WRAPPER, path], 60)
            jrc, jout, jerr, jt = run([jsrun, path], 60)
            nl = len(nout.splitlines())
            if nout == jout and (nrc == 0) == (jrc == 0):
                passed += 1
                checks += nl
                if verbose:
                    print("ok   %-34s %4d lines %6.2fs" % (f, nl, jt))
            else:
                failed += 1
                line, x, y = first_diff(nout, jout)
                failures.append(f)
                print("FAIL %s (exit node=%d jsrun=%d)" % (f, nrc, jrc))
                if line:
                    print("     line %d\n       node : %s\n       jsrun: %s" % (line, x[:300], y[:300]))
                if jerr.strip():
                    print("     jsrun stderr: " + jerr.strip().splitlines()[0][:300])
        print("\n%d/%d case files passed, %d output lines (checks) matched" % (passed, passed + failed, checks))
        if failures:
            print("failing: " + " ".join(failures))

    if perf:
        pdir = os.path.join(HERE, "perf")
        pfiles = sorted(f for f in os.listdir(pdir) if f.endswith(".js")) if os.path.isdir(pdir) else []
        if filters and not perf_only:
            pfiles = []
        if pfiles:
            print("\nperformance (ms)            jsrun      node    ratio")
        for f in pfiles:
            path = os.path.join(pdir, f)
            nrc, nout, _, _ = run([node, NODE_WRAPPER, path], 300)
            jrc, jout, jerr, _ = run([jsrun, path], 300)

            def parse(out):
                res, times = [], {}
                for l in out.splitlines():
                    if l.startswith("RESULT"):
                        res.append(l)
                    elif l.startswith("TIME "):
                        _, name, ms = l.split()
                        times[name] = float(ms)
                return res, times

            nres, ntimes = parse(nout)
            jres, jtimes = parse(jout)
            ok = nres == jres and jrc == 0
            for name in ntimes:
                jt = jtimes.get(name)
                nt = ntimes[name]
                if jt is None:
                    print("  %-24s %9s %9.1f" % (name, "-", nt))
                else:
                    print("  %-24s %9.1f %9.1f %8.1fx" % (name, jt, nt, jt / max(nt, 0.05)))
            if not ok:
                failed += 1
                print("  PERF RESULT MISMATCH in %s" % f)
                if jerr.strip():
                    print("  jsrun stderr: " + jerr.strip().splitlines()[0][:300])
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
