OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);   /* R-X */
  data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
  . = 0x200000;
  __kernel_start = .;

  .text : ALIGN(0x1000)
  {
    __text_start = .;
    *(.text*)
    __text_end = .;
  } :text

  .rodata : ALIGN(0x1000)
  {
    __rodata_start = .;
    *(.rodata*)
    __rodata_end = .;
  } :text

  .data : ALIGN(0x1000)
  {
    __data_start = .;
    *(.data*)
    __data_end = .;
  } :data

  .bss : ALIGN(0x1000)
  {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    __bss_end = .;
  } :data

  . = ALIGN(0x1000);
  __kernel_end = .;

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
