# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, multitasking, IPC, and framebuffer graphics.

The project serves as a learning and experimentation platform for progressively building core operating system components, from boot to pixel output.

---

## Current Status

**Stage 9 - Framebuffer Driver**

The kernel boots fully under UEFI, owns the machine after ExitBootServices, and now renders directly to the screen. The IPC subsystem and cooperative/preemptive scheduler run concurrently with framebuffer output.

### Completed stages

| Stage | Component | Description |
|-------|-----------|-------------|
| 0-2 | UEFI Bootstrap | Loader parses ELF kernel, passes `iris_boot_info`, jumps to kernel |
| 3 | ExitBootServices | Firmware OFF, kernel owns machine, full memory map passed |
| 4 | PMM | Bitmap allocator, 503 MB tracked at 4K granularity |
| 5 | Paging | 4-level x86_64 paging, 2MB huge pages, kernel higher half mapped |
| 6 | GDT + IDT | 5-entry GDT, 256-entry IDT, exception handlers with serial output |
| 7 | Scheduler | Round-robin cooperative+preemptive scheduler, PIT at 100Hz |
| 8 | IPC | Circular buffer channels, blocking recv, producer/consumer validated |
| 9 | Framebuffer | Direct pixel writes to UEFI GOP framebuffer, `fb_fill`, `fb_draw_rect` |

---

## Architecture

- **Target:** x86_64, UEFI boot (BOOTX64.EFI)
- **Kernel type:** Microkernel-oriented
- **Memory model:** 4-level paging, kernel at `0xFFFFFFFF80200000`, framebuffer at `0x80000000`
- **Scheduler:** Cooperative + preemptive (PIT IRQ0 at 100Hz), round-robin
- **IPC:** In-kernel circular buffer channels with blocking `ipc_recv` and non-blocking `ipc_send`
- **Framebuffer:** UEFI GOP base mapped at boot, `pixels_per_scanline` stride, ARGB pixel format

---

## Repository Structure
```
boot/uefi/boot.c                         UEFI loader (silent, passes iris_boot_info)
kernel/kernel_main.c                     Kernel entry, subsystem init, IPC + FB demo
kernel/include/iris/                     All kernel headers
kernel/mm/pmm/pmm.c                      Bitmap PMM
kernel/arch/x86_64/paging.c             4-level paging + huge pages
kernel/arch/x86_64/gdt.c                GDT setup
kernel/arch/x86_64/idt.c                IDT + exception/IRQ dispatch
kernel/arch/x86_64/pic.c                PIC remap + PIT
kernel/arch/x86_64/isr_stubs.S          32 ISR stubs + IRQ0
kernel/arch/x86_64/context_switch.S     context_switch(old, new)
kernel/core/scheduler/scheduler.c       Round-robin scheduler
kernel/core/ipc/ipc.c                   IPC channels
kernel/drivers/fb/fb.c                  Framebuffer driver
```

---

## Build & Run

**Requirements:** gcc, ld, gnu-efi, OVMF, QEMU with GTK display
```bash
make clean && make && make run
```

QEMU launches with a GTK window showing the framebuffer. Serial output goes to stdout.

---

## Stage 9 Demo

On boot, the kernel:
1. Initializes PMM, paging, GDT, IDT, PIC, PIT
2. Calls `fb_init()` and paints 7 color stripes across the full screen
3. Creates two IPC tasks (producer + consumer) that exchange messages indefinitely

The framebuffer and IPC run simultaneously, validating that graphics and scheduling coexist correctly.

---

## Roadmap
```
Stage 10  VFS + ramfs
Stage 11  Userland init process
Stage 12  Syscall interface
Stage 13  Driver model (PCI enumeration)
Stage 14  GPU / Audio / Input drivers
```

---

## Branch Strategy

| Branch | Purpose |
|--------|---------|
| `silver` | Active development |
| `staging` | Integration + testing |
| `main` | Stable releases |
| `collab` | External contributors |
