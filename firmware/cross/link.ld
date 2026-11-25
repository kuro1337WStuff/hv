OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(cross_main)

PHDRS {
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0xffffffff80000000;
    .text : { *(.text .text.*) } :text
    .rodata ALIGN(4096) : { *(.rodata .rodata.*) } :rodata

    /* Limine writes response pointers here before entering cross_main. */
    .limine_requests_start ALIGN(4096) : { KEEP(*(.limine_requests_start)) } :data
    .limine_requests : { KEEP(*(.limine_requests)) } :data
    .limine_requests_end : { KEEP(*(.limine_requests_end)) } :data
    .data : { *(.data .data.*) } :data
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : { *(.eh_frame* .note* .comment) }
}
