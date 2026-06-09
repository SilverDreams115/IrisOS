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
KERNEL_KSTACK_OBJ    := $(BUILD_DIR)/kstack.o
KERNEL_LIFECYCLE_OBJ := $(BUILD_DIR)/task_lifecycle.o
KERNEL_TRAMP_OBJ     := $(BUILD_DIR)/user_trampoline.o
KERNEL_USERINIT_OBJ  := $(BUILD_DIR)/user_init.o
KERNEL_SYSCALL_DISPATCH_OBJ := $(BUILD_DIR)/syscall_dispatch.o
KERNEL_SYSCALL_IPC_OBJ      := $(BUILD_DIR)/syscall_ipc.o
KERNEL_SYSCALL_VM_OBJ       := $(BUILD_DIR)/syscall_vm.o
KERNEL_SYSCALL_PROC_OBJ     := $(BUILD_DIR)/syscall_proc.o
KERNEL_SYSCALL_CAP_OBJ      := $(BUILD_DIR)/syscall_cap.o
KERNEL_SYSCALL_IRQ_OBJ      := $(BUILD_DIR)/syscall_irq.o
KERNEL_SYSCALL_DIAG_OBJ     := $(BUILD_DIR)/syscall_diag.o
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
KERNEL_NC_KENDPOINT_OBJ     := $(BUILD_DIR)/nc_kendpoint.o
KERNEL_SYSCALL_EP_OBJ       := $(BUILD_DIR)/syscall_endpoint.o
KERNEL_IRQROUTING_OBJ    := $(BUILD_DIR)/irq_routing.o
KERNEL_PHASE3_SELFTEST_OBJ := $(BUILD_DIR)/phase3_selftest.o
KERNEL_INITRD_OBJ         := $(BUILD_DIR)/initrd.o
KERNEL_FUTEX_OBJ          := $(BUILD_DIR)/futex.o
KERNEL_KPAGE_OBJ          := $(BUILD_DIR)/kpage.o
KERNEL_KSLAB_OBJ          := $(BUILD_DIR)/kslab.o
KERNEL_KLOG_OBJ           := $(BUILD_DIR)/klog.o
KERNEL_PANIC_OBJ          := $(BUILD_DIR)/panic.o
KERNEL_LAPIC_OBJ          := $(BUILD_DIR)/lapic.o
STACK_GUARD_OBJ           := $(BUILD_DIR)/stack_guard.o
KERNEL_NC_KREPLY_OBJ         := $(BUILD_DIR)/nc_kreply.o
KERNEL_NC_KCNODE_OBJ         := $(BUILD_DIR)/nc_kcnode.o
KERNEL_NC_KSCHEDCTX_OBJ      := $(BUILD_DIR)/nc_kschedctx.o
KERNEL_NC_KUNTYPED_OBJ       := $(BUILD_DIR)/nc_kuntyped.o
KERNEL_SYSCALL_CSPACE_OBJ    := $(BUILD_DIR)/syscall_cspace.o
KERNEL_SYSCALL_SCHED_OBJ     := $(BUILD_DIR)/syscall_sched.o
KERNEL_SYSCALL_UNTYPED_OBJ   := $(BUILD_DIR)/syscall_untyped.o
KERNEL_SYSCALL_REPLY_OBJ     := $(BUILD_DIR)/syscall_reply.o
KERNEL_SYSCALL_CNODE_OPS_OBJ := $(BUILD_DIR)/syscall_cnode_ops.o
KERNEL_NC_KTCB_OBJ           := $(BUILD_DIR)/nc_ktcb.o
KERNEL_SYSCALL_TCB_OBJ       := $(BUILD_DIR)/syscall_tcb.o
KERNEL_NC_CSPACE_OBJ         := $(BUILD_DIR)/nc_cspace.o
KERNEL_NC_KVSPACE_OBJ        := $(BUILD_DIR)/nc_kvspace.o
# Service ELF binaries — output directly into their source directories so that
# the objcopy -I binary relative path (services/xxx/xxx.elf) matches the symbol
# name mangling expected by kernel/core/initrd/initrd.c.
SERVICE_SVCMGR_ELF    := services/svcmgr/svcmgr.elf
SERVICE_KBD_ELF       := services/kbd/kbd.elf
SERVICE_VFS_ELF       := services/vfs/vfs.elf
SERVICE_INIT_ELF      := services/init/init.elf
SERVICE_CONSOLE_ELF   := services/console/console.elf
SERVICE_FB_ELF        := services/fb/fb.elf
SERVICE_SH_ELF        := services/sh/sh.elf
SERVICE_IRIS_TEST_ELF := services/iris_test/iris_test.elf
# userboot: linked as raw flat binary (OUTPUT_FORMAT(binary)) for direct kernel mapping
SERVICE_USERBOOT_BIN  := services/userboot/userboot.bin
# objcopy-embedded initrd object files (binary blobs → linkable .o)
KERNEL_SVCMGR_BIN_OBJ    := $(BUILD_DIR)/svcmgr_bin.o
KERNEL_KBD_BIN_OBJ        := $(BUILD_DIR)/kbd_bin.o
KERNEL_VFS_BIN_OBJ        := $(BUILD_DIR)/vfs_bin.o
KERNEL_INIT_BIN_OBJ       := $(BUILD_DIR)/init_bin.o
KERNEL_CONSOLE_BIN_OBJ    := $(BUILD_DIR)/console_bin.o
KERNEL_FB_SVC_BIN_OBJ     := $(BUILD_DIR)/fb_svc_bin.o
KERNEL_SH_BIN_OBJ         := $(BUILD_DIR)/sh_bin.o
KERNEL_IRIS_TEST_BIN_OBJ  := $(BUILD_DIR)/iris_test_bin.o
KERNEL_USERBOOT_BIN_OBJ   := $(BUILD_DIR)/userboot_bin.o
KERNEL_DEMO_OBJS :=
KERNEL_DEMO_DEFINES :=
ifeq ($(ENABLE_RUNTIME_SELFTESTS),1)
KERNEL_DEMO_DEFINES      += -DIRIS_ENABLE_RUNTIME_SELFTESTS
# user_init.S contains user_selftest + vfs_leak_child — only needed for selftests.
KERNEL_DEMO_OBJS         += $(KERNEL_USERINIT_OBJ)
endif
KERNEL_OBJS := $(KERNEL_ENTRY_OBJ) $(KERNEL_MAIN_OBJ) $(KERNEL_KSLAB_OBJ) $(KERNEL_PMM_OBJ) $(KERNEL_PAGING_OBJ) $(KERNEL_GDT_OBJ) $(KERNEL_IDT_OBJ) $(KERNEL_PIC_OBJ) $(KERNEL_GDT_FLUSH_OBJ) $(KERNEL_ISR_OBJ) $(KERNEL_CTX_OBJ) $(KERNEL_SCHED_OBJ) $(KERNEL_KSTACK_OBJ) $(KERNEL_LIFECYCLE_OBJ) $(KERNEL_TRAMP_OBJ) $(KERNEL_SYSCALL_DISPATCH_OBJ) $(KERNEL_SYSCALL_IPC_OBJ) $(KERNEL_SYSCALL_VM_OBJ) $(KERNEL_SYSCALL_PROC_OBJ) $(KERNEL_SYSCALL_CAP_OBJ) $(KERNEL_SYSCALL_IRQ_OBJ) $(KERNEL_SYSCALL_DIAG_OBJ) $(KERNEL_SYSCALL_EP_OBJ) $(KERNEL_USERCOPY_OBJ) $(KERNEL_SYSCALLE_OBJ) $(KERNEL_SERIAL_OBJ) $(KERNEL_NC_KOBJECT_OBJ) $(KERNEL_NC_HANDLE_OBJ) $(KERNEL_NC_HANDLETBL_OBJ) $(KERNEL_NC_KCHANNEL_OBJ) $(KERNEL_NC_KVMO_OBJ) $(KERNEL_NC_KNOTIF_OBJ) $(KERNEL_NC_KBOOTCAP_OBJ) $(KERNEL_NC_KPROCESS_OBJ) $(KERNEL_NC_KIRQCAP_OBJ) $(KERNEL_NC_KIOPORT_OBJ) $(KERNEL_NC_KINITRDENTRY_OBJ) $(KERNEL_NC_KENDPOINT_OBJ) $(KERNEL_IRQROUTING_OBJ) $(KERNEL_PHASE3_SELFTEST_OBJ) $(KERNEL_INITRD_OBJ) $(KERNEL_FUTEX_OBJ) $(KERNEL_KPAGE_OBJ) $(KERNEL_KLOG_OBJ) $(KERNEL_PANIC_OBJ) $(KERNEL_LAPIC_OBJ) $(KERNEL_NC_KREPLY_OBJ) $(KERNEL_NC_KCNODE_OBJ) $(KERNEL_NC_KSCHEDCTX_OBJ) $(KERNEL_NC_KUNTYPED_OBJ) $(KERNEL_SYSCALL_CSPACE_OBJ) $(KERNEL_SYSCALL_SCHED_OBJ) $(KERNEL_SYSCALL_UNTYPED_OBJ) $(KERNEL_SYSCALL_REPLY_OBJ) $(KERNEL_SYSCALL_CNODE_OPS_OBJ) $(KERNEL_NC_KTCB_OBJ) $(KERNEL_SYSCALL_TCB_OBJ) $(KERNEL_NC_CSPACE_OBJ) $(KERNEL_NC_KVSPACE_OBJ) $(KERNEL_USERBOOT_BIN_OBJ) $(KERNEL_SVCMGR_BIN_OBJ) $(KERNEL_KBD_BIN_OBJ) $(KERNEL_VFS_BIN_OBJ) $(KERNEL_INIT_BIN_OBJ) $(KERNEL_CONSOLE_BIN_OBJ) $(KERNEL_FB_SVC_BIN_OBJ) $(KERNEL_SH_BIN_OBJ) $(KERNEL_IRIS_TEST_BIN_OBJ) $(KERNEL_DEMO_OBJS)
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
# Service binaries: freestanding static PIE ELFs; no kernel-internal symbols.
# -D__KERNEL__ is explicitly absent so headers expose only the userland ABI.
# -fPIE: position-independent code; the kernel ELF loader applies ASLR bias
#   and R_X86_64_RELATIVE relocations before the task starts.
# -static -pie: ET_DYN binary with no shared library dependencies; the default
#   small code model works because all symbols are within the PIE itself.
SERVICE_CFLAGS  := -ffreestanding -fstack-protector-strong -mstack-protector-guard=global -fPIE -mno-red-zone $(COMMON_WARNINGS) $(SERVICE_INCLUDES) -MMD -MP
# userboot is a flat binary (no ELF, no CRT); stack-protector requires __stack_chk_guard
# which userboot does not link. Keep -fno-stack-protector for userboot only.
USERBOOT_CFLAGS := -ffreestanding -fno-stack-protector -fPIE -mno-red-zone $(COMMON_WARNINGS) $(SERVICE_INCLUDES) -MMD -MP
SERVICE_ASFLAGS := -ffreestanding -mno-red-zone $(SERVICE_INCLUDES)
SERVICE_LDFLAGS := -nostdlib -static -pie -T services/link_service.ld

