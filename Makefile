SHELL := /usr/bin/env bash

BUILD_DIR := build
EFI_ROOT := $(BUILD_DIR)/efi_root
EFI_BOOT_DIR := $(EFI_ROOT)/EFI/BOOT
EFI_IRIS_DIR := $(EFI_ROOT)/EFI/IRIS

LOADER_OBJ := $(BUILD_DIR)/boot_loader.o
BOOT_SO := $(BUILD_DIR)/BOOTX64.so
BOOT_APP := $(EFI_BOOT_DIR)/BOOTX64.EFI

KERNEL_ENTRY_OBJ := $(BUILD_DIR)/kernel_entry.o
KERNEL_MAIN_OBJ := $(BUILD_DIR)/kernel_main.o
KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_DST := $(EFI_IRIS_DIR)/KERNEL.ELF

CRT0 := $(firstword $(wildcard /usr/lib/crt0-efi-x86_64.o /usr/lib/x86_64-linux-gnu/gnuefi/crt0-efi-x86_64.o))
EFI_LDS := $(firstword $(wildcard /usr/lib/elf_x86_64_efi.lds /usr/lib/x86_64-linux-gnu/gnuefi/elf_x86_64_efi.lds))

ifeq ($(strip $(CRT0)),)
$(error No se encontro crt0-efi-x86_64.o. Instala gnu-efi)
endif

ifeq ($(strip $(EFI_LDS)),)
$(error No se encontro elf_x86_64_efi.lds. Instala gnu-efi)
endif

KERNEL_INCLUDES := -I./kernel/include
UEFI_INCLUDES := -I./kernel/include -isystem /usr/include/efi -isystem /usr/include/efi/x86_64

COMMON_WARNINGS := -Wall -Wextra -Wshadow -Wundef
UEFI_DEFINES := -DEFI_DEBUG=0 -DEFI_DEBUG_CLEAR_MEMORY=0
UEFI_CFLAGS := -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -fpic $(COMMON_WARNINGS) $(UEFI_DEFINES) $(UEFI_INCLUDES)
KERNEL_CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone $(COMMON_WARNINGS) $(KERNEL_INCLUDES)
KERNEL_ASFLAGS := -ffreestanding -fno-pic -fno-pie -mno-red-zone $(KERNEL_INCLUDES)

EFI_LIBDIR_FLAGS := -L/usr/lib -L/usr/lib/x86_64-linux-gnu -L/usr/lib/x86_64-linux-gnu/gnuefi
LDFLAGS_EFI := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic $(EFI_LIBDIR_FLAGS)
KERNEL_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T kernel/arch/x86_64/linker.ld
OBJCOPY_FLAGS := -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64

.PHONY: all dirs run clean help check

all: $(BOOT_APP) $(KERNEL_DST)

help:
	@echo "Targets disponibles:"
	@echo "  make        -> compila loader EFI y kernel ELF"
	@echo "  make run    -> arranca IRIS en QEMU con OVMF"
	@echo "  make check  -> inspecciona headers y segmentos del kernel ELF"
	@echo "  make clean  -> limpia artefactos generados"

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

$(KERNEL_ELF): $(KERNEL_ENTRY_OBJ) $(KERNEL_MAIN_OBJ)
	ld $(KERNEL_LDFLAGS) $^ -o $@

$(KERNEL_DST): $(KERNEL_ELF) | dirs
	cp $< $@

check: $(KERNEL_ELF)
	@echo "== ELF header =="
	readelf -h $(KERNEL_ELF)
	@echo
	@echo "== Program headers =="
	readelf -l $(KERNEL_ELF)
	@echo
	@echo "== Sections =="
	readelf -S $(KERNEL_ELF)

run: all
	bash scripts/run_qemu.sh

clean:
	rm -f $(BUILD_DIR)/*.o
	rm -f $(BUILD_DIR)/*.so
	rm -f $(BUILD_DIR)/*.elf
	rm -f $(BUILD_DIR)/OVMF_VARS.fd
	rm -rf $(BUILD_DIR)/efi_root
