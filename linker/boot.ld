ENTRY(Reset_Handler)
MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > RAM
 .text : { *(.text*) *(.rodata*) } > RAM
 .ARM.extab : { *(.ARM.extab*) } > RAM
 .ARM.exidx : { *(.ARM.exidx*) } > RAM
 .data : { *(.data*) . = ALIGN(4); _image_end = .; } > RAM
 .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
 _stack_top = ORIGIN(RAM) + LENGTH(RAM);
 ASSERT(_bss_end <= _stack_top - 8K, "Bootloader leaves less than 8 KiB stack")
 ASSERT(_image_end - ORIGIN(RAM) < 16K, "Boot relocation would overwrite its running ROM entry")
 /DISCARD/ : { *(.comment) *(.note*) }
}
