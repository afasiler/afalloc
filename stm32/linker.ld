/* Linker script for the STM32F411RE (Nucleo-F411RE): 512 KB flash, 128 KB SRAM.
 *
 * Memory map facts (DS10314 p.54, RM0383 p.40-42), confirmed on the board:
 *   flash 0x0800 0000, 512 KB      SRAM 0x2000 0000, 128 KB
 *   after reset the CPU reads word 0 (initial stack pointer) and word 1
 *   (reset handler) from address 0, which the boot pins alias to flash.
 */

ENTRY(Reset_Handler)

/* Design decision: stack size. 4 KB is a guess until we measure real usage. */
_Min_Stack_Size = 0x1000;

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

/* Top of RAM = initial stack pointer = 0x2002 0000. The stack grows downward. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  /* 1. The vector table must be first in flash (the CPU reads it from address 0).
        KEEP stops --gc-sections from throwing it away: nothing refers to it by name. */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  /* 2. Code and read-only constants stay in flash. */
  .text :
  {
    . = ALIGN(4);
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  /* 3. Initialised globals. They are STORED in flash (AT> FLASH) but LINKED to run in
        RAM, so the startup code must copy them. _sidata is where the copy lives in
        flash, _sdata/_edata are where it must go in RAM. */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  /* 4. Zero-initialised globals. Only exist in RAM; the startup code clears them. */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* 5. Memory for the pools, kept apart so we can see its exact size and bounds.
        NOLOAD: it is not initialised by startup; the pool init code sets it up.
        Declare pool arrays with __attribute__((section(".pool_mem"))). */
  .pool_mem (NOLOAD) :
  {
    . = ALIGN(8);
    _spool = .;
    *(.pool_mem*)
    . = ALIGN(8);
    _epool = .;
  } > RAM

  /* 6. Account for the stack. This section holds no data and does not sit where the stack
        lives (the stack is at the TOP of RAM, growing down from _estack): it only makes the
        linker count _Min_Stack_Size bytes after the pools, so that if data + bss + pools leave
        less than that the link fails with "region RAM overflowed" instead of the program
        corrupting itself at run time. */
  ._stack_reserve (NOLOAD) :
  {
    . = ALIGN(8);
    _sstack_reserve = .;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } > RAM

  /* Lowest address the stack may reach: _Min_Stack_Size below the top of RAM. Nothing
     detects a stack that grows past it (an MPU guard region is the planned fix). */
  _sstack = _estack - _Min_Stack_Size;

  /* Heap for the optional malloc benchmark build only (newlib's malloc, see src/sbrk.c):
     everything from the end of the pools up to the bottom of the stack. The normal builds do
     not use malloc and never reference these symbols. */
  PROVIDE(_sheap = _epool);
  PROVIDE(_eheap = _sstack);

  /* Unwind tables are not used in this C program. */
  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.ARM.extab*)
  }
}
