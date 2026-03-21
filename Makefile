SHELL := /usr/bin/env bash

BUILD_DIR    := build
EFI_ROOT     := $(BUILD_DIR)/efi_root
EFI_BOOT_DIR := $(EFI_ROOT)/EFI/BOOT
EFI_IRIS_DIR := $(EFI_ROOT)/EFI/IRIS

LOADER_OBJ           := $(BUILD_DIR)/boot_loader.o
BOOT_SO              := $(BUILD_DIR)/BOOTX64.so
BOOT_APP             := $(EFI_BOOT_DIR)/BOOTX64.EFI

KERNEL_ENTRY_OBJ     := $(BUILD_DIR)/kernel_entry.o
KERNEL_MAIN_OBJ      := $(BUILD_DIR)/kernel_main.o
KERNEL_PMM_OBJ       := $(BUILD_DIR)/pmm.o
KERNEL_PAGING_OBJ    := $(BUILD_DIR)/paging.o
KERNEL_GDT_OBJ       := $(BUILD_DIR)/gdt.o
KERNEL_IDT_OBJ       := $(BUILD_DIR)/idt.o
KERNEL_PIC_OBJ       := $(BUILD_DIR)/pic.o
KERNEL_GDT_FLUSH_OBJ := $(BUILD_DIR)/gdt_idt_flush.o
KERNEL_ISR_OBJ       := $(BUILD_DIR)/isr_stubs.o
KERNEL_CTX_OBJ       := $(BUILD_DIR)/context_switch.o
KERNEL_SCHED_OBJ     := $(BUILD_DIR)/scheduler.o
KERNEL_IPC_OBJ       := $(BUILD_DIR)/ipc.o
KERNEL_FB_OBJ        := $(BUILD_DIR)/fb.o
KERNEL_RAMFS_OBJ     := $(BUILD_DIR)/ramfs.o
KERNEL_VFS_OBJ       := $(BUILD_DIR)/vfs.o
KERNEL_TRAMP_OBJ     := $(BUILD_DIR)/user_trampoline.o
KERNEL_USERINIT_OBJ  := $(BUILD_DIR)/user_init.o
KERNEL_SYSCALL_OBJ   := $(BUILD_DIR)/syscall.o
KERNEL_SYSCALLE_OBJ  := $(BUILD_DIR)/syscall_entry.o
KERNEL_SERIAL_OBJ    := $(BUILD_DIR)/serial.o
KERNEL_PCI_OBJ       := $(BUILD_DIR)/pci.o
KERNEL_OBJS := $(KERNEL_ENTRY_OBJ) $(KERNEL_MAIN_OBJ) $(KERNEL_PMM_OBJ) $(KERNEL_PAGING_OBJ) $(KERNEL_GDT_OBJ) $(KERNEL_IDT_OBJ) $(KERNEL_PIC_OBJ) $(KERNEL_GDT_FLUSH_OBJ) $(KERNEL_ISR_OBJ) $(KERNEL_CTX_OBJ) $(KERNEL_SCHED_OBJ) $(KERNEL_IPC_OBJ) $(KERNEL_FB_OBJ) $(KERNEL_RAMFS_OBJ) $(KERNEL_VFS_OBJ) $(KERNEL_TRAMP_OBJ) $(KERNEL_USERINIT_OBJ) $(KERNEL_SYSCALL_OBJ) $(KERNEL_SYSCALLE_OBJ) $(KERNEL_SERIAL_OBJ) $(KERNEL_PCI_OBJ)
KERNEL_ELF  := $(BUILD_DIR)/kernel.elf
KERNEL_DST  := $(EFI_IRIS_DIR)/KERNEL.ELF

CRT0    := $(firstword $(wildcard /usr/lib/crt0-efi-x86_64.o /usr/lib/x86_64-linux-gnu/gnuefi/crt0-efi-x86_64.o))
EFI_LDS := $(firstword $(wildcard /usr/lib/elf_x86_64_efi.lds /usr/lib/x86_64-linux-gnu/gnuefi/elf_x86_64_efi.lds))

ifeq ($(strip $(CRT0)),)
$(error No se encontro crt0-efi-x86_64.o. Instala gnu-efi)
endif
ifeq ($(strip $(EFI_LDS)),)
$(error No se encontro elf_x86_64_efi.lds. Instala gnu-efi)
endif

KERNEL_INCLUDES := -I./kernel/include
UEFI_INCLUDES   := -I./kernel/include -isystem /usr/include/efi -isystem /usr/include/efi/x86_64

