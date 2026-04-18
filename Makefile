SHELL := /usr/bin/env bash

ENABLE_RUNTIME_SELFTESTS ?= 0

BUILD_DIR    := build
EFI_ROOT     := $(BUILD_DIR)/efi_root
EFI_BOOT_DIR := $(EFI_ROOT)/EFI/BOOT
EFI_IRIS_DIR := $(EFI_ROOT)/EFI/IRIS
BUILD_CONFIG_STAMP := $(BUILD_DIR)/.build_config
BUILD_CONFIG_MODE  := ENABLE_RUNTIME_SELFTESTS=$(ENABLE_RUNTIME_SELFTESTS)

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
KERNEL_FB_OBJ        := $(BUILD_DIR)/fb.o
KERNEL_TRAMP_OBJ     := $(BUILD_DIR)/user_trampoline.o
KERNEL_USERINIT_OBJ  := $(BUILD_DIR)/user_init.o
KERNEL_SYSCALL_OBJ   := $(BUILD_DIR)/syscall.o
KERNEL_USERCOPY_OBJ  := $(BUILD_DIR)/usercopy.o
KERNEL_SYSCALLE_OBJ  := $(BUILD_DIR)/syscall_entry.o
KERNEL_SERIAL_OBJ    := $(BUILD_DIR)/serial.o
KERNEL_NC_KOBJECT_OBJ    := $(BUILD_DIR)/nc_kobject.o
KERNEL_NC_HANDLE_OBJ     := $(BUILD_DIR)/nc_handle.o
KERNEL_NC_HANDLETBL_OBJ  := $(BUILD_DIR)/nc_handle_table.o
KERNEL_NC_KCHANNEL_OBJ   := $(BUILD_DIR)/nc_kchannel.o
KERNEL_NC_KVMO_OBJ       := $(BUILD_DIR)/nc_kvmo.o
KERNEL_NC_KNOTIF_OBJ     := $(BUILD_DIR)/nc_knotification.o
KERNEL_NC_KBOOTCAP_OBJ   := $(BUILD_DIR)/nc_kbootcap.o
KERNEL_NC_KPROCESS_OBJ   := $(BUILD_DIR)/nc_kprocess.o
KERNEL_NC_KIRQCAP_OBJ       := $(BUILD_DIR)/nc_kirqcap.o
KERNEL_NC_KIOPORT_OBJ       := $(BUILD_DIR)/nc_kioport.o
KERNEL_NC_KINITRDENTRY_OBJ  := $(BUILD_DIR)/nc_kinitrdentry.o
KERNEL_IRQROUTING_OBJ    := $(BUILD_DIR)/irq_routing.o
KERNEL_PHASE3_SELFTEST_OBJ := $(BUILD_DIR)/phase3_selftest.o
KERNEL_ELF_LOADER_OBJ     := $(BUILD_DIR)/elf_loader.o
KERNEL_INITRD_OBJ         := $(BUILD_DIR)/initrd.o
KERNEL_FUTEX_OBJ          := $(BUILD_DIR)/futex.o
# Service ELF binaries — output directly into their source directories so that
# the objcopy -I binary relative path (services/xxx/xxx.elf) matches the symbol
# name mangling expected by kernel/core/initrd/initrd.c.
SERVICE_SVCMGR_ELF := services/svcmgr/svcmgr.elf
SERVICE_KBD_ELF    := services/kbd/kbd.elf
SERVICE_VFS_ELF    := services/vfs/vfs.elf
SERVICE_INIT_ELF   := services/init/init.elf
# objcopy-embedded initrd object files (binary blobs → linkable .o)
KERNEL_SVCMGR_BIN_OBJ := $(BUILD_DIR)/svcmgr_bin.o
KERNEL_KBD_BIN_OBJ    := $(BUILD_DIR)/kbd_bin.o
KERNEL_VFS_BIN_OBJ    := $(BUILD_DIR)/vfs_bin.o
KERNEL_INIT_BIN_OBJ   := $(BUILD_DIR)/init_bin.o
KERNEL_DEMO_OBJS :=
KERNEL_DEMO_DEFINES :=
ifeq ($(ENABLE_RUNTIME_SELFTESTS),1)
KERNEL_DEMO_DEFINES      += -DIRIS_ENABLE_RUNTIME_SELFTESTS
# user_init.S contains user_selftest + vfs_leak_child — only needed for selftests.
KERNEL_DEMO_OBJS         += $(KERNEL_USERINIT_OBJ)
endif
KERNEL_OBJS := $(KERNEL_ENTRY_OBJ) $(KERNEL_MAIN_OBJ) $(KERNEL_PMM_OBJ) $(KERNEL_PAGING_OBJ) $(KERNEL_GDT_OBJ) $(KERNEL_IDT_OBJ) $(KERNEL_PIC_OBJ) $(KERNEL_GDT_FLUSH_OBJ) $(KERNEL_ISR_OBJ) $(KERNEL_CTX_OBJ) $(KERNEL_SCHED_OBJ) $(KERNEL_FB_OBJ) $(KERNEL_TRAMP_OBJ) $(KERNEL_SYSCALL_OBJ) $(KERNEL_USERCOPY_OBJ) $(KERNEL_SYSCALLE_OBJ) $(KERNEL_SERIAL_OBJ) $(KERNEL_NC_KOBJECT_OBJ) $(KERNEL_NC_HANDLE_OBJ) $(KERNEL_NC_HANDLETBL_OBJ) $(KERNEL_NC_KCHANNEL_OBJ) $(KERNEL_NC_KVMO_OBJ) $(KERNEL_NC_KNOTIF_OBJ) $(KERNEL_NC_KBOOTCAP_OBJ) $(KERNEL_NC_KPROCESS_OBJ) $(KERNEL_NC_KIRQCAP_OBJ) $(KERNEL_NC_KIOPORT_OBJ) $(KERNEL_NC_KINITRDENTRY_OBJ) $(KERNEL_IRQROUTING_OBJ) $(KERNEL_PHASE3_SELFTEST_OBJ) $(KERNEL_ELF_LOADER_OBJ) $(KERNEL_INITRD_OBJ) $(KERNEL_FUTEX_OBJ) $(KERNEL_SVCMGR_BIN_OBJ) $(KERNEL_KBD_BIN_OBJ) $(KERNEL_VFS_BIN_OBJ) $(KERNEL_INIT_BIN_OBJ) $(KERNEL_DEMO_OBJS)
KERNEL_ELF  := $(BUILD_DIR)/kernel.elf
KERNEL_DST  := $(EFI_IRIS_DIR)/KERNEL.ELF

