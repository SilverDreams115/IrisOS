# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, multitasking, IPC, framebuffer graphics, filesystem abstractions, userland execution, syscall interface, hardware enumeration, and kernel stability.

---

## Current Status

**Stage 13 + Stability Fixes Batch 1**

The kernel has completed PCI enumeration and a full stability pass covering memory isolation, syscall hardening, scheduler architecture, and virtual address space formalization.

### Completed stages

| Stage | Component | Description |
|-------|-----------|-------------|
| 0-2 | UEFI Bootstrap | Loader parses ELF kernel, passes `iris_boot_info`, jumps to kernel |
| 3 | ExitBootServices | Firmware OFF, kernel owns machine, full memory map passed |
| 4 | PMM | Bitmap allocator, 503 MB tracked at 4K granularity, `pmm_alloc_pages(n)` |
| 5 | Paging | 4-level x86_64 paging, 2MB huge pages, kernel/user separation |
| 6 | GDT + IDT | 7-entry GDT with TSS, 256-entry IDT, exception handlers |
| 7 | Scheduler | Round-robin cooperative scheduler, PIT at 100Hz, per-task quantum |
| 8 | IPC | Circular buffer channels, blocking recv, producer/consumer validated |
| 9 | Framebuffer | Direct pixel writes to UEFI GOP framebuffer, `fb_fill`, `fb_draw_rect` |
| 10 | VFS + ramfs | Virtual filesystem, in-memory ramfs, open/read/write/close/stat/seek/mkdir |
| 11 | Userland | TSS, kernel stack per task, `iretq` trampoline, ring 3 user_init process |
| 12 | Syscall | MSR LSTAR/STAR/SFMASK, per-task kernel stack, SYS_WRITE/EXIT/GETPID/YIELD/OPEN/READ/CLOSE/BRK/SLEEP |
| 13 | PCI | Bus enumeration via config space, device table, class detection |

### Stability fixes applied (post-Stage 13)

| Fix | Description |
|-----|-------------|
| Build | Unified assembler flags — `KASM_FLAGS` → `KERNEL_ASFLAGS` throughout Makefile |
| Security | Ring 3 RFLAGS: lowered IOPL from 3 to 0 (`0x3202` → `0x0202`) |
| Security | Closed IOPB: `iopb_offset = sizeof(tss)` — no direct I/O ports from ring 3 |
| MM | Each ring 3 task gets its own CR3 via `paging_create_user_space()` |
| MM | `PAGE_USER` removed from identity map, framebuffer, and intermediate tables |
| MM | `PAGE_USER` now propagates only from leaf flags — intermediate tables follow the leaf |
| MM | User stack mapped at `USER_STACK_TOP` in process CR3 — no longer aliased to kernel BSS |
| MM | `ustack[]` removed from `struct task` — replaced by virtual stack metadata fields |
| MM | Fix 13: formal virtual address space layout with named constants in `paging.h` |
| MM | Linker script exports `__text_start/end`, `__rodata_start/end`, `__data_start/end`, `__bss_start/end` |
| Sched | Documented as cooperative-only — IRQ0 increments ticks but does not force preemption |
| Sched | Per-task `time_slice`, `ticks_left`, `need_resched` fields added |
| Sched | Quantum reloaded on `task_yield()` |
| Syscall | Per-task kernel stack for syscalls — `syscall_set_kstack()` called on every context switch |
| Syscall | `syscall_entry.S` rewritten — clean save/restore frame, no duplicate offset blocks |
| Syscall | User pointer validation checks page table presence via `paging_virt_to_phys()` |
| Syscall | Added SYS_YIELD(3), SYS_OPEN(4), SYS_READ(5), SYS_CLOSE(6), SYS_BRK(7), SYS_SLEEP(8) |

---

## Architecture

- **Target:** x86_64, UEFI boot (BOOTX64.EFI)
- **Kernel type:** Microkernel-oriented
- **Memory model:** 4-level paging, kernel at `KERNEL_VIRT_BASE + 0x200000`, framebuffer identity-mapped kernel-only
- **Scheduler:** Cooperative (IRQ0 ticks only, no forced preemption yet), round-robin, per-task quantum (10 ticks = 100ms at 100Hz)
- **IPC:** In-kernel circular buffer channels
- **Framebuffer:** UEFI GOP, `pixels_per_scanline` stride, ARGB, kernel-only access
- **VFS:** POSIX-inspired API backed by ramfs, supports files and directories
- **Syscall:** MSR-based (`syscall`/`sysretq`), per-task kernel stack, validated user pointers, 9 syscalls
- **PCI:** Config space enumeration (ports 0xCF8/0xCFC), multi-function support, class detection

