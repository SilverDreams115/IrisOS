SHELL := /usr/bin/env bash

BUILD_DIR := build
EFI_ROOT := $(BUILD_DIR)/efi_root
EFI_BOOT_DIR := $(EFI_ROOT)/EFI/BOOT
EFI_IRIS_DIR := $(EFI_ROOT)/EFI/IRIS

LOADER_OBJ := $(BUILD_DIR)/boot_loader.o
KERNEL_ENTRY_OBJ := $(BUILD_DIR)/kernel_efi_entry.o
KERNEL_MAIN_OBJ := $(BUILD_DIR)/kernel_main.o

BOOT_SO := $(BUILD_DIR)/BOOTX64.so
KERNEL_SO := $(BUILD_DIR)/KERNELX64.so

BOOT_APP := $(EFI_BOOT_DIR)/BOOTX64.EFI
KERNEL_APP := $(EFI_IRIS_DIR)/KERNELX64.EFI

CRT0 := $(firstword $(wildcard /usr/lib/crt0-efi-x86_64.o /usr/lib/x86_64-linux-gnu/gnuefi/crt0-efi-x86_64.o))
EFI_LDS := $(firstword $(wildcard /usr/lib/elf_x86_64_efi.lds /usr/lib/x86_64-linux-gnu/gnuefi/elf_x86_64_efi.lds))

ifeq ($(strip $(CRT0)),)
$(error No se encontro crt0-efi-x86_64.o. Instala gnu-efi)
endif

ifeq ($(strip $(EFI_LDS)),)
$(error No se encontro elf_x86_64_efi.lds. Instala gnu-efi)
endif

INCLUDES := -I./kernel/include -I/usr/include/efi -I/usr/include/efi/x86_64
CFLAGS := -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -Wall -Wextra $(INCLUDES)
UEFI_CFLAGS := $(CFLAGS) -fpic

EFI_LIBDIR_FLAGS := -L/usr/lib -L/usr/lib/x86_64-linux-gnu -L/usr/lib/x86_64-linux-gnu/gnuefi
LDFLAGS_EFI := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic $(EFI_LIBDIR_FLAGS)
OBJCOPY_FLAGS := -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64

.PHONY: all dirs run clean help

all: $(BOOT_APP) $(KERNEL_APP)

help:
	@echo "Targets disponibles:"
	@echo "  make        -> compila loader y kernel EFI"
	@echo "  make run    -> arranca IRIS en QEMU con OVMF"
	@echo "  make clean  -> limpia artefactos generados"

dirs:
	mkdir -p $(BUILD_DIR) $(EFI_BOOT_DIR) $(EFI_IRIS_DIR)

$(LOADER_OBJ): boot/uefi/boot.c | dirs
	gcc $(UEFI_CFLAGS) -c $< -o $@

$(KERNEL_ENTRY_OBJ): kernel/uefi/efi_entry.c | dirs
	gcc $(UEFI_CFLAGS) -c $< -o $@

$(KERNEL_MAIN_OBJ): kernel/kernel_main.c | dirs
	gcc $(UEFI_CFLAGS) -c $< -o $@

$(BOOT_SO): $(LOADER_OBJ)
	ld $(LDFLAGS_EFI) $(CRT0) $^ -o $@ -lefi -lgnuefi

$(KERNEL_SO): $(KERNEL_ENTRY_OBJ) $(KERNEL_MAIN_OBJ)
	ld $(LDFLAGS_EFI) $(CRT0) $^ -o $@ -lefi -lgnuefi

$(BOOT_APP): $(BOOT_SO) | dirs
	objcopy $(OBJCOPY_FLAGS) $< $@

$(KERNEL_APP): $(KERNEL_SO) | dirs
	objcopy $(OBJCOPY_FLAGS) $< $@

run: all
	bash scripts/run_qemu.sh

clean:
	rm -f $(BUILD_DIR)/*.o
	rm -f $(BUILD_DIR)/*.so
	rm -f $(BUILD_DIR)/OVMF_VARS.fd
	rm -rf $(BUILD_DIR)/efi_root
