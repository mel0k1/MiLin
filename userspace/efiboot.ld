/* MiLin efiboot (M22): PE32+ образ фиксированной геометрии.
 * Заголовки (секция .pe из efiboot.c) — первые 512 байт файла,
 * тело (.flat) — с LMA 0x200, VA = ImageBase 0x100000 + 0x200.
 * .sbat (M22, revocation-метаданные) — с LMA 0x10000, сразу за .flat.
 * Хвост добивается нулями: SizeOfImage зашит в PE-заголовке константой
 * (0x10000 + SBAT_RAW) и не зависит от линковки. */
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
  . = 0x100000;

  .pe 0x100000 : AT(0) {
    *(.pe)
  }

  .flat 0x100200 : AT(0x200) {
    *(.text.start)
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    *(.bss .bss.*) *(COMMON)
    . = 0xFE00;   /* относительно начала секции: тело ровно 0xFE00 */
  }

  .sbat 0x110000 : AT(0x10000) {
    *(.sbat)
    . = ALIGN(0x20);   /* rawsize в PE-заголовке выровнен — файл совпадает */
  }

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.*) *(.rela*) }
}
