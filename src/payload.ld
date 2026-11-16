ENTRY(carrier_entry)

SECTIONS
{
    . = IMAGE_BASE;
    .text : {
        __image_start = .;
        KEEP(*(.manifest))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4096);
        __image_end = .;
    }
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) *(.gnu*) *(.ARM.exidx*) *(.ARM.attributes) }
}
