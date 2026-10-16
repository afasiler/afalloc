#!/usr/bin/env python3
"""Mutation testing for embedded_pool/src/pool.c: inject ONE deliberate bug at a time into a copy of
the source and run the host tests (and, for interrupt-mask bugs, which are invisible on a computer,
the emulated-core tests). A mutant is CAUGHT if any
test fails, crashes, hangs, or the mutant no longer compiles; it SURVIVES if every test still passes.
Every command runs under a timeout in its own process group, at low priority.
usage: python3 tools/mutate_pool.py [name-substring]   (or: make mutate)"""
import os, shutil, signal, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # embedded_pool/
HOSTFLAGS = "-std=c11 -Wall -Wextra -g -O1 -DPOOL_HOST -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude"
ENV = dict(os.environ, LC_ALL="en_US.UTF-8")
ENV["PATH"] = ENV.get("PATH", "")

# (name, old text, new text, needs_emulator)
M = [
 ("route: ceil_log2 uses size instead of size-1", "(size - 1u) | 1u", "size | 1u", 0),
 ("route: table fill uses <= instead of <", "pools[j].block_size < (1u << k)", "pools[j].block_size <= (1u << k)", 0),
 ("route: table fill can run past the last pool", "j + 1 < NUM_POOLS && pools[j]", "j < NUM_POOLS && pools[j]", 0),
 ("alloc: size limit off by one (accepts MAX_BLOCK+1)", "size - 1u >= MAX_BLOCK", "size - 1u > MAX_BLOCK", 0),
 ("alloc: zero_size and too_big swapped", "            misc.zero_size++;\n        else\n            misc.too_big++;", "            misc.too_big++;\n        else\n            misc.zero_size++;", 0),
 ("owner_map: fill writes pool 0 for every granule", "owner_map[off / GRAN + g] = (uint8_t)i; \\\n    off += ALIGN_UP_GRAN((size_t)(size) * (count)); i++;\n#else", "owner_map[off / GRAN + g] = 0; \\\n    off += ALIGN_UP_GRAN((size_t)(size) * (count)); i++;\n#else", 0),
 ("owner_map: release-build fill writes pool 0", "owner_map[off / GRAN + g] = (uint8_t)i; \\\n    off += ALIGN_UP_GRAN((size_t)(size) * (count)); i++;\n#endif", "owner_map[off / GRAN + g] = 0; \\\n    off += ALIGN_UP_GRAN((size_t)(size) * (count)); i++;\n#endif", 0),
 ("owner_map: lookup indexes the wrong granule", "&pools[owner_map[off / GRAN]]", "&pools[owner_map[(off / GRAN) & ~(size_t)1]]", 0),
 ("granule_owner: arena bound off by one", "if (off >= ARENA_BYTES)", "if (off > ARENA_BYTES)", 0),
 ("find_owner: padding counted as the pool's", "((uintptr_t)p->count << p->shift))\n        return 0;", "((uintptr_t)p->count << p->shift) + 1)\n        return 0;", 0),
 ("find_owner: exact check removed", "if (p && (uintptr_t)ptr - (uintptr_t)p->base >= ((uintptr_t)p->count << p->shift))\n        return 0;", "", 0),
 ("free: unaligned pointer accepted (mask wrong)", "((off & (p->block_size - 1u)) | (off >= p->used))", "((off & (p->block_size - 2u)) | (off >= p->used))", 0),
 ("free: block at `used` accepted (never handed out)", "(off >= p->used)", "(off > p->used)", 0),
 ("free: `used` check removed", "| (off >= p->used)", "| 0", 0),
 ("free: in_use not decremented", "    p->in_use--;\n    pool_irq_restore(state);\n}", "    pool_irq_restore(state);\n}", 0),
 ("free: block not pushed on the list", "    p->free_head = n;\n    p->in_use--;", "    p->in_use--;", 0),
 ("free_refused: padding counted as bad_free", "off >= (p->count << p->shift))\n        misc.unowned_free++;", "off > (p->count << p->shift))\n        misc.unowned_free++;", 0),
 ("free_refused: bad_free never counted", "        p->bad_free++;                                  /* middle", "        (void)0;                                        /* middle", 0),
 ("free (debug): double-free test inverted", "if ((p->bitmap[idx >> 5] & bit) == 0) {          /* already free", "if ((p->bitmap[idx >> 5] & bit) != 0) {          /* already free", 0),
 ("free (debug): bit not cleared", "        p->bitmap[idx >> 5] &= ~bit;\n        poison", "        poison", 0),
 ("free (debug): poison removed", "        poison((uint8_t *)ptr, p->block_size);\n    }\n#endif\n    free_node_t", "    }\n#endif\n    free_node_t", 0),
 ("take_fresh: exhaustion check off by one", "if (p->used >= (p->count << p->shift)) {", "if (p->used > (p->count << p->shift)) {", 0),
 ("take_fresh: `used` advances by 8", "p->used += p->block_size;", "p->used += 8;", 0),
 ("take_fresh: alloc_fail not counted", "        if (!lent)\n            p->alloc_fail++;\n        return 0;", "        return 0;", 0),
 ("take_fresh: alloc_fail counted for lent pools too", "        if (!lent)\n            p->alloc_fail++;", "        p->alloc_fail++;", 0),
 ("account: in_use not incremented", "    p->in_use++;\n#if POOL_TRACK_PEAK", "#if POOL_TRACK_PEAK", 0),
 ("account: peak never updated", "        p->peak = p->in_use;\n#endif\n    if (lent)", "        (void)0;\n#endif\n    if (lent)", 0),
 ("account: spill_in not counted", "    if (lent)\n        p->spill_in++;", "", 0),
 ("account (debug): bitmap bit not set", "        p->bitmap[idx >> 5] |= 1u << (idx & 31u);\n    }\n#else", "    }\n#else", 0),
 ("pop_free: list head not advanced", "        p->free_head = n->next;                         /* reuse", "        (void)0;                                        /* reuse", 0),
 ("pop_free (debug): bitmap sanity check removed", "            sane = (p->bitmap[idx >> 5] & (1u << (idx & 31u))) == 0;", "            sane = 1;", 0),
 ("pop_free (debug): range sanity check removed", "        int sane = off < (uintptr_t)p->count * p->block_size\n                && (off & (p->block_size - 1u)) == 0;", "        int sane = 1;", 0),
 ("pop_free (debug): corrupt link not counted", "            misc.corrupt_links++;\n", "", 0),
 ("alloc_slow: spill starts at the pool itself", "(unsigned)(p - pools) + 1; result == 0", "(unsigned)(p - pools); result == 0", 0),
 ("alloc_slow: spill never tries (loop bound)", "result == 0 && j < NUM_POOLS; j++)\n        result = take_lent", "result == 0 && j < 0; j++)\n        result = take_lent", 0),
 ("init: ascending check allows equal sizes", "pools[k].block_size <= pools[k - 1].block_size", "pools[k].block_size < pools[k - 1].block_size", 0),
 ("init: ascending check removed", "            rc = -1;                                    /* must be strictly ascending */", "            (void)0;", 0),
 ("init (debug): bitmap not cleared", "        bitmap[i] = 0;", "        (void)bitmap;", 0),
 ("stats: reset forgets alloc_fail", "pools[i].alloc_fail = pools[i].bad_free = pools[i].spill_in = 0;", "pools[i].bad_free = pools[i].spill_in = 0;", 0),
 ("stats: reset forgets misc counters", "    misc.zero_size = misc.too_big = misc.unowned_free = misc.corrupt_links = 0;\n    pool_irq_restore(state);\n}\n\nuint32_t pool_total_bytes", "    pool_irq_restore(state);\n}\n\nuint32_t pool_total_bytes", 0),
 ("block_size: wrong answer", "return p ? p->block_size : 0;", "return p ? p->block_size / 2 : 0;", 0),
 ("check: cycle detection removed (code 1)", "        if (listed >= fresh)\n            return 1;", "", 0),
 ("check: range check removed (code 2)", "            return 2;", "            (void)0;", 0),
 ("check: missing-blocks test removed (code 6)", "    if (listed + p->in_use != fresh)\n        return 6;", "", 0),
 ("check (debug): poison scan removed (code 5)", "                    return 5; ", "                    (void)0; ", 0),
 ("fixed-size: pool_class_of off by one (<=)", "((size_t)(block_size) < pool_sz_)", "((size_t)(block_size) <= pool_sz_)", 0, "include/pool.h"),
 ("fixed-size: pool_class_of accepts size 0", "return pool_sz_ == 0 ? (size_t)(0 POOL_TABLE(POOL__ONE_)) : (size_t)(0 POOL_TABLE(POOL__LT_));", "return (size_t)(0 POOL_TABLE(POOL__LT_));", 0, "include/pool.h"),
 ("fixed-size: class bounds check off by one", "class_index >= NUM_POOLS", "class_index > NUM_POOLS", 0),
 ("fixed-size: out-of-range class not counted", "    if (__builtin_expect(class_index >= NUM_POOLS, 0)) {\n        misc.too_big++;\n", "    if (__builtin_expect(class_index >= NUM_POOLS, 0)) {\n", 0),
 ("fixed-size: class ignored (always pool 0)", "alloc_from(&pools[class_index], state)", "alloc_from(&pools[0], state)", 0),
 # interrupt mask bugs: invisible on the host (irq is a no-op there), must be caught by the emulator
 ("irq: fast alloc path forgets pool_irq_restore", "        account(p, n, 0);\n        pool_irq_restore(state);\n        return n;", "        account(p, n, 0);\n        return n;", 1),
 ("irq: free forgets pool_irq_restore", "    p->in_use--;\n    pool_irq_restore(state);\n}", "    p->in_use--;\n}", 1),
 ("irq: refused free forgets pool_irq_restore", "        p->bad_free++;                                  /* middle of a block, never allocated, or double free */\n    pool_irq_restore(state);", "        p->bad_free++;                                  /* middle of a block, never allocated, or double free */", 1),
 ("irq: alloc_slow forgets pool_irq_restore", "    pool_irq_restore(state);\n    return result;\n}\n\n/* The part of pool_alloc()", "    return result;\n}\n\n/* The part of pool_alloc()", 1),
 ("irq: class alloc out-of-range path forgets pool_irq_restore", "        misc.too_big++;\n        pool_irq_restore(state);\n        return 0;\n    }\n\n    return alloc_from(&pools[class_index]", "        misc.too_big++;\n        return 0;\n    }\n\n    return alloc_from(&pools[class_index]", 1),
 ("irq: zero/too-big alloc path forgets pool_irq_restore", "            misc.too_big++;\n        pool_irq_restore(state);\n        return 0;", "            misc.too_big++;\n        return 0;", 1),
]