EFI_LIBDIR_FLAGS := -L/usr/lib -L/usr/lib/x86_64-linux-gnu -L/usr/lib/x86_64-linux-gnu/gnuefi
LDFLAGS_EFI      := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic $(EFI_LIBDIR_FLAGS)
KERNEL_LDFLAGS   := -nostdlib -z max-page-size=0x1000 -z noexecstack -T kernel/arch/x86_64/linker.ld
OBJCOPY_FLAGS    := -I elf64-x86-64 -O pei-x86-64 --subsystem=10 -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc -j .rodata

# ── Host unit-test build ────────────────────────────────────────────────────
TEST_UNIT_INCS  := -I tests/kernel/include -I kernel/new_core/include -I kernel/include -I tests/kernel
TEST_UNIT_CFLAGS := -D__KERNEL__ -Wall -Wextra -Wshadow -std=c11 \
    -Wno-unused-function \
    $(TEST_UNIT_INCS)
TEST_UNIT_SRCS  := \
    tests/kernel/stubs.c \
    kernel/new_core/src/kobject.c \
    kernel/new_core/src/handle.c \
    kernel/new_core/src/handle_table.c \
    kernel/new_core/src/kcnode.c \
    kernel/new_core/src/kuntyped.c \
    kernel/new_core/src/kbootcap.c \
    kernel/new_core/src/kendpoint.c \
    kernel/new_core/src/kchannel.c \
    kernel/new_core/src/knotification.c \
    kernel/new_core/src/kreply.c \
    kernel/new_core/src/kschedctx.c \
    kernel/new_core/src/cspace.c \
    kernel/new_core/src/kvspace.c \
    tests/kernel/test_rights.c \
    tests/kernel/test_kobject.c \
    tests/kernel/test_kcnode.c \
    tests/kernel/test_kuntyped.c \
    tests/kernel/test_handle_table.c \
    tests/kernel/test_kendpoint.c \
    tests/kernel/test_kchannel.c \
    tests/kernel/test_knotification.c \
    tests/kernel/test_kreply.c \
    tests/kernel/test_kschedctx.c \
    tests/kernel/test_cspace.c \
    tests/kernel/test_ipc_cspace.c \
    tests/kernel/test_untyped_cspace.c \
    tests/kernel/test_boot_cspace.c \
    tests/kernel/test_vspace_cspace.c \
    tests/kernel/test_klog.c \
    kernel/core/klog/klog.c \
    tests/kernel/test_main.c
