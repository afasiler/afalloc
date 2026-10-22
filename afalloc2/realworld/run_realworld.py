#!/usr/bin/env python3
"""Run the real-world proxies (SQLite, game loop, ray tracer, AI) with the system allocator and with afalloc at
security levels 0, 1 and 2. Runs are interleaved (sys, L0, L1, L2, sys, ...) so slow drift of the machine hits all
variants equally; each cell is the median of REPEATS runs. Checksums must agree across variants.
usage: python3 realworld/run_realworld.py [repeats]"""
import os, statistics, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
build = os.path.join(root, "build_rw")
os.makedirs(build, exist_ok=True)
REPEATS = int(sys.argv[1]) if len(sys.argv) > 1 else 5

def sh(cmd):
    subprocess.run(cmd, check=True)

af_obj = os.path.join(build, "afalloc.o")
sh(["cc", "-std=gnu11", "-O2", "-pthread", "-c", os.path.join(root, "afalloc.c"), "-o", af_obj])
sh(["cc", "-std=gnu11", "-O2", os.path.join(here, "sqlite_bench.c"), af_obj, "-lsqlite3", "-pthread", "-o", os.path.join(build, "sqlite_bench")])
progs = ["game_bench", "render_bench", "nn_bench"]
for p in progs:
    src = os.path.join(here, p + ".cpp")
    sh(["clang++", "-std=c++17", "-O2", src, "-o", os.path.join(build, p + ".sys")])
    sh(["clang++", "-std=c++17", "-O2", "-DUSE_AFALLOC", src, af_obj, "-pthread", "-o", os.path.join(build, p + ".af")])
    sh(["clang++", "-std=c++17", "-O2", "-DCOUNT_ALLOCS", src, "-o", os.path.join(build, p + ".count")])

def run(cmd, env_extra=None):
    out = subprocess.run(["nice", "-n", "10"] + cmd, capture_output=True, text=True, env=dict(os.environ, **(env_extra or {})), timeout=300)
    if out.returncode != 0:
        print("FAILED:", cmd, out.stderr[-400:]); sys.exit(1)
    d = {}
    for line in out.stdout.splitlines():
        k, v = line.split(",")
        d[k] = float(v)
    return d

# variant name -> command builder
def variants(prog):
    if prog == "sqlite":
        exe = os.path.join(build, "sqlite_bench")
        return [("sys (default)", [exe, "default"], {}), ("sys (hook)", [exe, "sys"], {}),
                ("afalloc L0", [exe, "af"], {"AFALLOC_SECURE": "0"}), ("afalloc L1", [exe, "af"], {"AFALLOC_SECURE": "1"}),
                ("afalloc L2", [exe, "af"], {"AFALLOC_SECURE": "2"})]
    return [("system", [os.path.join(build, prog + ".sys")], {}),
            ("afalloc L0", [os.path.join(build, prog + ".af")], {"AFALLOC_SECURE": "0"}),
            ("afalloc L1", [os.path.join(build, prog + ".af")], {"AFALLOC_SECURE": "1"}),
            ("afalloc L2", [os.path.join(build, prog + ".af")], {"AFALLOC_SECURE": "2"})]

report = []
for prog in ["sqlite"] + progs:
    vs = variants(prog)
    samples = {name: [] for name, _, _ in vs}
    for rep in range(REPEATS):
        for name, cmd, env in vs:
            samples[name].append(run(cmd, env))
    counts = run([os.path.join(build, "sqlite_bench"), "count"] if prog == "sqlite" else [os.path.join(build, prog + ".count")])
    # checksum agreement
    sums = {s["checksum"] for v in samples.values() for s in v}
    metrics = [k for k in samples[vs[0][0]][0] if k != "checksum"]
    report.append((prog, vs, samples, metrics, counts, len(sums) == 1))

names_w = 28
for prog, vs, samples, metrics, counts, agree in report:
    print(f"\n=== {prog}   (median of {REPEATS} runs; checksums {'agree' if agree else 'DIFFER'}; "
          f"{counts.get('alloc_calls', 0) / 1e6:.2f} million allocations, {counts.get('alloc_bytes_MB', 0):.0f} MB requested)")
    print(f"{'metric':<{names_w}}" + "".join(f"{n:>16}" for n, _, _ in vs) + f"{'L1 vs system':>16}")
    for m in metrics:
        med = {n: statistics.median(s[m] for s in samples[n]) for n, _, _ in vs}
        base = med[vs[1][0]] if prog == "sqlite" else med[vs[0][0]]                # the like-for-like baseline
        l1 = med["afalloc L1"]
        change = (l1 / base - 1.0) * 100 if base else 0
        print(f"{m:<{names_w}}" + "".join(f"{med[n]:>16.2f}" for n, _, _ in vs) + f"{change:>+15.1f}%")
