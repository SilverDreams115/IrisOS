# IRIS OS

IRIS OS is a custom x86_64 operating system built from scratch with UEFI, focused on low-level systems development, memory management, interrupt handling, and multitasking.

The project serves as a learning and experimentation platform for progressively building core operating system components, from boot to scheduling and beyond.

---

## Current Status

**Stage 7 - Stable Cooperative Multitasking**

The current  branch includes:

- UEFI boot through a custom bootloader
- ExitBootServices and kernel ownership of the machine
- Physical Memory Manager (PMM) with bitmap-based page tracking
- Virtual memory and paging initialization
- GDT and IDT setup
- PIC remapping and PIT timer initialization
- Exception handling with serial debug output
- Stable cooperative multitasking
- Per-task stack isolation with fixed RSP handling
- Idle task fallback
- Active timer IRQ as groundwork for future preemption

---

## Project Goals

IRIS OS is being developed to progressively implement:

- Boot and firmware handoff
- Physical and virtual memory management
- Interrupt and exception handling
- Task scheduling
- Hardware abstraction foundations
- Future userland support
- Future graphics, drivers, and system services

---

## Repository Structure

boot/  
UEFI bootloader source  

kernel/  
Kernel core, memory management, architecture-specific code, scheduler  

userland/  
Reserved for future user space programs and runtime components  

scripts/  
Helper scripts for running and testing in QEMU  

docs/  
Project notes and technical documentation  

tests/  
Validation and testing assets  

third_party/  
External dependencies and bundled components  

tools/  
Project-specific tooling  

build/  
Generated build artifacts  

---

## Build Requirements

To build and run IRIS OS, you need:

- GCC for x86_64
- GNU-EFI
- QEMU
- Make
- A Linux development environment

---

## Build and Run



---

## Example Runtime Output



---

## Scheduling Model

The current scheduler is **cooperative**, meaning tasks voluntarily yield execution.

- Context switching is stable
- Each task uses its own stack
- Timer interrupt is active but does not enforce preemption yet

---

## Next Steps

- Preemptive multitasking via timer interrupts
- Interrupt frame-based context switching ()
- Improved scheduler logic
- Expansion of kernel subsystems

---

## Branch Strategy

-  -> stable milestones
-  -> validated integration
-  -> active development
-  -> collaboration

---

## Disclaimer

IRIS OS is an experimental operating system project.

- Not production-ready
- No userland yet
- Built for learning and low-level experimentation

---

## Author

Mauricio Mendoza Molina

---

## License

TBD