TEST_UNIT_BIN   := $(BUILD_DIR)/test_unit

.PHONY: all dirs run run-headless clean help check smoke smoke-runtime smoke-runtime-selftests config-sync test-unit

all: config-sync $(BOOT_APP) $(KERNEL_DST)

help:
	@echo 'Available targets:'
	@echo '  make        -> build EFI loader and kernel ELF'
	@echo '  make run    -> launch IRIS in QEMU with OVMF'
	@echo '  make run-headless -> launch IRIS in headless QEMU with serial log capture'
	@echo '  make check  -> inspect kernel ELF headers and segments'
	@echo '  make smoke  -> reproducible local build smoke (default + selftest build)'
	@echo '  make smoke-runtime -> headless runtime smoke with healthy-boot log assertion'
	@echo '  make smoke-runtime-selftests -> headless runtime smoke for ENABLE_RUNTIME_SELFTESTS=1'
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
		rm -f $(SERVICE_SVCMGR_ELF) $(SERVICE_KBD_ELF) $(SERVICE_VFS_ELF) $(SERVICE_INIT_ELF) $(SERVICE_CONSOLE_ELF) $(SERVICE_FB_ELF) $(SERVICE_SH_ELF) $(SERVICE_LIFECYCLE_PROBE_ELF) $(SERVICE_USERBOOT_BIN); \
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