def run(cmd, cwd, timeout):
    p = subprocess.Popen(cmd, shell=True, cwd=cwd, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True, text=True)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.communicate()
        return -999, "TIMEOUT"


def host_cmds():
    def cc(extra, srcs, out):
        return f"nice cc {HOSTFLAGS} {extra} {srcs} -o build/{out} && perl -e 'alarm shift; exec @ARGV' 40 build/{out}"
    t = "tests/test_pool_host.c src/pool.c src/pool_selftest.c"
    w = "tests/test_pool_whitebox.c"
    return [
        ("unit tests", cc("", t, "t1")),
        ("unit tests, spill", cc("-DPOOL_SPILL=1", t, "t2")),
        ("unit tests, release", cc("-DPOOL_DEBUG=0", t, "t3")),
        ("white-box", cc("", w, "w1")),
        ("white-box, spill", cc("-DPOOL_SPILL=1", w, "w2")),
        ("white-box, release", cc("-DPOOL_DEBUG=0", w, "w3")),
        ("white-box, padded tables", cc("-D'POOL_TABLE(X)=X(8,3) X(16,5) X(32,7)'", w, "w4")),
        ("white-box, bad table", cc("-DPOOL_EXPECT_BAD_TABLE -D'POOL_TABLE(X)=X(16,2) X(16,3)'", w, "w5")),
        ("unit tests, tiny table", cc("-D'POOL_TABLE(X)=X(8,2) X(16,1)'", t, "t4")),
    ]