COMMON_WARNINGS := -Wall -Wextra -Wshadow -Wundef
UEFI_DEFINES    := -DEFI_DEBUG=0 -DEFI_DEBUG_CLEAR_MEMORY=0

UEFI_CFLAGS    := -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -fpic $(COMMON_WARNINGS) $(UEFI_DEFINES) $(UEFI_INCLUDES)
KERNEL_CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone $(COMMON_WARNINGS) $(KERNEL_INCLUDES)
KERNEL_ASFLAGS := -ffreestanding -fno-pic -fno-pie -mno-red-zone $(KERNEL_INCLUDES)

EFI_LIBDIR_FLAGS := -L/usr/lib -L/usr/lib/x86_64-linux-gnu -L/usr/lib/x86_64-linux-gnu/gnuefi
LDFLAGS_EFI      := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic $(EFI_LIBDIR_FLAGS)
KERNEL_LDFLAGS   := -nostdlib -z max-page-size=0x1000 -T kernel/arch/x86_64/linker.ld
OBJCOPY_FLAGS    := -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64

.PHONY: all dirs run clean help check

all: $(BOOT_APP) $(KERNEL_DST)

help:
	@echo 'Targets disponibles:'
	@echo '  make        -> compila loader EFI y kernel ELF'
	@echo '  make run    -> arranca IRIS en QEMU con OVMF'
	@echo '  make check  -> inspecciona headers y segmentos del kernel ELF'
	@echo '  make clean  -> limpia artefactos generados'

dirs:
	mkdir -p $(BUILD_DIR) $(EFI_BOOT_DIR) $(EFI_IRIS_DIR)

$(LOADER_OBJ): boot/uefi/boot.c | dirs
	gcc $(UEFI_CFLAGS) -c $< -o $@

$(BOOT_SO): $(LOADER_OBJ)
	ld $(LDFLAGS_EFI) $(CRT0) $^ -o $@ -lefi -lgnuefi

$(BOOT_APP): $(BOOT_SO) | dirs
	objcopy $(OBJCOPY_FLAGS) $< $@

$(KERNEL_ENTRY_OBJ): kernel/arch/x86_64/boot/entry.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_MAIN_OBJ): kernel/kernel_main.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_PMM_OBJ): kernel/mm/pmm/pmm.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_PAGING_OBJ): kernel/arch/x86_64/paging.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_GDT_OBJ): kernel/arch/x86_64/gdt.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_IDT_OBJ): kernel/arch/x86_64/idt.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_PIC_OBJ): kernel/arch/x86_64/pic.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_GDT_FLUSH_OBJ): kernel/arch/x86_64/gdt_idt_flush.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_ISR_OBJ): kernel/arch/x86_64/isr_stubs.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_CTX_OBJ): kernel/arch/x86_64/context_switch.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_SCHED_OBJ): kernel/core/scheduler/scheduler.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_IPC_OBJ): kernel/core/ipc/ipc.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_FB_OBJ): kernel/drivers/fb/fb.c | dirs

	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_RAMFS_OBJ): kernel/fs/ramfs/ramfs.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_VFS_OBJ): kernel/fs/ramfs/vfs.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_TRAMP_OBJ): kernel/arch/x86_64/user_trampoline.S | dirs
	gcc $(KASM_FLAGS) -c $< -o $@

$(KERNEL_USERINIT_OBJ): kernel/arch/x86_64/user_init.S | dirs
	gcc $(KASM_FLAGS) -c $< -o $@

$(KERNEL_SYSCALL_OBJ): kernel/core/syscall/syscall.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALLE_OBJ): kernel/arch/x86_64/syscall_entry.S | dirs
	gcc $(KASM_FLAGS) -c $< -o $@

$(KERNEL_SERIAL_OBJ): kernel/drivers/serial/serial.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_PCI_OBJ): kernel/drivers/pci/pci.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS)
	ld $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@

$(KERNEL_DST): $(KERNEL_ELF) | dirs
	cp $< $@

check: $(KERNEL_ELF)
	@echo '== ELF header =='
	readelf -h $(KERNEL_ELF)
	@echo
	@echo '== Program headers =='
	readelf -l $(KERNEL_ELF)
	@echo
	@echo '== Sections =='
	readelf -S $(KERNEL_ELF)

run: all
	bash scripts/run_qemu.sh

clean:
	rm -f $(BUILD_DIR)/*.o
	rm -f $(BUILD_DIR)/*.so
	rm -f $(BUILD_DIR)/*.elf
	rm -f $(BUILD_DIR)/OVMF_VARS.fd
	rm -rf $(BUILD_DIR)/efi_root

