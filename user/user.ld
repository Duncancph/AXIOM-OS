ENTRY(_start)
SECTIONS
{
    . = 0x8000000000;
    .text : { *(.text .text.*) }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data : ALIGN(4K) { *(.data .data.*) }
    .bss : ALIGN(4K) { *(COMMON) *(.bss .bss.*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