$(KERNEL_KSTACK_OBJ): kernel/core/scheduler/kstack.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_LIFECYCLE_OBJ): kernel/core/scheduler/task_lifecycle.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_TRAMP_OBJ): kernel/arch/x86_64/user_trampoline.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_USERINIT_OBJ): kernel/arch/x86_64/user_init.S | dirs
	gcc $(KERNEL_ASFLAGS) $(KERNEL_DEMO_DEFINES) -c $< -o $@

$(KERNEL_SYSCALL_DISPATCH_OBJ): kernel/core/syscall/syscall_dispatch.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_IPC_OBJ): kernel/core/syscall/syscall_ipc.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_VM_OBJ): kernel/core/syscall/syscall_vm.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_PROC_OBJ): kernel/core/syscall/syscall_proc.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_CAP_OBJ): kernel/core/syscall/syscall_cap.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_IRQ_OBJ): kernel/core/syscall/syscall_irq.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_DIAG_OBJ): kernel/core/syscall/syscall_diag.c kernel/core/syscall/syscall_priv.h | dirs
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

$(KERNEL_NC_KENDPOINT_OBJ): kernel/new_core/src/kendpoint.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_EP_OBJ): kernel/core/syscall/syscall_endpoint.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_IRQROUTING_OBJ): kernel/core/irq/irq_routing.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@


