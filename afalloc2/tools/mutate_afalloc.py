#!/usr/bin/env python3
"""Mutation testing for afalloc.c: inject ONE deliberate bug at a time into a copy and run the tests
(functional at security levels 0/1/2, the multi-threaded tests, the security tests and the soak test). A mutant is
CAUGHT if any test fails, crashes or hangs; it SURVIVES if everything still passes.
usage: python3 tools/mutate_afalloc.py [name-substring]"""
import os, shutil, signal, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFLAGS = "-std=gnu11 -O1 -g -Wall -pthread"

# (name, old, new)
M = [
 ("class: 16-byte rounding off by one", "return n ? (unsigned)((n + 15) >> 4) - 1 : 0;", "return n ? (unsigned)((n + 14) >> 4) - 1 : 0;"),
 ("class: wrong sub-step above 128", "(((n - 1) - ((size_t)1 << b)) >> (b - 2))", "(((n - 1) - ((size_t)1 << b)) >> (b - 1))"),
 ("class: table size wrong", "class_size[c] = (1u << b) + (j + 1) * (1u << (b - 2));", "class_size[c] = (1u << b) + j * (1u << (b - 2)) + 16;"),
 ("slab: capacity ignores the guard page", "m->cap = (uint32_t)((SLAB_SIZE - guard_bytes()) / size);", "m->cap = (uint32_t)(SLAB_SIZE / size);"),
 ("slab: reciprocal rounds down", "m->magic = (((uint64_t)1 << 40) + size - 1) / size;", "m->magic = ((uint64_t)1 << 40) / size;"),
 ("slab: no guard page is ever protected", "if (mprotect(d, SLAB_SIZE - guard_bytes(), PROT_READ | PROT_WRITE) != 0) {", "if (mprotect(d, SLAB_SIZE, PROT_READ | PROT_WRITE) != 0) {"),
 ("alloc: used never counted (fast path)", "        m->local_free = (void *)nx;\n        m->used++;\n        return (void *)b;\n    }\n    return small_alloc_slow(h, c);", "        m->local_free = (void *)nx;\n        return (void *)b;\n    }\n    return small_alloc_slow(h, c);"),
 ("alloc: used never counted (slow path)", "            m->local_free = (void *)nx;\n            m->used++;", "            m->local_free = (void *)nx;"),
 ("alloc: fresh blocks carved twice", "    uint32_t i0 = m->fresh;\n    m->fresh += k;", "    uint32_t i0 = m->fresh;\n    m->fresh += k > 1 ? k - 1 : k;"),
 ("alloc: level-2 bitmap bit never set (fast)", "            if (m->flags & FLAG_EXACT) {\n                bitmap_set(m, (void *)b);\n                *(uintptr_t *)b = 0;\n            }\n        }\n        m->local_free = (void *)nx;", "            if (m->flags & FLAG_EXACT) {\n                *(uintptr_t *)b = 0;\n            }\n        }\n        m->local_free = (void *)nx;"),
 ("alloc: stale free-list link not cleared at level 2", "                bitmap_set(m, (void *)b);\n                *(uintptr_t *)b = 0;\n            }\n        }\n        m->local_free = (void *)nx;", "                bitmap_set(m, (void *)b);\n            }\n        }\n        m->local_free = (void *)nx;"),
 ("alloc: wild free-list link accepted (fast path)", "            if (!link_ok(m, b, nx))\n                die(\"corrupted free list (write after free?)\");\n            if (m->flags & FLAG_EXACT) {\n                bitmap_set(m, (void *)b);\n                *(uintptr_t *)b = 0;\n            }\n        }\n        m->local_free = (void *)nx;", "            if (m->flags & FLAG_EXACT) {\n                bitmap_set(m, (void *)b);\n                *(uintptr_t *)b = 0;\n            }\n        }\n        m->local_free = (void *)nx;"),
 ("alloc: free-list link range check removed", "return (next - (uintptr_t)m->data) < (uintptr_t)m->cap * m->block_size && ((next ^ blk) >> SLAB_SHIFT) == 0\n        && (next & 15) == 0;", "return ((next ^ blk) >> SLAB_SHIFT) == 0\n        && (next & 15) == 0;"),
 ("free: block never pushed on the list", "    *(uintptr_t *)p = (uintptr_t)m->local_free ^ m->key;\n    m->local_free = p;\n    if (unlikely(--m->used == 0", "    *(uintptr_t *)p = (uintptr_t)m->local_free ^ m->key;\n    if (unlikely(--m->used == 0"),
 ("free: used not decremented", "if (unlikely(--m->used == 0 || !atomic_load_explicit(&m->linked, memory_order_relaxed)))", "if (unlikely(m->used == 0 || !atomic_load_explicit(&m->linked, memory_order_relaxed)))"),
 ("free: interior pointer accepted", "(uintptr_t)i * m->block_size != off)", "0)"),
 ("free: immediate double free accepted", "    if (p == m->local_free)\n        die(\"double free detected\");", ""),
 ("free: exact double-free test inverted", "        if (!bitmap_clear(m, p))\n            die(\"double free or free of a block that is not allocated\");\n        if (m->zero_on_free)\n            memset(p, 0, m->block_size);\n        void *old", "        if (bitmap_clear(m, p))\n            die(\"double free or free of a block that is not allocated\");\n        if (m->zero_on_free)\n            memset(p, 0, m->block_size);\n        void *old"),
 ("free: zero-on-free skipped (local)", "        if (m->zero_on_free)\n            memset(p, 0, m->block_size);\n        void *old", "        void *old"),
 ("free: quarantine never checks for writes", "                for (uint32_t i = 1; i < om->block_size / 8; i++)\n                    if (w[i])\n                        die(\"write after free detected (a freed block was modified)\");", "                (void)w;"),
 ("free: foreign pointer inside the heap accepted", "            if (G.level >= 1 && m->state != 1)\n                die(\"free of a pointer into an unused part of the heap\");", ""),
 ("free: foreign pointer outside the heap accepted", "    if (G.level >= 1)\n        die(\"free of a pointer that was not allocated by afalloc\");", ""),
 ("free: remote path skips the block check", "    if (m->flags & FLAG_HARD) {\n        check_block(m, p);\n        if (m->flags & FLAG_EXACT) {", "    if (m->flags & FLAG_HARD) {\n        if (m->flags & FLAG_EXACT) {"),
 ("remote: free-list links not encrypted", "        *(uintptr_t *)p = head ^ m->key;\n    } while (!atomic_compare_exchange_weak_explicit(&m->remote_free, &head, (uintptr_t)p, memory_order_release,\n                                                      memory_order_relaxed));\n    atomic_thread_fence(memory_order_seq_cst);", "        *(uintptr_t *)p = head;\n    } while (!atomic_compare_exchange_weak_explicit(&m->remote_free, &head, (uintptr_t)p, memory_order_release,\n                                                      memory_order_relaxed));\n    atomic_thread_fence(memory_order_seq_cst);"),
 ("abandoned: links not encrypted", "    do {                                            /* other threads may still push without the lock */\n        *(uintptr_t *)p = head ^ m->key;", "    do {                                            /* other threads may still push without the lock */\n        *(uintptr_t *)p = head;"),
 ("abandoned: last free does not return the slab", "    if (--m->ab_live == 0) {\n        ab_remove(m);\n        slab_return_locked(m);\n    }", "    --m->ab_live;"),
 ("abandoned: live count off by one", "            s->ab_live = s->used;", "            s->ab_live = s->used > 1 ? s->used - 1 : s->used;"),
 ("abandoned: adoption does not remove it from the list", "        if (s) {\n            ab_remove(s);\n            atomic_store_explicit(&s->owner, h, memory_order_release);", "        if (s) {\n            G.abandoned[c] = s->ab_next;\n            atomic_store_explicit(&s->owner, h, memory_order_release);"),
 ("remote: collected blocks not subtracted from used", "    *(uintptr_t *)b = (uintptr_t)m->local_free ^ m->key;\n    m->local_free = (void *)head;\n    m->used -= n;", "    *(uintptr_t *)b = (uintptr_t)m->local_free ^ m->key;\n    m->local_free = (void *)head;"),
 ("remote: tail not linked to the local list", "    *(uintptr_t *)b = (uintptr_t)m->local_free ^ m->key;\n    m->local_free = (void *)head;\n    m->used -= n;", "    m->local_free = (void *)head;\n    m->used -= n;"),
 ("remote: owner never notified about a full slab", "    if (unlikely(!atomic_load_explicit(&m->linked, memory_order_relaxed))) {\n        heap_t *o", "    if (0) {\n        heap_t *o"),
 ("slow: full slab keeps being current", "                h->cur[c] = &g_empty;\n                m = &g_empty;\n                continue;", "                continue;"),
 ("slow: avail slab with remote frees not collected", "        avail_remove(h, s);\n        if (s->used == 0 && h->empty_kept[c])\n            h->empty_kept[c]--;\n        collect_remote(s);", "        avail_remove(h, s);\n        if (s->used == 0 && h->empty_kept[c])\n            h->empty_kept[c]--;"),
 ("slow: pending notifications never drained", "    if (atomic_load_explicit(&h->pending, memory_order_relaxed))\n        drain_pending(h);\n    slab_meta_t *m = h->cur[c];", "    slab_meta_t *m = h->cur[c];"),
 ("thread exit: slabs never abandoned", "            atomic_store_explicit(&s->owner, NULL, memory_order_release);\n            ab_push(s);", "            atomic_store_explicit(&s->owner, NULL, memory_order_release);"),
 ("thread exit: remote frees not collected", "        collect_remote(s);                          /* under the lock, so the count below is exact */\n", ""),
 ("adopt: adopted slab not collected", "        all_push(h, s);\n        collect_remote(s);\n        if (s->used == 0) {\n            slab_release(h, s);\n            continue;", "        all_push(h, s);\n        if (s->used == 0) {\n            slab_release(h, s);\n            continue;"),
 ("large: no guard pages", "    size_t pg = G.page, guard = G.level >= 1 ? pg : 0;\n    if (n > ((size_t)1 << 46))", "    size_t pg = G.page, guard = 0;\n    if (n > ((size_t)1 << 46))"),
 ("large: cached block may be smaller than asked", "if (e->user && e->usable == data) {", "if (e->user && e->usable <= data) {"),
 ("large: freed block never leaves the live table", "                G.large[i].user = LARGE_TOMB;\n                G.large_live--;", "                G.large_live--;"),
 ("large: calloc skips clearing a cached block", "    else if (dirty)\n        memset(p, 0, total);", "    else if (0)\n        memset(p, 0, total);"),
 ("calloc: overflow not checked", "    if (__builtin_mul_overflow(n, m, &total)) {", "    total = n * m;\n    if (0) {"),
 ("realloc: copies too little", "    memcpy(q, p, n < have ? n : have);", "    memcpy(q, p, (n < have ? n : have) / 2);"),
 ("aligned_alloc: class not checked for alignment", "while (c < NCLASS && class_size[c] % align != 0)", "while (c < NCLASS && 0)"),
 ("usable_size: returns the requested class size minus 8", "return m->state == 1 ? m->block_size : 0;", "return m->state == 1 ? m->block_size - 8 : 0;"),
]

