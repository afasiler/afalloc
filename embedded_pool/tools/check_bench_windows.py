#!/usr/bin/env python3
"""Prove that every timed call in the benchmark is exactly  ldr / blx / ldr.

The measured sequence is one inline-assembly block (see TIME_ALLOC in src/bench_common.c):
    ldr t0, [counter]  /  blx fn  /  ldr t1, [counter]
This reads the compiled object and checks:
  * the expected number of such three-instruction windows exists (one per TIME_ALLOC/TIME_FREE
    call site: calibration 2, pair 2, cold 2, batch 2, churn 2 = 10);
  * no `blx` is only HALF bracketed by counter reads (a counter read on one side and something
    else on the other), which would mean the compiler moved an instruction into the window.
Ordinary indirect calls (also `blx`, e.g. the cleanup free after the churn loop) have no counter
read on either side and are fine."""
import re, subprocess, sys

EXPECTED_SITES = 10
objdump, obj = sys.argv[1], sys.argv[2]
text = subprocess.run([objdump, "-d", "--no-show-raw-insn", obj], capture_output=True, text=True, check=True).stdout
ins = [re.sub(r"^\s+[0-9a-f]+:\s+", "", l.rstrip()).split("\t@")[0].strip()
       for l in text.splitlines() if re.match(r"\s+[0-9a-f]+:\s", l)]
# a counter read: ldr rX, [rY]  or  ldr rX, [rY, #0]  (narrow and wide encodings)
rd = re.compile(r"ldr(?:\.w)?\s+(\w+),\s*\[(\w+)(?:,\s*#0)?\]$")
timed = partial = ordinary = 0
for i, x in enumerate(ins):
    if not x.startswith("blx"):
        continue
    before = rd.match(ins[i - 1]) if i > 0 else None
    after = rd.match(ins[i + 1]) if i + 1 < len(ins) else None
    if before and after and before.group(2) == after.group(2):
        timed += 1
    elif before or after:
        partial += 1
        print("call only half bracketed by counter reads:", ins[max(0, i - 2):i + 3])
    else:
        ordinary += 1
print(f"timed windows (ldr/blx/ldr): {timed}; half-bracketed (must be 0): {partial}; ordinary indirect calls: {ordinary}")
ok = partial == 0 and timed == EXPECTED_SITES
if not ok:
    print(f"FAIL: expected exactly {EXPECTED_SITES} clean timed windows")
sys.exit(0 if ok else 1)
