# IRIS

IRIS is a custom operating system built from scratch for development and gaming.

## Vision

IRIS is designed as a unified digital environment where low-level system control, software development, and gaming coexist inside a single platform.

This project is not a generic office-oriented OS. Its direction is centered around:

- low-level control and hardware ownership
- modular microkernel architecture
- custom boot and kernel flow
- developer-focused workflows
- gaming-oriented runtime design
- broad long-term hardware ambition
- unified authentication and system control

## Core Goals

- Build a real operating system from the ground up
- Maintain a clean separation between bootloader and kernel
- Evolve toward a high-performance modular microkernel design
- Support a unified environment for developers and gamers
- Keep authentication simple and consistent with a single master password per user
- Build the system with long-term extensibility in mind

## Current Technical Direction

- **Architecture:** x86_64
- **Boot model:** UEFI
- **Kernel format:** ELF
- **Kernel model:** modular microkernel
- **Primary focus:** developers and gamers
- **Authentication model:** one master password per user
- **Branch workflow:** main / staging / silver / collab

## Current Status

IRIS has completed its foundation boot and memory stages.

### Validated Milestones

#### Stage 0
- UEFI bootstrap validated in QEMU + OVMF
- EFI entry point executes correctly
- early kernel handoff works

#### Stage 1
- loader and kernel separated into independent EFI images
- `BOOTX64.EFI` works as the loader
- `KERNELX64.EFI` works as a separate kernel image
- loader successfully loads and starts the kernel in QEMU + OVMF

#### Stage 2
- kernel moved from EFI image to a real ELF executable
- UEFI loader reads and parses the ELF kernel
- loadable ELF segments copied into memory correctly
- basic `iris_boot_info` structure passed to the kernel
- ELF kernel executes successfully in QEMU + OVMF

#### Stage 3
- UEFI memory map retrieved and stored in `iris_boot_info`
- `ExitBootServices()` called successfully with correct `MapKey`
- kernel owns the machine without active firmware services
- 505 MB of RAM mapped and reported at boot

#### Stage 4
- Physical Memory Manager (PMM) implemented as a bitmap allocator
- 503 MB of usable RAM tracked across 128,943 pages
- `pmm_alloc_page()` and `pmm_free_page()` validated
- kernel correctly marks its own pages as reserved

#### Stage 5
- x86_64 paging initialized with 4-level page tables (PML4)
- identity map of first 64 MB using 2 MB huge pages
- kernel mapped in higher half: physical `0x200000` → virtual `0xFFFFFFFF80200000`
- framebuffer mapped for future display output
- CR3 loaded — virtual memory fully active
- `boot_info` safely copied to kernel BSS before paging activation

## Repository Structure
```
boot/          UEFI loader
kernel/
  arch/x86_64/ architecture-specific code (paging, entry, linker)
  core/        future: ipc, scheduler, panic, tasks
  drivers/     future: gpu, audio, input, pci, serial
  fs/          future: vfs, ramfs, irisfs
  include/iris/ kernel headers
  lib/         future: string, printf, bitmap, elf
  mm/          memory management (pmm, future vmm, heap)
  security/    future: auth, capabilities, audit
  uefi/        (unused — kept for reference)
userland/      future: shell, libc, services, apps
tools/         build and image tools
scripts/       run_qemu.sh and helpers
docs/          architecture, roadmap, driver compat, security
tests/         kernel, driver, fs, security test stubs
build/         compiled output (gitignored)
```

## Build
```bash
make        # build loader + kernel
make run    # launch in QEMU with OVMF
make check  # inspect ELF headers and segments
make clean  # remove build artifacts
```

**Requirements:** `gcc`, `ld`, `objcopy`, `gnu-efi`, `qemu-system-x86_64`, `ovmf`

## Branch Workflow

- `main` → stable and validated state
- `staging` → integration and testing
- `silver` → primary development branch
- `collab` → collaborator branch

## Current Execution Flow

1. UEFI starts `BOOTX64.EFI`
2. loader reads and parses `KERNEL.ELF`
3. ELF loadable segments placed in memory at `0x200000`
4. UEFI memory map retrieved into `iris_boot_info`
5. `ExitBootServices()` called — firmware disabled
6. control transferred to kernel entry point
7. kernel copies `boot_info` to BSS
8. PMM initialized — physical memory tracked
9. paging initialized — virtual memory active
10. kernel halts — awaiting Stage 6 (GDT + IDT)

## Next Milestone

### Stage 6 — GDT + IDT

- install kernel-owned Global Descriptor Table
- install Interrupt Descriptor Table with real ISRs
- handle CPU exceptions: page fault, double fault, GPF
- base for scheduler and microkernel IPC

## About

No description, website, or topics provided.