CRT0    := $(firstword $(wildcard /usr/lib/crt0-efi-x86_64.o /usr/lib/x86_64-linux-gnu/gnuefi/crt0-efi-x86_64.o))
EFI_LDS := $(firstword $(wildcard /usr/lib/elf_x86_64_efi.lds /usr/lib/x86_64-linux-gnu/gnuefi/elf_x86_64_efi.lds))

ifeq ($(strip $(CRT0)),)
$(error crt0-efi-x86_64.o not found. Install gnu-efi)
endif
ifeq ($(strip $(EFI_LDS)),)
$(error elf_x86_64_efi.lds not found. Install gnu-efi)
endif

KERNEL_INCLUDES := -I./kernel/include -I./kernel/new_core/include
UEFI_INCLUDES   := -I./kernel/include -isystem /usr/include/efi -isystem /usr/include/efi/x86_64
SERVICE_INCLUDES := -I./kernel/include -I./kernel/new_core/include

COMMON_WARNINGS := -Wall -Wextra -Wshadow -Wundef
UEFI_DEFINES    := -DEFI_DEBUG=0 -DEFI_DEBUG_CLEAR_MEMORY=0

UEFI_CFLAGS    := -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -fpic $(COMMON_WARNINGS) $(UEFI_DEFINES) $(UEFI_INCLUDES)
KERNEL_CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone $(COMMON_WARNINGS) -D__KERNEL__ $(KERNEL_INCLUDES) $(KERNEL_DEMO_DEFINES) -MMD -MP
KERNEL_ASFLAGS := -ffreestanding -fno-pic -fno-pie -mno-red-zone -D__KERNEL__ $(KERNEL_INCLUDES)
# Service binaries: freestanding static ELFs; no kernel-internal symbols.
# -D__KERNEL__ is explicitly absent so headers expose only the userland ABI.
# -mcmodel=large: services link at 0x0000008000100000 (> 4 GB), which exceeds
# the 32-bit address range assumed by the default small code model.  The large
# model emits 64-bit absolute relocations (R_X86_64_64) for all symbol
# references, so .rodata and function addresses are correctly resolved.
SERVICE_CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=large $(COMMON_WARNINGS) $(SERVICE_INCLUDES) -MMD -MP
SERVICE_ASFLAGS := -ffreestanding -fno-pic -fno-pie -mno-red-zone $(SERVICE_INCLUDES)
SERVICE_LDFLAGS := -nostdlib -static -T services/link_service.ld

