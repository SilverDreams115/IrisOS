# IRIS

IRIS is a custom operating system built from scratch for development and gaming.

## Vision

IRIS is designed as a unified digital environment where low-level system control, software development, and gaming can coexist inside a single platform.

This project is not intended to be a generic office-oriented operating system. Its direction is centered around:

- low-level control
- modular architecture
- custom boot and kernel flow
- developer-focused workflows
- gaming-oriented runtime design
- broad long-term hardware ambition
- unified authentication and system control

## Core Goals

- Build a real operating system from the ground up
- Maintain a clean separation between bootloader and kernel
- Evolve toward a high-performance modular hybrid kernel design
- Support a unified environment for developers and gamers
- Keep authentication simple and consistent with a single master password per user
- Build the system with long-term extensibility in mind

## Current Technical Direction

- **Architecture:** x86_64
- **Boot model:** UEFI
- **Kernel format:** ELF
- **Kernel model:** modular hybrid design
- **Primary focus:** developers and gamers
- **Authentication model:** one master password per user
- **Branch workflow:** main / staging / silver / collab

## Current Status

IRIS is currently in the early bootstrap and kernel-loading phase.

### Validated Milestones

#### Stage 0
- UEFI bootstrap validated in QEMU + OVMF
- EFI entry point executes correctly
- early kernel handoff works

#### Stage 1
- loader and kernel were separated into independent EFI images
- `BOOTX64.EFI` works as the loader
- `KERNELX64.EFI` works as a separate kernel image
- the loader successfully loads and starts the kernel in QEMU + OVMF

#### Stage 2
- the kernel was moved from an EFI image to a real ELF executable
- the UEFI loader now reads and parses the ELF kernel
- loadable ELF segments are copied into memory correctly
- a basic `iris_boot_info` structure is passed to the kernel
- the ELF kernel executes successfully in QEMU + OVMF
- the kernel no longer returns control as part of the normal execution path

## Repository Structure

boot/  
kernel/  
userland/  
drivers/  
tools/  
scripts/  
build/  
docs/  
tests/

## Build Capabilities

The current build system supports:

- building the UEFI loader
- building the ELF kernel
- staging files into the EFI filesystem layout
- running IRIS in QEMU with OVMF
- inspecting the ELF kernel with `make check`

## Branch Workflow

IRIS uses a simplified branch model:

- `main` → stable and validated state
- `staging` → integration and testing
- `silver` → primary development branch
- `collab` → collaborator branch

## Current Execution Flow

1. UEFI starts `BOOTX64.EFI`
2. the loader locates and reads `KERNEL.ELF`
3. ELF loadable segments are placed in memory
4. the loader prepares a basic `iris_boot_info` structure
5. control is transferred to the kernel entry point
6. the kernel initializes and enters its execution loop

## Next Milestone

### Stage 3
The next major step is to move from firmware-assisted execution to real kernel ownership of the machine.

Planned goals:

- retrieve the UEFI memory map
- store memory map data inside `iris_boot_info`
- obtain the correct `MapKey`
- call `ExitBootServices()`
- transfer control to the kernel without active firmware services
- prepare the base for real kernel-side memory management

## Development Notes

At this stage, IRIS is focused on foundation work:

- boot flow
- loader behavior
- ELF kernel loading
- boot protocol design
- repository structure
- build discipline
- early kernel execution

This is the base layer for the future system.

