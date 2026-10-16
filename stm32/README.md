# afalloc on the STM32F411RE

A bare-metal firmware project for the Nucleo-F411RE: our own startup code, linker script
and register access (no HAL, no RTOS, no C library), and a fixed-size **pool allocator** built
on top, with a cycle-accurate benchmark against newlib's `malloc`.

## Layers

```text
 apps/app_*.c     what runs: blink | selftest | bench | bench_malloc
 main.c           ordered bring-up, progress shown on the LED
 pool.c           the allocator (pure C, no hardware access, unit-tested on the Mac)
 bench_common.c   cycle-count harness (works for any malloc/free pair)
 board.c uart.c clock.c dwt.h   LED + delays, serial output, PLL clock, cycle counter
 regs.h           register map, every address/offset taken from DS10314 / RM0383
 startup.c        vector table + Reset_Handler (.data copy, .bss clear, FPU on, call main)
 linker.ld        memory map: flash, RAM, pools, stack accounting
```

## Memory map (what `linker.ld` builds)

```text
FLASH 0x0800 0000  vector table (408 B) | code + constants | copy of .data
RAM   0x2000 0000  .data | .bss | .pool_mem (the pools) | ... free ... | stack <-- 0x2002 0000
```
The pools share one static arena in `.pool_mem` (each pool starts on a 64-byte boundary). The linker also accounts for 4 KB of stack
(`_Min_Stack_Size`): if data + pools + stack do not fit in 128 KB, the **link** fails
("region RAM overflowed"). The stack itself lives at the top of RAM and grows down; nothing
detects an overflow yet (the planned guard is an MPU region).

## Commands

| Command | What it does |
|---|---|
| `make host-test-all` | everything that runs on the Mac (the four lines below) |
| `make host-test` | pool unit tests under the address/UB sanitizers |
| `make host-test-configs` | the same tests under 11 pool configurations (tiny/one/big pools, debug on/off, spill) |
| `make host-test-white` | white-box tests that corrupt the pool's private state |
| `make host-bench-check` | runs the benchmark harness on the Mac with a fake cycle counter (logic check) |
| `make emu-test` | runs the **real startup code + pool** on an emulated Cortex-M4 (QEMU) |
| `make check-bench-windows` | proves on the compiled code that every timed call is exactly `ldr / blx / ldr` |
| `make all-apps` | builds every app for the board (compile + link check) |
| `make APP=blink flash` | builds and writes an app to the board over SWD |
| `make size APP=...` / `make disasm APP=...` | flash/RAM usage / listing |

Apps: `blink` (first bring-up), `selftest` (pool tests on the board), `bench` (pool timing),
`bench_malloc` (same timing on newlib-nano malloc).

Build options (changing one rebuilds everything that depends on it):
`POOL_DEBUG=0|1` extra pool checks (default 1; benchmarks default 0);
`POOL_IRQ_SAFE=0|1` interrupt masking in alloc/free (default 1; 0 shows what the safety costs);
`CLOCK_MHZ=96|100` CPU clock (default 96, see below).

Serial output: the board's USB cable also carries a serial port (USART2 on PA2/PA3):
`screen /dev/cu.usbmodem* 115200`.

## The clock: why 96 MHz and not 100 MHz

The chip's maximum is 100 MHz, but the PLL is fed by the internal 16 MHz HSI, which is only
accurate to +-1 % at 25 C and up to +5.5 % hot (DS10314 Table 39, p.87). The PLL multiplies that
error, so a nominal 100 MHz can really be 101 MHz or more: out of specification. The default is
therefore **96 MHz** (M=8, N=96, P=/2): about 4 % margin, 3 flash wait states (valid for 90..100 MHz),
APB1 at 48 MHz, and the PLL's second output lands on exactly 48 MHz. `CLOCK_MHZ=100` is
available (N=100, Q=5) for experiments. `clock_init_pll()` also waits for the voltage
regulator (`PWR_CSR.VOSRDY`), works if called a second time, and puts the chip back on the HSI
if any step fails.

## The pool allocator

Memory is split ahead of time into pools of equally sized blocks (sizes are powers of two,
set in `src/pool_config.h`). `pool_alloc(n)` picks the smallest pool whose blocks fit and pops
the first block off that pool's free list; `pool_free(p)` pushes it back. The link to the next
free block is stored inside the free block itself, so blocks need **no header**.

With `POOL_DEBUG=0` both calls take a **fixed number of steps, whatever the request size or the
number of pools** (measured on the board: identical cycle counts for 16, 64 and 256 B):
- *alloc* turns the size into a pool with a count-leading-zeros instruction and a 32-byte lookup
  table (`route[]`), then pops;