def emu_cmd():
    # a Thumb-1 core (no CLZ) and a Thumb-2 core, both emulated
    return [("emulated cores", "nice make test-qemu QEMU_TARGETS='microbit:cortex-m0:0x4000 mps2-an386:cortex-m4:0x10000' QEMU_TIMEOUT=40")]


def setup(tmp, pool_src, rel="src/pool.c"):
    for d in ("include", "src", "tests", "configs", "ports"):
        shutil.copytree(os.path.join(REPO, d), os.path.join(tmp, d), ignore=shutil.ignore_patterns("build"))
    shutil.copy(os.path.join(REPO, "Makefile"), tmp)
    os.makedirs(os.path.join(tmp, "build"), exist_ok=True)
    with open(os.path.join(tmp, rel), "w") as fh:
        fh.write(pool_src)


def test_one(pool_src, need_emu, rel="src/pool.c"):
    tmp = tempfile.mkdtemp(prefix="mut_", dir=os.environ.get("MUT_TMP"))
    try:
        setup(tmp, pool_src, rel)
        cmds = host_cmds() + (emu_cmd() if need_emu else [])
        for name, cmd in cmds:
            rc, out = run(cmd, tmp, 150 if name == "emulated cores" else 100)
            if rc != 0:
                why = "TIMEOUT" if rc == -999 else ("compile error" if (" error:" in out and "runtime error" not in out) else "test failed")
                return True, f"{name}: {why}"
        return False, "all tests passed"
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    orig = open(os.path.join(REPO, "src", "pool.c")).read()
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    print("baseline (unmutated):", end=" ", flush=True)
    caught, why = test_one(orig, True)
    if caught:
        print("FAILED -> mutation results would be meaningless:", why)
        sys.exit(2)
    print("passes host + emulated-core tests", flush=True)
    survivors, bad = [], []
    n = 0
    for entry in M:
        name, old, new, emu = entry[:4]
        rel = entry[4] if len(entry) > 4 else "src/pool.c"
        text = orig if rel == "src/pool.c" else open(os.path.join(REPO, rel)).read()
        if want and want not in name:
            continue
        cnt = text.count(old)
        if cnt != 1:
            bad.append((name, cnt))
            print(f"  [skip] {name}: pattern found {cnt} times (must be exactly 1)")
            continue
        n += 1
        t0 = time.time()
        caught, why = test_one(text.replace(old, new), emu, rel)
        print(f"  {'CAUGHT  ' if caught else 'SURVIVED'} {name}   <- {why}  ({time.time()-t0:.0f}s)", flush=True)
        if not caught:
            survivors.append(name)
    print(f"\n{n} mutants run, {n-len(survivors)} caught, {len(survivors)} survived, {len(bad)} patterns invalid")
    for s in survivors:
        print("  SURVIVED:", s)
    for s, c in bad:
        print("  BAD PATTERN:", s, c)


main()