EFI_LIBDIR_FLAGS := -L/usr/lib -L/usr/lib/x86_64-linux-gnu -L/usr/lib/x86_64-linux-gnu/gnuefi
LDFLAGS_EFI      := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic $(EFI_LIBDIR_FLAGS)
KERNEL_LDFLAGS   := -nostdlib -z max-page-size=0x1000 -z noexecstack -T kernel/arch/x86_64/linker.ld
OBJCOPY_FLAGS    := -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64

.PHONY: all dirs run run-headless clean help check smoke smoke-runtime config-sync

all: config-sync $(BOOT_APP) $(KERNEL_DST)

help:
	@echo 'Available targets:'
	@echo '  make        -> build EFI loader and kernel ELF'
	@echo '  make run    -> launch IRIS in QEMU with OVMF'
	@echo '  make run-headless -> launch IRIS in headless QEMU with serial log capture'
	@echo '  make check  -> inspect kernel ELF headers and segments'
	@echo '  make smoke  -> reproducible local build smoke (default + selftest build)'
	@echo '  make smoke-runtime -> headless runtime smoke with healthy-boot log assertion'
	@echo '  make clean  -> remove all build artifacts'
	@echo
	@echo 'Options:'
	@echo '  ENABLE_RUNTIME_SELFTESTS=1 -> enable heavy runtime probes/selftests'

dirs:
	mkdir -p $(BUILD_DIR) $(EFI_BOOT_DIR) $(EFI_IRIS_DIR)