---

## Virtual Address Space Layout
```
0x0000000000001000  user text start (identity-mapped)
0x0000000000200000  user text/data (kernel ELF physical base)
0x0000000000400000  user heap base  (SYS_BRK)
0x0000000000600000  user heap max
0x000000007FFF7000  user stack base (32 KB, grows down)
0x000000007FFFF000  user stack top

0xFFFFFFFF80200000  kernel text (.text)
0xFFFFFFFF80205000  kernel data (.data/.bss)

MMIO/framebuffer    identity-mapped, kernel-only (no PAGE_USER)
```

---

## Repository Structure
```
boot/uefi/boot.c                          UEFI loader
kernel/kernel_main.c                      Kernel entry, subsystem init
kernel/include/iris/                      All kernel headers
kernel/include/iris/paging.h             Virtual address space constants + page flags
kernel/mm/pmm/pmm.c                       Bitmap PMM + pmm_alloc_pages(n)
kernel/arch/x86_64/paging.c              4-level paging, user space isolation, paging_map_in()
kernel/arch/x86_64/linker.ld             Kernel ELF layout with exported section symbols
kernel/arch/x86_64/gdt.c                 GDT + TSS (IOPB closed)
kernel/arch/x86_64/idt.c                 IDT + exception/IRQ dispatch
kernel/arch/x86_64/pic.c                 PIC remap + PIT
kernel/arch/x86_64/syscall_entry.S       MSR syscall handler, per-task kernel stack
kernel/arch/x86_64/user_trampoline.S     iretq trampoline to ring 3
kernel/arch/x86_64/user_init.S           First user-space process
kernel/arch/x86_64/context_switch.S      context_switch(old, new)
kernel/core/scheduler/scheduler.c        Cooperative round-robin scheduler, per-task quantum
kernel/core/ipc/ipc.c                    IPC channels
kernel/core/syscall/syscall.c            Syscall dispatcher + user ptr validation
kernel/drivers/fb/fb.c                   Framebuffer driver
kernel/drivers/serial/serial.c           Serial output (hex8/hex16/hex64/dec)
kernel/drivers/pci/pci.c                 PCI bus enumeration + device table
kernel/drivers/keyboard/keyboard.c       PS/2 keyboard driver (IRQ1, scancode→ASCII)
kernel/fs/ramfs/ramfs.c                  In-memory filesystem
kernel/fs/ramfs/vfs.c                    VFS layer
```

---

## Build & Run
```bash
make clean && make && make run
```

---

## Syscall Interface

| Number | Name | Args | Description |
|--------|------|------|-------------|
| 0 | SYS_WRITE | rdi=str_ptr | Print string to serial (validated ptr) |
| 1 | SYS_EXIT | rdi=code | Terminate process |
| 2 | SYS_GETPID | — | Return current task id |
| 3 | SYS_YIELD | — | Yield CPU to next task |
| 4 | SYS_OPEN | rdi=path, rsi=flags | Open file, return fd |
| 5 | SYS_READ | rdi=fd, rsi=buf, rdx=len | Read from fd |
| 6 | SYS_CLOSE | rdi=fd | Close fd |
| 7 | SYS_BRK | rdi=new_brk | Set heap break (0 = query) |
| 8 | SYS_SLEEP | rdi=ticks | Yield for N ticks (100Hz) |

---

## PCI Device Table (QEMU)

| Bus:Dev.Func | Vendor | Device | Class |
|-------------|--------|--------|-------|
| 0:0.0 | 0x8086 (Intel) | 0x29C0 | Host Bridge |
| 0:1.0 | 0x1234 (QEMU) | 0x1111 | VGA Display |
| 0:2.0 | 0x8086 (Intel) | 0x10D3 | e1000 Network |
| 0:31.0 | 0x8086 (Intel) | 0x2918 | ICH9 LPC Bridge |

---

## Roadmap
```
Stage 14  PS/2 keyboard driver (IRQ1, in progress)
Stage 15  Interactive shell
Stage 16  W^X memory permissions (PAGE_NX per segment)
Stage 17  Preemptive scheduler (need_resched + forced yield on IRQ exit)
```

---

## Branch Strategy

| Branch | Purpose |
|--------|---------|
| `silver` | Active development |
| `staging` | Integration + testing |
| `main` | Stable releases |
| `collab` | External contributors |