$(KERNEL_PHASE3_SELFTEST_OBJ): kernel/core/phase3_selftest.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_INITRD_OBJ): kernel/core/initrd/initrd.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_FUTEX_OBJ): kernel/core/futex/futex.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_KPAGE_OBJ): kernel/mm/kpage/kpage.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_KSLAB_OBJ): kernel/mm/kslab/kslab.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_KLOG_OBJ): kernel/core/klog/klog.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_PANIC_OBJ): kernel/core/panic/panic.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_LAPIC_OBJ): kernel/arch/x86_64/lapic.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KREPLY_OBJ): kernel/new_core/src/kreply.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KCNODE_OBJ): kernel/new_core/src/kcnode.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KSCHEDCTX_OBJ): kernel/new_core/src/kschedctx.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KUNTYPED_OBJ): kernel/new_core/src/kuntyped.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_CSPACE_OBJ): kernel/core/syscall/syscall_cspace.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_SCHED_OBJ): kernel/core/syscall/syscall_sched.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_UNTYPED_OBJ): kernel/core/syscall/syscall_untyped.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_REPLY_OBJ): kernel/core/syscall/syscall_reply.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_CNODE_OPS_OBJ): kernel/core/syscall/syscall_cnode_ops.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KTCB_OBJ): kernel/new_core/src/ktcb.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_TCB_OBJ): kernel/core/syscall/syscall_tcb.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_CSPACE_OBJ): kernel/new_core/src/cspace.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KVSPACE_OBJ): kernel/new_core/src/kvspace.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

# ── userboot — ring-3 bootstrap; built as raw flat binary for direct kernel mapping ─
# Linked with OUTPUT_FORMAT(binary) so byte 0 = _start (no ELF header overhead).
# -nostdlib -static only (no -pie: binary format is not ELF).
$(BUILD_DIR)/ub_entry.o: services/userboot/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/ub_main.o: services/userboot/main.c | dirs
	gcc $(USERBOOT_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ub_svc_loader.o: services/common/svc_loader.c | dirs
	gcc $(USERBOOT_CFLAGS) -c $< -o $@

$(SERVICE_USERBOOT_BIN): $(BUILD_DIR)/ub_entry.o $(BUILD_DIR)/ub_main.o $(BUILD_DIR)/ub_svc_loader.o services/userboot/link_userboot.ld
	ld -nostdlib -static -T services/userboot/link_userboot.ld \
	    $(BUILD_DIR)/ub_entry.o $(BUILD_DIR)/ub_main.o $(BUILD_DIR)/ub_svc_loader.o \
	    -o $(SERVICE_USERBOOT_BIN)

$(KERNEL_USERBOOT_BIN_OBJ): $(SERVICE_USERBOOT_BIN) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_USERBOOT_BIN) $@

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

# ── shared ring-3 ELF loader (linked into init + svcmgr) ─────────────────────
$(BUILD_DIR)/svc_loader.o: services/common/svc_loader.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(STACK_GUARD_OBJ): services/common/stack_guard.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

# ── svcmgr service ──────────────────────────────────────────────────────────
$(BUILD_DIR)/svcmgr_entry.o: services/svcmgr/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/svcmgr_main.o: services/svcmgr/svcmgr.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_SVCMGR_ELF): $(BUILD_DIR)/svcmgr_entry.o $(BUILD_DIR)/svcmgr_main.o $(BUILD_DIR)/svc_loader.o $(STACK_GUARD_OBJ)
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