def run(cmd, cwd, timeout, env=None):
    p = subprocess.Popen(cmd, shell=True, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True, text=True)
    try:
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL); p.communicate()
        return -999, "TIMEOUT"

def test_one(src):
    tmp = tempfile.mkdtemp(prefix="mut_af_", dir=os.environ.get("MUT_TMP"))
    try:
        for f in ("afalloc.h",): shutil.copy(os.path.join(ROOT, f), tmp)
        shutil.copytree(os.path.join(ROOT, "tests"), os.path.join(tmp, "tests"))
        open(os.path.join(tmp, "afalloc.c"), "w").write(src)
        rc, out = run(f"cc {CFLAGS} tests/test_afalloc.c afalloc.c -o t_func && cc {CFLAGS} tests/test_security.c afalloc.c -o t_sec && cc {CFLAGS} tests/test_soak.c afalloc.c -o t_soak", tmp, 120)
        if rc != 0: return True, "does not compile"
        for lvl in (0, 1, 2):
            rc, out = run("perl -e 'alarm shift; exec @ARGV' 60 ./t_func", tmp, 90, dict(os.environ, AFALLOC_SECURE=str(lvl)))
            if rc != 0: return True, f"functional test, level {lvl}: " + ("hang" if rc in (-999, 142) else "crash" if rc < 0 or rc >= 128 else "failed")
        rc, out = run("perl -e 'alarm shift; exec @ARGV' 120 ./t_sec", tmp, 150)
        if rc != 0: return True, "security test: " + ("hang" if rc in (-999, 142) else "failed")
        rc, out = run("perl -e 'alarm shift; exec @ARGV' 180 ./t_soak", tmp, 200, dict(os.environ, AFALLOC_SECURE="1"))
        if rc != 0: return True, "soak test (memory does not stay bounded, or it crashed): " + ("hang" if rc in (-999, 142) else "failed")
        return False, "all tests passed"
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

def main():
    orig = open(os.path.join(ROOT, "afalloc.c")).read()
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    print("baseline:", end=" ", flush=True)
    caught, why = test_one(orig)
    if caught: print("FAILED ->", why); sys.exit(2)
    print("passes", flush=True)
    surv, bad, n = [], [], 0
    for name, old, new in M:
        if want and want not in name: continue
        c = orig.count(old)
        if c != 1:
            bad.append((name, c)); print(f"  [skip] {name}: pattern found {c} times"); continue
        n += 1
        t0 = time.time()
        caught, why = test_one(orig.replace(old, new))
        print(f"  {'CAUGHT  ' if caught else 'SURVIVED'} {name}   <- {why}  ({time.time()-t0:.0f}s)", flush=True)
        if not caught: surv.append(name)
    print(f"\n{n} mutants run, {n-len(surv)} caught, {len(surv)} survived, {len(bad)} invalid patterns")
    for s in surv: print("  SURVIVED:", s)
    for s, c in bad: print("  BAD PATTERN:", s, c)

main()
