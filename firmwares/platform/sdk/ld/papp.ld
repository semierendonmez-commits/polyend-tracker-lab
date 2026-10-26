/* PLATFORM native app link script (tools/mkpapp.py turns the ELF into a .papp).
 * Main segment linked at 0: code, read-only data, constructors, data, bss --
 * one contiguous block the loader places anywhere in the SDRAM. The optional
 * .fast segment (PLAT_FAST functions) is linked at 0x40000000 so mkpapp.py can
 * tell the two apart; the loader copies it to SRAM_L when it fits. */
ENTRY(plat_app_entry)
SECTIONS
{
    . = 0;
    .text : ALIGN(8) {
        KEEP(*(.text.plat_app_entry))
        *(.text .text.*)
        KEEP(*(.rodata.plat_app_info))
        *(.rodata .rodata.*)
        . = ALIGN(4);
        __plat_init_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        __plat_init_end = .;
        *(.data .data.*)
        . = ALIGN(8);
    }
    .bss (NOLOAD) : ALIGN(8) {
        __plat_bss_start = .;
        *(.bss .bss.*)
        *(.sdram_bss .sdram_bss.*)   /* a station's OT_SDRAM_BSS data: all app memory is SDRAM */
        *(.noinit .noinit.*)
        *(COMMON)
        . = ALIGN(8);
        __plat_bss_end = .;
    }
    .fast 0x40000000 : ALIGN(8) {
        *(.fast .fast.*)
        *(.fastrun .fastrun.*)   /* OT_FASTRUN_FN code of a station (src/core/fastrun.h) */
        . = ALIGN(4);
    }
    /DISCARD/ : {
        *(.ARM.exidx*) *(.ARM.extab*) *(.fini_array*) *(.preinit_array*)
        *(.comment) *(.note*) *(.ARM.attributes)
    }
}
