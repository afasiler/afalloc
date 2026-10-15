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
 board.c uart.c clock.c dwt.h   LED + delays, serial output, 100 MHz clock, cycle counter
 regs.h           register map, every address/offset taken from DS10314 / RM0383
 startup.c        vector table + Reset_Handler (.data copy, .bss clear, FPU on, call main)
 linker.ld        memory map: flash, RAM, pools, stack reserve
```

## Memory map (what `linker.ld` builds)

```text
FLASH 0x0800 0000  vector table (408 B) | code + constants | copy of .data
RAM   0x2000 0000  .data | .bss | .pool_mem (the pools) | ... free ... | stack <-- 0x2002 0000
```
The pools are static arrays in `.pool_mem`. The linker also reserves 4 KB for the stack: if
data + pools + stack do not fit in 128 KB, the **link** fails ("region RAM overflowed").

## Commands

| Command | What it does |
|---|---|
| `make host-test` | pool unit tests on the Mac, under the address/UB sanitizers |
| `make host-bench-check` | runs the benchmark harness on the Mac with a fake cycle counter (logic check) |
| `make emu-test` | runs the **real firmware startup + pool** on an emulated Cortex-M4 (QEMU) |
| `make all-apps` | builds every app for the board (compile + link check) |
| `make APP=blink flash` | builds and writes an app to the board over SWD |
| `make size APP=...` | flash / RAM usage |

Apps: `blink` (first bring-up), `selftest` (pool tests on the board), `bench` (pool timing),
`bench_malloc` (same timing on newlib-nano malloc). `POOL_DEBUG=0|1` turns the pool's extra
checks on/off (benchmarks default to off).

Serial output: the board's USB cable also carries a serial port (USART2 on PA2/PA3):
`screen /dev/cu.usbmodem* 115200`.

## The pool allocator in one paragraph

Memory is split ahead of time into pools of equally sized blocks (sizes are powers of two,
set in `src/pool_config.h`). `pool_alloc(n)` picks the smallest pool whose blocks fit and pops
the first block off that pool's free list; `pool_free(p)` pushes it back. The link to the next
free block is stored inside the free block itself, so blocks need **no header**. Both calls take
a constant number of steps, there is no search and no fragmentation inside a pool, and the
critical section (interrupts masked) lasts a few instructions, so it is safe from interrupt
handlers. With `POOL_DEBUG=1` a bitmap catches double frees and freed blocks are filled with a
poison pattern so writes after free are detected by `pool_check()`.

## What has been verified, and how

| Claim | Verified by | Where |
|---|---|---|
| Pool logic is correct (routing, exhaustion, reuse, bad/double frees, no overlap, stress) | host unit tests + deliberate-bug checks (each injected bug is caught), several configurations, `-O0/-O1/-O2/-Os`, sanitizers | `make host-test` |
| Same logic on a real 32-bit Cortex-M4 (4-byte pointers, Thumb-2) | QEMU, real startup code and linker script | `make emu-test` |
| Startup copies `.data`, zeroes `.bss`; vector table found; stack works | QEMU test (and it fails if the copy is removed) | `make emu-test` |
| Stack really needed ~250 B for the pool test (4 KB reserved) | stack painting under QEMU | `make emu-test` |
| Register offsets match the manual | `_Static_assert`s against RM0383 offsets | `regs.h` |
| PLL numbers respect every limit in the manual | `_Static_assert`s | `clock.c` |
| Benchmark harness is sound | host run under sanitizers; counts add up | `make host-bench-check` |
| Chip identity, flash size, reset values, LED = PA5 active-high, debug path | read/written on the real board earlier | OpenOCD session |

## NOT yet verified on the board (do these first, in order)

These run only on real hardware. Each has an exact expected value so a mismatch points at the cause.

1. `make APP=blink flash`. Expected LED: **1 blink** (alive, cycle counter works), then **2 blinks**
   (clock now 100 MHz), then 1 blink per second. 5 fast blinks = PLL setup failed (the serial
   log says at which stage); endless triple blink = no cycle counter.
2. Serial: `screen /dev/cu.usbmodem* 115200` shows `HCLK  = 100000000 Hz` and `tick N` lines.
3. Read back the registers (OpenOCD telnet, `nc localhost 4444`) and compare:

   | Register | Address | Expected after clock init |
   |---|---|---|
   | `RCC_CFGR` | `0x40023808` | bits 3:2 = `10` (PLL is the system clock), bits 12:10 = `100` (APB1 /2) |
   | `RCC_PLLCFGR` | `0x40023804` | `0x24001908` (M=8, N=100, P=/2, Q=4, reserved bit kept) |
   | `FLASH_ACR` | `0x40023C00` | `0x00000703` (3 wait states + prefetch + both caches) |
   | `DWT_CTRL` | `0xE0001000` | bit 0 set (cycle counter running) |
   | `CPACR` | `0xE000ED88` | `0x00F00000` (FPU enabled) |
   | `USART2_BRR` | `0x40004408` | `0x01B2` = 434 (50 MHz / 115200) |

   The core addresses (`DWT`, `DEMCR`, `CPACR`) are not in RM0383 (they are in the Arm core
   documentation), so they are the likeliest to need a fix.
4. `make APP=selftest flash`: the serial log should show every test `ok` and the LED stays on.
5. `make APP=bench flash` and `make APP=bench_malloc flash`: collect both tables, compare.

## Assumptions and open items

- **Pool sizes are placeholders** (`pool_config.h`). Real values come from the real workload:
  for each kind of object, its size and the most alive at once. `pool_get_stats()` reports the
  observed peak and the failure count to check the choice.
- **Stack size 4 KB is a guess** (the pool test used ~250 B). Measure the real application and adjust
  `_Min_Stack_Size` in `linker.ld`.
- A stack overflow is not detected yet; the planned guard is an MPU region (the core has 8).
- The clock sequence follows RM0383 p.45 and the PLL section but has only been checked
  against the manual, not run.
