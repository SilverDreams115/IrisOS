# IRIS

IRIS is a custom operating system focused on development and gaming.

## Vision

IRIS is being built as a unified digital environment where software development, system-level control, and gaming can coexist inside a single platform designed from the ground up.

This project is not intended to be a generic office-oriented operating system. Its long-term direction is centered around:

- low-level control
- modular architecture
- graphics and hardware compatibility
- developer-oriented workflows
- gaming-oriented runtime design
- unified authentication and system control

## Current Technical Direction

- **Architecture:** x86_64
- **Boot model:** UEFI
- **Kernel model:** high-performance modular hybrid design
- **Primary focus:** developers and gamers
- **Authentication model:** one master password per user, unified across login, terminal authentication, and privileged actions
- **Hardware ambition:** broad GPU compatibility across major vendors

## Current Status

IRIS is currently in early bootstrap development.

### Validated milestones

#### Stage 0
- UEFI bootstrap successfully validated in QEMU + OVMF
- EFI entry point executes correctly
- early kernel handoff works

#### Stage 1
- loader and kernel were separated into independent EFI images
- `BOOTX64.EFI` works as the loader
- `KERNELX64.EFI` works as a separate kernel image
- the loader successfully loads and starts the kernel in QEMU + OVMF

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

## Branch Workflow

IRIS uses a simplified branch model:

- `main` → stable and validated state
- `staging` → integration and testing
- `silver` → primary development branch
- `collab` → collaborator branch

## Short-Term Roadmap

### Next milestone
Move from a kernel EFI image to a real kernel loading flow based on ELF.

### Near-term goals
- keep `BOOTX64.EFI` as the loader
- replace the EFI-style kernel image with a true ELF kernel
- implement ELF parsing and segment loading
- transfer execution to the kernel entry point without returning to firmware
- begin building the real kernel execution model

## Development Notes

At this stage, IRIS is still focused on foundation work:

- boot process
- loader behavior
- kernel separation
- repository organization
- documentation and workflow discipline

This is the base layer for the future system.

