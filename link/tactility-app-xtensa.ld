/* The link script for a Tactility device app on an Xtensa chip (ESP32-S3).
 *
 * Binutils' own script for -shared (elf32xtensa.xs, from the ESP-IDF 6.1
 * toolchain) with one purpose: keep everything an app needs inside the five
 * sections elf_loader loads. On the S3 the loader works section by section and
 * maps .text, .rodata, .data.rel.ro, .data and .bss only; any other section is
 * dropped without a word. So:
 *
 *   - the static constructor tables (.ctors, which this toolchain emits, and
 *     .init_array/.preinit_array, which others do) go at the start of
 *     .data.rel.ro, between __deki_ctors_start/__deki_ctors_end and
 *     __deki_init_array_start/__deki_init_array_end. The loader runs none of
 *     them; the app's main() does (TactilityRuntime.cpp).
 *   - .rodata1 joins .rodata, .data1 and .lit4 join .data.
 *   - .dtors and .fini_array are discarded: destructors are registered through
 *     __cxa_atexit, which the app defines itself.
 *
 * Copyright (C) 2014-2025 Free Software Foundation, Inc.
 * Copying and distribution of this script, with or without modification,
 * are permitted in any medium without royalty provided the copyright
 * notice and this notice are preserved.
 */
ENTRY(_start)
SECTIONS
{
  . = 0 + SIZEOF_HEADERS;
  .note.gnu.build-id  : { *(.note.gnu.build-id) }
  .hash           : { *(.hash) }
  .gnu.hash       : { *(.gnu.hash) }
  .dynsym         : { *(.dynsym) }
  .dynstr         : { *(.dynstr) }
  .gnu.version    : { *(.gnu.version) }
  .gnu.version_d  : { *(.gnu.version_d) }
  .gnu.version_r  : { *(.gnu.version_r) }
  .rela.init      : { *(.rela.init) }
  .rela.text      : { *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*) }
  .rela.fini      : { *(.rela.fini) }
  .rela.rodata    : { *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*) }
  .rela.data.rel.ro   : { *(.rela.data.rel.ro .rela.data.rel.ro.* .rela.gnu.linkonce.d.rel.ro.*) }
  .rela.data      : { *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*) }
  .rela.ctors     : { *(.rela.ctors) }
  .rela.got       : { *(.rela.got) }
  .rela.bss       : { *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*) }
  .rela.plt       : { *(.rela.plt) }
  .text           :
  {
    *(.got.plt* .plt*)
    KEEP (*(.init.literal))
    KEEP (*(SORT_NONE(.init)))
    *(.literal .text .stub .literal.* .text.* .gnu.linkonce.literal.* .gnu.linkonce.t.*.literal .gnu.linkonce.t.*)
    *(.gnu.warning)
    KEEP (*(.fini.literal))
    KEEP (*(SORT_NONE(.fini)))
  } =0
  .rodata         : { *(.rodata .rodata.* .gnu.linkonce.r.*) *(.rodata1) }
  .got.loc        : { *(.got.loc) }
  . = ALIGN (CONSTANT (MAXPAGESIZE)) - ((CONSTANT (MAXPAGESIZE) - .) & (CONSTANT (MAXPAGESIZE) - 1)); . = DATA_SEGMENT_ALIGN (CONSTANT (MAXPAGESIZE), CONSTANT (COMMONPAGESIZE));
  .data.rel.ro    :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__deki_ctors_start = .);
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
    PROVIDE_HIDDEN (__deki_ctors_end = .);
    PROVIDE_HIDDEN (__deki_init_array_start = .);
    KEEP (*(.preinit_array))
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array))
    PROVIDE_HIDDEN (__deki_init_array_end = .);
    *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*) *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*)
  }
  .dynamic        : { *(.dynamic) }
  . = DATA_SEGMENT_RELRO_END (0, .);
  .got            : { *(.got) }
  .data           :
  {
    *(.data .data.* .gnu.linkonce.d.*)
    SORT(CONSTRUCTORS)
    *(.data1)
    *(.lit4 .lit4.* .gnu.linkonce.lit4.*)
  }
  _edata = .; PROVIDE (edata = .);
  . = ALIGN(ALIGNOF(NEXT_SECTION));
  __bss_start = .;
  .bss            :
  {
    *(.dynbss)
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(. != 0 ? 32 / 8 : 1);
  }
  . = ALIGN(32 / 8);
  _end = .; PROVIDE (end = .);
  . = DATA_SEGMENT_END (.);
  .comment 0 (INFO) : { *(.comment); LINKER_VERSION; }
  .xtensa.info : { *(.xtensa.info) }
  .xt.lit       0 : { KEEP (*(.xt.lit .xt.lit.* .gnu.linkonce.p.*)) }
  .xt.insn      0 : { KEEP (*(.xt.insn .gnu.linkonce.x.*)) }
  .xt.prop      0 : { KEEP (*(.xt.prop .xt.prop.* .gnu.linkonce.prop.*)) }
  /DISCARD/ : { *(.dtors) *(.dtors.*) *(.fini_array) *(.fini_array.*) *(.note.GNU-stack) *(.gnu_debuglink) *(.gnu.lto_*) }
}
