# kernel/drivers — Estado mixto

**Único driver real:** `serial/serial.c` — driver UART 16550 para debug serial.
Correctamente en el kernel porque la consola de debug precede cualquier servidor.

**Todos los demás subdirectorios están vacíos (solo `.gitkeep`):**

- `acpi/` — No implementado. ACPI parsing requeriría una tabla AML completa.
  En microkernel correcto, ACPI lo maneja un servidor de userland. Por ahora
  el boot usa información del bootloader (UEFI) directamente.
- `audio/` — No implementado. Fuera de scope para Phase 0-2.
- `dma/` — No implementado. No hay dispositivos DMA activos en el kernel hoy.
- `framebuffer/` — El framebuffer lo gestiona el servicio `services/fb/`.
  No debe haber driver de framebuffer en el kernel excepto la inicialización
  de la dirección base (pasada por el bootloader).
- `gpu/` (amd/, core/, intel/, nvidia/) — No implementado. GPU es userland,
  siempre. Placeholder prematuro que da impresión falsa de soporte GPU.
- `input/` — No implementado. El teclado PS/2 está en `services/kbd/`.
- `keyboard/` — Duplicado de `input/`. Ambos vacíos.
- `pci/` — No implementado. PCI enumeration es necesaria antes de cualquier
  driver PCIe. Candidato real para Phase 2, pero solo si hay dispositivos
  PCIe que el kernel deba manejar directamente (raro en microkernel).

**Decisión Phase 0:** No agregar ningún driver nuevo. Solo `serial.c` permanece.

**Riesgo:** La presencia de `gpu/nvidia/` etc. crea expectativa falsa de soporte.
Propuesta para Phase 1: eliminar subdirectorios de drivers que no se planean
en los próximos 3 fases del roadmap.