$(SERVICE_VFS_ELF): $(BUILD_DIR)/vfs_entry.o $(BUILD_DIR)/vfs_main.o $(STACK_GUARD_OBJ)
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

$(SERVICE_INIT_ELF): $(BUILD_DIR)/init_entry.o $(BUILD_DIR)/init_main.o $(BUILD_DIR)/svc_loader.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_INIT_BIN_OBJ): $(SERVICE_INIT_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_INIT_ELF) $@

# ── console service ──────────────────────────────────────────────────────────
$(BUILD_DIR)/console_entry.o: services/console/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/console_main.o: services/console/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_CONSOLE_ELF): $(BUILD_DIR)/console_entry.o $(BUILD_DIR)/console_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_CONSOLE_BIN_OBJ): $(SERVICE_CONSOLE_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_CONSOLE_ELF) $@

# ── fb service (ring-3 framebuffer painter) ──────────────────────────────────
$(BUILD_DIR)/fb_svc_entry.o: services/fb/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/fb_svc_main.o: services/fb/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_FB_ELF): $(BUILD_DIR)/fb_svc_entry.o $(BUILD_DIR)/fb_svc_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_FB_SVC_BIN_OBJ): $(SERVICE_FB_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_FB_ELF) $@

# ── sh service (ring-3 interactive shell) ────────────────────────────────────
$(BUILD_DIR)/sh_entry.o: services/sh/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/sh_main.o: services/sh/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_SH_ELF): $(BUILD_DIR)/sh_entry.o $(BUILD_DIR)/sh_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_SH_BIN_OBJ): $(SERVICE_SH_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_SH_ELF) $@

# ── iris_test service (ring-3 syscall test suite, Block 8) ───────────────────
$(BUILD_DIR)/iris_test_entry.o: services/iris_test/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/iris_test_main.o: services/iris_test/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_IRIS_TEST_ELF): $(BUILD_DIR)/iris_test_entry.o $(BUILD_DIR)/iris_test_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@
	strip --strip-all $@

$(KERNEL_IRIS_TEST_BIN_OBJ): $(SERVICE_IRIS_TEST_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_IRIS_TEST_ELF) $@

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

smoke-runtime-selftests: all
	IRIS_QEMU_TIMEOUT_SECS=35 IRIS_QEMU_EXPECT_SELFTESTS=1 \
		IRIS_QEMU_LOG=$(BUILD_DIR)/qemu-headless-selftests.log \
		bash scripts/run_qemu_headless.sh

smoke-full: all
	IRIS_QEMU_TIMEOUT_SECS=90 \
		IRIS_QEMU_LOG=$(BUILD_DIR)/qemu-headless-full.log \
		bash scripts/run_qemu_headless.sh

smoke-full-selftests: all
	IRIS_QEMU_TIMEOUT_SECS=90 IRIS_QEMU_EXPECT_SELFTESTS=1 \
		IRIS_QEMU_LOG=$(BUILD_DIR)/qemu-headless-full-selftests.log \
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
	rm -f $(SERVICE_SVCMGR_ELF) $(SERVICE_KBD_ELF) $(SERVICE_VFS_ELF) $(SERVICE_INIT_ELF) $(SERVICE_SH_ELF) $(SERVICE_IRIS_TEST_ELF)

$(TEST_UNIT_BIN): $(TEST_UNIT_SRCS) | dirs
	gcc $(TEST_UNIT_CFLAGS) $(TEST_UNIT_SRCS) -o $@

test-unit: $(TEST_UNIT_BIN)
	@$(TEST_UNIT_BIN)

# ── Header dependency tracking ──────────────────────────────────────────────
# Generated by -MMD -MP in KERNEL_CFLAGS and SERVICE_CFLAGS.
# Each .o rule writes a .d file alongside itself; we include them here so that
# modifying any header triggers a rebuild of all dependent .o files without
# requiring 'make clean'.
-include $(wildcard $(BUILD_DIR)/*.d)