config-sync:
	@mkdir -p $(BUILD_DIR)
	@if [ -f $(BUILD_CONFIG_STAMP) ] && [ "$$(cat $(BUILD_CONFIG_STAMP))" != "$(BUILD_CONFIG_MODE)" ]; then \
		echo "[build] configuration changed to $(BUILD_CONFIG_MODE); cleaning stale artifacts"; \
		rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.so $(BUILD_DIR)/*.elf $(BUILD_DIR)/*.d $(BUILD_DIR)/OVMF_VARS.fd; \
		rm -rf $(EFI_ROOT); \
		rm -f $(SERVICE_SVCMGR_ELF) $(SERVICE_KBD_ELF) $(SERVICE_VFS_ELF) $(SERVICE_INIT_ELF) $(SERVICE_LIFECYCLE_PROBE_ELF); \
	fi
	@printf '%s\n' "$(BUILD_CONFIG_MODE)" > $(BUILD_CONFIG_STAMP)

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

$(KERNEL_FB_OBJ): kernel/drivers/fb/fb.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_TRAMP_OBJ): kernel/arch/x86_64/user_trampoline.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_USERINIT_OBJ): kernel/arch/x86_64/user_init.S | dirs
	gcc $(KERNEL_ASFLAGS) $(KERNEL_DEMO_DEFINES) -c $< -o $@

$(KERNEL_SYSCALL_OBJ): kernel/core/syscall/syscall.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_USERCOPY_OBJ): kernel/core/usercopy.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALLE_OBJ): kernel/arch/x86_64/syscall_entry.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_SERIAL_OBJ): kernel/drivers/serial/serial.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KOBJECT_OBJ): kernel/new_core/src/kobject.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_HANDLE_OBJ): kernel/new_core/src/handle.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_HANDLETBL_OBJ): kernel/new_core/src/handle_table.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KCHANNEL_OBJ): kernel/new_core/src/kchannel.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KVMO_OBJ): kernel/new_core/src/kvmo.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KNOTIF_OBJ): kernel/new_core/src/knotification.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KBOOTCAP_OBJ): kernel/new_core/src/kbootcap.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KPROCESS_OBJ): kernel/new_core/src/kprocess.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KIRQCAP_OBJ): kernel/new_core/src/kirqcap.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KIOPORT_OBJ): kernel/new_core/src/kioport.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KINITRDENTRY_OBJ): kernel/new_core/src/kinitrdentry.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_IRQROUTING_OBJ): kernel/core/irq/irq_routing.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_VFSSRV_ENTRY_OBJ): kernel/arch/x86_64/vfs_server.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_VFSSRV_OBJ): kernel/core/vfs_service.c | dirs
	gcc $(VFSSRV_CFLAGS) -c $< -o $@

$(KERNEL_SVCMGR_ENTRY_OBJ): kernel/arch/x86_64/svcmgr.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_SVCMGR_OBJ): kernel/core/svcmgr.c | dirs
	gcc $(SVCMGR_CFLAGS) -c $< -o $@

$(KERNEL_PHASE3_SELFTEST_OBJ): kernel/core/phase3_selftest.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_ELF_LOADER_OBJ): kernel/core/loader/elf_loader.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_INITRD_OBJ): kernel/core/initrd/initrd.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_FUTEX_OBJ): kernel/core/futex/futex.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

# ── Service ELF build rules ─────────────────────────────────────────────────
#
# Each service is a standalone static ELF64 ET_EXEC binary linked at
# SERVICE_LOAD_BASE (0x0000008000100000).  The compiled object files are fed
# to ld with services/link_service.ld.  The resulting ELF is then embedded
# into the kernel binary as a binary blob via objcopy -I binary.
#
# objcopy mangling rule: every non-alphanumeric character in the input path
# becomes '_', producing the symbol prefix:
#   services/svcmgr/svcmgr.elf  →  _binary_services_svcmgr_svcmgr_elf_{start,end,size}
#   services/kbd/kbd.elf        →  _binary_services_kbd_kbd_elf_{start,end,size}
#   services/vfs/vfs.elf        →  _binary_services_vfs_vfs_elf_{start,end,size}
#
# The objcopy command is run from the project root so the relative path of the
# input file determines the mangled symbol name.  The service ELFs must be
# built before the kernel objects that reference them (initrd.o, svcmgr_bin.o,
# etc.) — the explicit order dependency below enforces this.

# ── svcmgr service ──────────────────────────────────────────────────────────
$(BUILD_DIR)/svcmgr_entry.o: services/svcmgr/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/svcmgr_main.o: services/svcmgr/svcmgr.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_SVCMGR_ELF): $(BUILD_DIR)/svcmgr_entry.o $(BUILD_DIR)/svcmgr_main.o
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_SVCMGR_BIN_OBJ): $(SERVICE_SVCMGR_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_SVCMGR_ELF) $@

# ── kbd service ─────────────────────────────────────────────────────────────
$(BUILD_DIR)/kbd_main.o: services/kbd/main.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(SERVICE_KBD_ELF): $(BUILD_DIR)/kbd_main.o
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_KBD_BIN_OBJ): $(SERVICE_KBD_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_KBD_ELF) $@

# ── vfs service ─────────────────────────────────────────────────────────────
$(BUILD_DIR)/vfs_entry.o: services/vfs/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs_main.o: services/vfs/vfs.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_VFS_ELF): $(BUILD_DIR)/vfs_entry.o $(BUILD_DIR)/vfs_main.o
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_VFS_BIN_OBJ): $(SERVICE_VFS_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_VFS_ELF) $@

# ── init service ─────────────────────────────────────────────────────────────
$(BUILD_DIR)/init_entry.o: services/init/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/init_main.o: services/init/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_INIT_ELF): $(BUILD_DIR)/init_entry.o $(BUILD_DIR)/init_main.o
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_INIT_BIN_OBJ): $(SERVICE_INIT_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_INIT_ELF) $@

$(KERNEL_ELF): $(KERNEL_OBJS)
	ld $(KERNEL_LDFLAGS) $(KERNEL_OBJS) -o $@

$(KERNEL_DST): $(KERNEL_ELF) | dirs
	cp $< $@

check: config-sync $(KERNEL_ELF)
	@echo '== ELF header =='
	readelf -h $(KERNEL_ELF)
	@echo
	@echo '== Program headers =='
	readelf -l $(KERNEL_ELF)
	@echo
	@echo '== Sections =='
	readelf -S $(KERNEL_ELF)

smoke:
	bash scripts/smoke_local.sh

smoke-runtime: all
	bash scripts/run_qemu_headless.sh

run: all
	bash scripts/run_qemu.sh

run-headless: all
	bash scripts/run_qemu_headless.sh

clean:
	rm -f $(BUILD_DIR)/*.o
	rm -f $(BUILD_DIR)/*.so
	rm -f $(BUILD_DIR)/*.elf
	rm -f $(BUILD_DIR)/*.d
	rm -f $(BUILD_CONFIG_STAMP)
	rm -f $(BUILD_DIR)/OVMF_VARS.fd
	rm -rf $(BUILD_DIR)/efi_root
	rm -f $(SERVICE_SVCMGR_ELF) $(SERVICE_KBD_ELF) $(SERVICE_VFS_ELF) $(SERVICE_INIT_ELF)

# ── Header dependency tracking ──────────────────────────────────────────────
# Generated by -MMD -MP in KERNEL_CFLAGS and SERVICE_CFLAGS.
# Each .o rule writes a .d file alongside itself; we include them here so that
# modifying any header triggers a rebuild of all dependent .o files without
# requiring 'make clean'.
-include $(wildcard $(BUILD_DIR)/*.d)
