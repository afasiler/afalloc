#!/usr/bin/env python3
"""Run bench.c once per allocator (afalloc2 once per security level) and print merged tables.
usage: python3 bench/run_bench.py [quick]"""
import os, subprocess, sys, collections

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
quick = "quick" in sys.argv
exe = os.path.join(root, "build_bench")
old_obj = os.path.join(root, "old_afalloc.o")
subprocess.run(["cc", "-std=gnu11", "-O2", "-c", os.path.join(root, "..", "mmap", "afalloc_persistent.c"), "-Dafalloc=old_afalloc",
                "-Dafree=old_afree", "-o", old_obj], check=True)       # the old allocator keeps its code, only its names change
subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-pthread", os.path.join(here, "bench.c"),
                os.path.join(root, "afalloc.c"), old_obj, "-o", exe], check=True)

runs = [("sys", {}), ("old", {}), ("af", {"AFALLOC_SECURE": "0"}), ("af", {"AFALLOC_SECURE": "1"}), ("af", {"AFALLOC_SECURE": "2"})]
data = collections.OrderedDict()
names = []
for which, env in runs:
    cmd = ["nice", "-n", "10", exe, which] + (["quick"] if quick else [])
    out = subprocess.run(cmd, env=dict(os.environ, **env), capture_output=True, text=True, timeout=900)
    if out.returncode != 0:
        print("bench failed:", which, env, out.stderr[-500:]); sys.exit(1)
    for line in out.stdout.splitlines():
        metric, name, val = line.split(",")
        if name not in names: names.append(name)
        data.setdefault(metric, {})[name] = float(val)

w = 28
print(f"{'metric':<{w}}" + "".join(f"{n:>22}" for n in names))
for metric, row in data.items():
    print(f"{metric:<{w}}" + "".join(f"{row[n]:>22.2f}" if n in row else f"{'-':>22}" for n in names))