- *free* finds the owning pool from the pointer with one table read: the arena is divided into
  64-byte granules (`POOL_OWNER_GRANULE`) and `owner_map[]` holds one byte per granule naming its
  pool. A pool whose size is not a multiple of the granule leaves up to 63 bytes of padding, which
  `pool_free` refuses as "not a block";
- the uncommon cases (empty free list, refused free) are out of line, so the common path is a
  short leaf function with no stack frame.

Cost of this in RAM, for the current 5 pools: `owner_map` 176 B (pool memory / 64), `route` 32 B,
and each pool descriptor is padded to 64 B (so `&pools[i]` is a shift). Pool memory itself is
unchanged (11264 B). There is no search and no fragmentation inside a pool, and the critical
section (interrupts masked) lasts a few instructions, so both calls are safe from interrupt
handlers (not from NMI/HardFault, which PRIMASK cannot mask). Per-pool peak usage tracking
(`POOL_TRACK_PEAK`, default on) costs 6 cycles per alloc and is the number to read when sizing the
pools.

With `POOL_DEBUG=1` a bitmap catches double frees, freed blocks are filled with a poison pattern so
writes after free are detected by `pool_check()`, and a free-list link that was overwritten is
abandoned instead of followed; that build is for catching bugs, not for timing. The release build
trusts the caller (a double free is not detected).

## The benchmark: what it measures and what it cannot tell you

`bench` and `bench_malloc` run the same scenarios with the same function-pointer call: alloc+free
pairs (16/64/256 B), a batch of 16, a random 8..256 B workload (16 live objects, same random
sequence for both allocators), and a **cold** pair measured after emptying the ART caches. Each
row reports min, median, mean, p99, p99.9 and max in **net cycles per call**. Each timed call is a
single inline-assembly block (`ldr counter / blx fn / ldr counter`), so the compiler cannot move
instructions into the measured window; `make check-bench-windows` proves it. The cost of measuring
(an empty call through a function pointer) is calibrated the same way and subtracted. The banner
prints the build flags and warns if the clock setup failed.

### Results measured on the board (96 MHz, `-O2`, `POOL_DEBUG=0`, net cycles per call)

| | first version | now | newlib-nano `malloc` |
|---|---|---|---|
| alloc 16 / 64 / 256 B | 43 / 61 / 79 | **36 / 36 / 36** | 86 / 90 / 90 |
| free 16 / 64 / 256 B | 56 / 76 / 96 | **40 / 40 / 40** | 52 / 67 / 67 |
| random 8..256 B workload, alloc (p50 / max) | 79 / 92 | 36 / 55 | 104 / 266 |
| random workload, free (p50 / max) | 95 / 95 | 40 / 40 | 81 / 130 |
| cold (caches flushed) alloc / free 64 B | 81 / 90 | 45 / 46 | 134 / 94 |

