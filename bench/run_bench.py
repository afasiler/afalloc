#!/usr/bin/env python3
"""Build and run bench/bench.c once per allocator backend and print a report."""
import os, subprocess, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
CC = os.environ.get("CC", "cc")
CFLAGS = os.environ.get("BENCH_CFLAGS", "-O2 -Wall -Wextra -Wno-unused-function").split()

BACKENDS = [  # name, define, sources
    ("malloc",  "BE_MALLOC",  []),
    ("persist", "BE_PERSIST", ["mmap/afalloc_persistent.c"]),
    ("arena",   "BE_ARENA",   ["arena_allocator/rv1103.c"]),
    ("arenaf",  "BE_ARENAF",  ["arena_allocator/arena_malloc.c"]),
    ("gp",      "BE_GP",      ["mmap/mmap_allocator.c"]),
]
LABEL = {"malloc": "malloc/free", "persist": "persist (mmap bump)",
         "arena": "arena (rv1103)", "arenaf": "arena_malloc (free)", "gp": "gp (mmap first-fit)"}

def build_and_run(name, define, srcs, runs):
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "bench_" + name)
    cmd = [CC, *CFLAGS, "-D" + define, "-I" + os.path.join(ROOT, "mmap"),
           "-I" + os.path.join(ROOT, "arena_allocator"),
           os.path.join(ROOT, "bench/bench.c"), *[os.path.join(ROOT, s) for s in srcs], "-o", exe]
    subprocess.run(cmd, check=True)
    out = []
    for _ in range(runs):
        out.append(subprocess.run([exe], check=True, capture_output=True, text=True).stdout)
    return out

def parse(outs):
    """-> {(workload, metric): median over runs}"""
    vals = collections.defaultdict(list)
    for out in outs:
        for line in out.splitlines():
            _, wl, metric, v = line.split(",")
            vals[(wl, metric)].append(float(v))
    return {k: sorted(v)[len(v) // 2] for k, v in vals.items()}

def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    res = {}
    for name, define, srcs in BACKENDS:
        print(f"[bench] {name} ({runs} process runs)", file=sys.stderr)
        res[name] = parse(build_and_run(name, define, srcs, runs))

    names = [b[0] for b in BACKENDS]
    def cell(name, wl, metric, fmt="{:.1f}"):
        v = res[name].get((wl, metric))
        return "n/a" if v is None else fmt.format(v)
    def table(title, rows, header_note=""):
        print(f"\n### {title}\n{header_note}")
        print("| workload | " + " | ".join(LABEL[n] for n in names) + " |")
        print("|---|" + "---:|" * len(names))
        for label, wl, metric in rows:
            print(f"| {label} | " + " | ".join(cell(n, wl, metric) for n in names) + " |")

    batch = [("16 B x 100", "fixed_16B_x100"), ("16 B x 1,000", "fixed_16B_x1000"),
             ("16 B x 10,000", "fixed_16B_x10000"), ("16 B x 30,000", "fixed_16B_x30000"),
             ("256 B x 2,000", "fixed_256B_x2000"), ("4 KiB x 150", "fixed_4KiB_x150"),
             ("mixed sizes (~900 KiB)", "mixed_sizes")]
    print(f"# afalloc benchmark ({runs} process runs, median of {9} samples each)")
    table("Allocate, ns per allocation", [(l, w, "alloc_ns") for l, w in batch])
    table("Release the whole batch, ns per allocation",
          [(l, w, "release_ns") for l, w in batch],
          "malloc frees every block; the others do one bulk reset.\n")
    # total = alloc + release
    print("\n### Total (alloc + bulk release), ns per allocation")
    print("| workload | " + " | ".join(LABEL[n] for n in names) + " | malloc / persist |")
    print("|---|" + "---:|" * (len(names) + 1))
    for l, w in batch:
        tot = {n: res[n][(w, "alloc_ns")] + res[n][(w, "release_ns")] for n in names}
        ratio = tot["malloc"] / tot["persist"]
        print(f"| {l} | " + " | ".join(f"{tot[n]:.1f}" for n in names) + f" | {ratio:.1f}x |")
    table("First-touch / use (write first+last byte), ns per allocation",
          [(l, w, "touch_ns") for l, w in batch])
    table("Failed allocations (must be 0)", [(l, w, "failed_allocs") for l, w in batch],
          "")
    table("Address-space cost of a 16 B allocation (bytes)",
          [("bytes per 16 B alloc", "density_16B", "bytes_per_alloc")])
    large = [("64 KiB", "large_64KiB"), ("256 KiB", "large_256KiB"), ("512 KiB", "large_512KiB"),
             ("1 MiB - 16 B (largest supported)", "large_max"),
             ("random 1 B .. 1 MiB - 16 B", "large_random"),
             ("exactly 1 MiB", "large_1MiB_exact")]
    def utable(title, metric, note=""):
        print(f"\n### {title}\n{note}")
        print("| block size | " + " | ".join(LABEL[n] for n in names) + " |")
        print("|---|" + "---:|" * len(names))
        for l, w in large:
            cells = []
            for n in names:
                v = res[n].get((w, metric))
                cells.append("n/a" if v is None else f"{v / 1000:.3f}")
            print(f"| {l} | " + " | ".join(cells) + " |")
    print("\n## Large allocations: 1,000 iterations of alloc, touch, release per block size")
    print("Times are microseconds per block. One block is live at a time because the bump arenas are a single 1 MiB region.")
    utable("Allocate (us per block)", "alloc_ns")
    utable("Release (us per block)", "release_ns")
    utable("Touch first and last byte (us per block)", "touch_sparse_ns")
    utable("Write every byte with memset (us per block)", "touch_full_ns",
           "Includes page-fault cost on fresh pages; bump arenas reuse already-faulted pages.\n")
    print("\n### Failed allocations per 1,000 requests")
    print("| block size | " + " | ".join(LABEL[n] for n in names) + " |")
    print("|---|" + "---:|" * len(names))
    for l, w in large:
        print(f"| {l} | " + " | ".join(cell(n, w, "failed_allocs", "{:.0f}") for n in names) + " |")
    table("Per-allocation latency, mixed sizes (ns)",
          [("p50", "latency_mixed", "p50_ns"), ("p99", "latency_mixed", "p99_ns"),
           ("p99.9", "latency_mixed", "p999_ns"), ("max", "latency_mixed", "max_ns"),
           ("timer floor", "latency_mixed", "timer_floor_ns")],
          "Single-op timing; the clock ticks coarsely (see timer floor), so trust the tail, not p50.\n")
    table("Interleaved alloc/free churn (256 live slots, 16..512 B), ns per op",
          [("ns/op", "churn_alloc_free", "ns_per_op"),
           ("failed allocs", "churn_alloc_free", "failed_allocs")],
          "`arena` (rv1103) has no free, so it cannot run this workload.\n")

if __name__ == "__main__":
    main()
