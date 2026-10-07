#!/usr/bin/env python3
"""Large-block benchmark across chunk/region sizes (1 MiB, 4 MiB, 16 MiB, 1 GiB).

Builds bench/bench.c once per allocator and per -DAFA_SIZE, runs its `big` suite
(requests of 1 MiB, 4 MiB, 16 MiB, 1 GiB and the largest block the chunk holds)
and prints one table per chunk size. malloc does not depend on AFA_SIZE and is
run once.
"""
import collections, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
CC = os.environ.get("CC", "cc")
CFLAGS = ["-O2", "-Wall", "-Wextra", "-Wno-unused-function"]
# Apple clang outlines pieces of the hot path into calls; turn that off when the
# compiler knows the flag so the numbers measure the allocator, not the outliner.
if subprocess.run([CC, "-mno-outline", "-fsyntax-only", "-x", "c", "/dev/null"],
                  capture_output=True).returncode == 0:
    CFLAGS.append("-mno-outline")

BACKENDS = [("persist", "BE_PERSIST", "mmap/afalloc_persistent.c"),
            ("arena", "BE_ARENA", "arena_allocator/rv1103.c"),
            ("arenaf", "BE_ARENAF", "arena_allocator/arena_malloc.c"),
            ("gp", "BE_GP", "mmap/mmap_allocator.c")]
LABEL = {"malloc": "malloc/free", "persist": "persist", "arena": "arena (rv1103)",
         "arenaf": "arena_malloc", "gp": "gp"}
SIZES = [("1 MiB", 1 << 20), ("4 MiB", 4 << 20), ("16 MiB", 16 << 20), ("1 GiB", 1 << 30), ("2 GiB", 2 << 30)]
REQUESTS = [("1 MiB", "big_1MiB"), ("4 MiB", "big_4MiB"), ("16 MiB", "big_16MiB"),
            ("1 GiB", "big_1GiB"), ("chunk size - 16 B", "big_chunk_max")]

def run(name, define, src, size, runs):
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, f"bsize_{name}_{size}")
    cmd = [CC, *CFLAGS, "-D" + define, f"-DAFA_SIZE={size}ULL",
           "-I" + os.path.join(ROOT, "mmap"), "-I" + os.path.join(ROOT, "arena_allocator"),
           os.path.join(ROOT, "bench/bench.c"), *([os.path.join(ROOT, src)] if src else []), "-o", exe]
    subprocess.run(cmd, check=True)
    vals = collections.defaultdict(list)
    for _ in range(runs):
        out = subprocess.run([exe, "big"], check=True, capture_output=True, text=True).stdout
        for line in out.splitlines():
            _, wl, metric, v = line.split(",")
            vals[(wl, metric)].append(float(v))
    return {k: sorted(v)[len(v) // 2] for k, v in vals.items()}

def cell(res, wl):
    if res.get((wl, "alloc_ns")) is None:
        return "NULL"
    a = res[(wl, "alloc_ns")] / 1000
    r = res[(wl, "release_ns")] / 1000
    f = res[(wl, "touch_full_ns")] / 1e6
    return f"{a:.2f} / {r:.2f} / {f:.2f}"

def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    malloc = run("malloc", "BE_MALLOC", None, 1 << 20, runs)
    print("# Large blocks across chunk sizes")
    print("\nEach cell is `alloc us / release us / memset-every-byte ms` per block, "
          "median of the process runs; `NULL` means every request failed. "
          "Request sizes are exact byte counts; `chunk size - 16 B` is the largest "
          "block that chunk can hold (arena_malloc needs 32 B for header and footer, so it "
          "fails that row by design). 2 GiB is included so a 1 GiB request can fit. "
          "One block is live at a time.\n")
    for label, size in SIZES:
        print(f"\n### Chunk / region size {label} (`-DAFA_SIZE={size}`)\n")
        print("| request | " + " | ".join(LABEL[n] for n in ["malloc"] + [b[0] for b in BACKENDS]) + " |")
        print("|---|" + "---:|" * (len(BACKENDS) + 1))
        res = {"malloc": malloc}
        for name, define, src in BACKENDS:
            print(f"[{name} @ {label}]", file=sys.stderr)
            res[name] = run(name, define, src, size, runs)
        for rl, wl in REQUESTS:
            if wl == "big_chunk_max":
                cells = ["n/a"] + [cell(res[n], wl) for n, _, _ in BACKENDS]
            else:
                cells = [cell(res[n], wl) for n in ["malloc"] + [b[0] for b in BACKENDS]]
            print(f"| {rl} | " + " | ".join(cells) + " |")

if __name__ == "__main__":
    main()