(36 cycles = 375 ns at 96 MHz.) The first version found its pool and its owner by looping over the
pools, so it got slower for bigger blocks (and its free was slower than `malloc`'s); the table
lookups removed that. What the knobs cost, measured the same way: interrupt safety
(`POOL_IRQ_SAFE=1` vs `0`) is 5 cycles on alloc and 3 on free; peak tracking
(`POOL_TRACK_PEAK=1` vs `0`) is 6 cycles on alloc and nothing on free. These are one run each on
one board; the pool numbers are exactly repeatable (min = max), `malloc`'s vary with the heap state.

Read the results with these differences in mind:
- the pool is built `-O2` from source; newlib-nano is a prebuilt `-Os` library;
- the pool is interrupt-safe by default, this newlib's lock hooks are empty stubs, so its malloc is
  not (run `make APP=bench POOL_IRQ_SAFE=0` to see what the safety costs);
- every pool block has 0 bytes of header, malloc has a header per block; the benchmark prints
  the pool's rounding waste and malloc's heap high-water mark;
- 20000 random operations on 16 live objects do not stress malloc's worst case under heavy
  fragmentation; the numbers support "the pool's per-call cost is small and nearly constant here",
  not an exact speedup ratio or a hard worst-case bound.

## What has been verified, and how

| Claim | Verified by | Where |
|---|---|---|
| Pool logic is correct (routing, exhaustion, reuse, bad/double frees, no overlap, stress) | 12 unit tests under sanitizers, 11 pool configurations, `-O0/-O1/-O2/-Os`; also on the board (self-test) | `make host-test-all`, `make APP=selftest flash` |
| `pool_check()`, spill accounting, refusal of a damaged free list, rejection of a bad table | white-box tests that corrupt the pool's own state, on 12 table/granule/debug combinations | `make host-test-white` |
| Every request size reaches the smallest pool that fits; every byte of the arena maps to the right owner (including padding between pools) | exhaustive white-box loops against a brute-force search | `make host-test-white` |
| Alloc/free restore the interrupt mask on every path (fast, slow, refused, statistics), also when it was already masked | PRIMASK read back on the emulated core | `make emu-test` |
| The tests themselves catch bugs | 50 deliberate bugs injected into `pool.c` one at a time: 47 caught, the other 3 cannot change behaviour (see mutation testing below) | mutation run |
| Same logic on a real 32-bit Cortex-M4 (4-byte pointers, Thumb-2) | QEMU with the real startup code and linker script | `make emu-test` |
| Startup copies all of `.data`, clears all of `.bss` (RAM pre-filled with 0xA5), SP is 8-byte aligned | QEMU test; 12 bugs injected into startup/vector table/linker: all 12 caught | `make emu-test` |
| Vector table slots are right | QEMU: SVC, PendSV, SysTick, IRQ 28, 38 and 85 (the last entry) are each triggered and must reach exactly their own handler | `make emu-test` |
| Stack needed ~950 B for the pool test (4 KB accounted) | stack painting under QEMU | `make emu-test` |
| Register offsets match the manual | `_Static_assert`s against RM0383 offsets | `regs.h` |
| PLL numbers respect every limit in the manual | `_Static_assert`s, including the 48 MHz limit of the second PLL output | `clock.c` |
| Hardware constants match the manuals | two independent reviews: one derived every value from the PDFs without seeing the code, one audited the code line by line; they agreed | cross-validation below |
| The timed benchmark windows contain only the call | compiled-code check | `make check-bench-windows` |
| Chip identity, flash size, reset values, LED = PA5 active-high, debug path | read/written on the real board earlier | OpenOCD session |

### Cross-validation

Five independent reviewers (separate contexts, none trusting the author's claims) examined the
project: a blind derivation of the clock/UART/vector-table values from the PDFs alone, a line-by-line
audit of every hardware constant, an adversarial review of the pool, a review of startup/linker/ELF,
and a review of the benchmark method. Their findings and what was done:

| Found by | Finding | Result |
|---|---|---|
| blind derivation + audit | HSI is only +-1..5 % accurate, so a nominal 100 MHz can exceed the limit; second PLL output was 50 MHz > 48 MHz | default now 96 MHz, 48 MHz output, compile-time checks |
| audit | no wait for `VOSRDY`; function assumed reset state; `uart_putc` did not wait for the last byte | fixed (`VOSRDY` wait, return-to-HSI guard, `uart_flush`) |
| pool review | debug build is not constant-time; a damaged free-list link could cause a wild write; `pool_init` unsafe when repeated; invalid table only reported at run time | hardened and documented; `main` halts on a bad table |
| pool review | tests missed many bugs | 12 tests, white-box tests, config matrix, mutation-proven |
| startup review | emulator test could not detect a missing `.bss` clear, a misaligned SP, or wrong vector slots | RAM pre-fill, SP/VTOR checks, six slots triggered |
| startup review | build flags did not trigger a rebuild; FPU safety depended on the compiler | flag stamp; `-mgeneral-regs-only` for `startup.c` |
| benchmark review | instructions inside the timed window; calibration did not match; thin statistics | one inline-asm block per call + checker; same-shape calibration; percentiles, cold scenario |

### Mutation testing: do the tests actually catch bugs?

A test that passes proves nothing unless it can also fail. Each bug below was injected on its own
into a copy of the source, and the test suites were run; a hang counts as "caught" (the bug made the
program loop forever). A baseline run of the unmodified code must pass first.

| Where the bugs went | Injected | Caught | Not caught |
|---|---|---|---|
| `pool.c` (size routing, owner map, free-list checks, every `pool_check()` code, spill accounting, statistics, ...) | 45 | 42 | 3, all **equivalent** (below) |
| `pool.c`, forgetting to restore the interrupt mask (invisible on the host, so they run on the emulator) | 5 | 5 | 0 |
| `startup.c`, the vector table and `linker.ld` (missing/short `.bss` clear, short `.data` copy, no FPU enable, wrong Reset/PendSV/SysTick/TIM2/SPI5 slot, misaligned SP, `.data` load address off by a word, no `KEEP`) | 12 | 12 | 0 |

The three that cannot be caught change no behaviour, so no test could tell them apart from the
original: (1) starting the lending loop at the pool itself instead of the next one only retries a
pool that just failed and fails again without side effects; (2) not zeroing the free bitmap in
`init_one` is invisible because a block can only be freed once it has been handed out (`used`
gating), and handing it out sets its bit anyway; (3) letting the routing-table fill run one entry
past the last pool only changes entries for sizes that the size-limit check rejects before the table
is read.

Mutation testing also exposed problems in the **test setup**, each of which had made a test pass
without proving anything:
- the emulator's RAM "pre-fill" wrote zeros (a doubled backslash in a `tr` command), and later turned
  out to write `c2 a5` pairs under a UTF-8 locale: it is now a locale-independent `perl` one-liner,
  guarded by a check of the first bytes;
- QEMU's ELF loader zeroes the `.bss` segment itself, which hides a missing `.bss` clear: the firmware
  is now loaded as a raw flash image (`fw.bin`) so only the startup code can clear `.bss`;
- the statistics-reset test never made the counters non-zero before resetting them;
- the spill test never exercised a chain of two empty pools;
- nothing checked that the interrupt mask is restored: five mutants that forget `irq_restore` all
  passed until the emulator test was taught to read PRIMASK back (found while optimising, when
  those paths were rewritten);
- nothing checked that a table that is not strictly ascending is rejected by `pool_init()`.

All were fixed and the runs repeated, under a UTF-8 locale.

## Board bring-up: what was checked on the real board, and what was not

Checked on a Nucleo-F411RE (ST-Link V2J33M25, 3.24 V), with the registers read back over OpenOCD
and the serial output captured from the ST-Link virtual COM port:

| Check | Expected | Seen |
|---|---|---|
| `make APP=blink flash` | writes and verifies | `Verified OK` |
| `RCC_CFGR` | `0x0000100A` | `0x0000100A` |
| `RCC_PLLCFGR` | `0x24001808` | `0x24001808` |
| `FLASH_ACR` | `0x00000703` | `0x00000703` |
| `PWR_CR` / `PWR_CSR` bit 14 (`VOSRDY`) | `0x0000C000` / set | `0x0000C000` / `0x00004000` |
| `USART2_BRR` | `0x000001A1` | `0x000001A1` |
| `DWT_CTRL` bit 0 / `CPACR` | set / `0x00F00000` | `0x40000001` / `0x00F00000` |
| Serial banner | `HCLK = 96000000 Hz`, `PCLK1 = 48000000 Hz` | exactly that, then `tick N` |
| `make APP=selftest flash` | every test ok | 12 tests ok, `SELFTEST PASSED` (also after each optimisation) |
| `make APP=bench flash`, `make APP=bench_malloc flash` | tables | see the results above |

Still not verified:
- the LED blink pattern of `blink` (1 blink, 2 blinks, then one per second) was not observed by
  eye in this round;
- the real accuracy of the 96 MHz clock: the HSI is only +-1 % at 25 C (+-5 % over temperature),
  and the only reference seen is that the 115200-baud text decodes correctly. Nothing was measured
  against an external time base;
- `CLOCK_MHZ=100` was never run;
- the first bytes after every reset are garbage on the serial line; the cause (the ST-Link
  re-applying the baud rate, or a glitch on TX while the pin is configured) was not investigated;
- the benchmark covers a small set of synthetic scenarios (see above), not a real workload;
- the firmware has not been run with interrupts actually firing while the pool is in use.

To repeat the capture on a Mac, the serial port must be held open while its speed is set (a
separate `stty` call is forgotten when it exits): start a reader (`cat /dev/cu.usbmodem*`), run
`stty -f /dev/cu.usbmodem* 115200 cs8 -cstopb -parenb raw -echo` while it is open, then reset the
board (`openocd -f board/st_nucleo_f4.cfg -c "init; reset run; shutdown"`).

## Assumptions and open items

- **Pool sizes are placeholders** (`pool_config.h`: 16/32/64/128/256 B, 11 KB in total). The real
  values come from the real workload: for each kind of object, its size and the most alive at once.
  `pool_get_stats()` reports the observed peak and the failure count to check the choice. If the
  workload is a neural network, its tensors need one large arena in a separate linker section, not
  pools; the pools then serve only small runtime objects.
- **Stack size 4 KB is a guess** (the pool test under the emulator used ~950 B). Measure the real application and adjust
  `_Min_Stack_Size` in `linker.ld`.
- A stack overflow is not detected yet (planned: an MPU guard region).
- The clock sequence follows RM0383 p.45 and the PLL section but has only been checked against the
  manual and by two independent reviewers, not run on the board.
- No constructors run (`-nostartfiles`); `bench_malloc` links a prebuilt newlib-nano.
