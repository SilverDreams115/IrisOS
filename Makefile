SHELL := /usr/bin/env bash

ENABLE_RUNTIME_SELFTESTS ?= 0

BUILD_DIR    := build
EFI_ROOT     := $(BUILD_DIR)/efi_root
EFI_BOOT_DIR := $(EFI_ROOT)/EFI/BOOT
EFI_IRIS_DIR := $(EFI_ROOT)/EFI/IRIS
BUILD_CONFIG_STAMP := $(BUILD_DIR)/.build_config
BUILD_CONFIG_MODE  := ENABLE_RUNTIME_SELFTESTS=$(ENABLE_RUNTIME_SELFTESTS)

# Stale-config cleanup must run at PARSE time, not as a recipe: GNU make
# stat-caches targets while building the dependency graph, so a recipe-time
# `rm build/*.o` (the old config-sync) deleted objects make had already seen
# as up to date — the first link after toggling ENABLE_RUNTIME_SELFTESTS
# failed on missing *_bin.o and only the second invocation passed. Running
# the cleanup here, before any target is considered (and before the *.d
# includes at the bottom are expanded), removes the race entirely.
BUILD_CONFIG_ON_DISK := $(strip $(shell cat $(BUILD_CONFIG_STAMP) 2>/dev/null))
ifneq ($(BUILD_CONFIG_ON_DISK),$(BUILD_CONFIG_MODE))
  $(info [build] configuration changed to $(BUILD_CONFIG_MODE); cleaning stale artifacts)
  $(shell rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.so $(BUILD_DIR)/*.elf $(BUILD_DIR)/*.d $(BUILD_DIR)/OVMF_VARS.fd)
  # The boot IMAGE is deliberately not deleted.  Everything in it — BOOTX64.EFI
  # and KERNEL.ELF — is produced by an ordinary rule from a file this line
  # above has just removed, so make rebuilds both either way; the `rm -rf` was
  # redundant.  It was also the one thing here that could leave the image half
  # made: make caches directory listings, so a tree emptied at parse time can
  # still be believed to hold its files, and the run boots to the UEFI shell
  # with no kernel and no explanation.  Seen for real after `make test-unit`
  # (which flips this config) and before a selftest run.
  $(shell rm -f services/svcmgr/svcmgr.elf services/kbd/kbd.elf services/vfs/vfs.elf services/init/init.elf services/console/console.elf services/fb/fb.elf services/sh/sh.elf services/iris_test/iris_test.elf services/lifecycle_probe/lifecycle_probe.elf services/pager/pager.elf services/userboot/userboot.bin)
  $(shell mkdir -p $(BUILD_DIR))
  $(shell printf '%s\n' "$(BUILD_CONFIG_MODE)" > $(BUILD_CONFIG_STAMP))
endif

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
KERNEL_UCOPYASM_OBJ  := $(BUILD_DIR)/usercopy_asm.o
KERNEL_FPU_OBJ       := $(BUILD_DIR)/fpu_switch.o
KERNEL_SCHED_OBJ     := $(BUILD_DIR)/scheduler.o
KERNEL_COREDISP_OBJ  := $(BUILD_DIR)/core_dispatch.o
KERNEL_LIFECYCLE_OBJ := $(BUILD_DIR)/task_lifecycle.o
KERNEL_TRAMP_OBJ     := $(BUILD_DIR)/user_trampoline.o
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
KERNEL_FBCON_OBJ          := $(BUILD_DIR)/fbcon.o
KERNEL_NC_KOBJECT_OBJ    := $(BUILD_DIR)/nc_kobject.o
KERNEL_NC_KNOTIF_OBJ     := $(BUILD_DIR)/nc_knotification.o
KERNEL_NC_KBOOTCAP_OBJ   := $(BUILD_DIR)/nc_kbootcap.o
KERNEL_NC_KFAULT_OBJ     := $(BUILD_DIR)/nc_kfault.o
KERNEL_NC_KIRQCAP_OBJ       := $(BUILD_DIR)/nc_kirqcap.o
KERNEL_NC_KIOPORT_OBJ       := $(BUILD_DIR)/nc_kioport.o
KERNEL_NC_KENDPOINT_OBJ     := $(BUILD_DIR)/nc_kendpoint.o
KERNEL_SYSCALL_EP_OBJ       := $(BUILD_DIR)/syscall_endpoint.o
KERNEL_ACPI_OBJ          := $(BUILD_DIR)/acpi.o
KERNEL_IOMMU_OBJ         := $(BUILD_DIR)/iommu.o
KERNEL_SMP_OBJ           := $(BUILD_DIR)/smp.o
KERNEL_APTRAMP_OBJ       := $(BUILD_DIR)/ap_trampoline.o
KERNEL_TLB_OBJ           := $(BUILD_DIR)/tlb.o
KERNEL_IRQROUTING_OBJ    := $(BUILD_DIR)/irq_routing.o
KERNEL_PHASE3_SELFTEST_OBJ := $(BUILD_DIR)/phase3_selftest.o
KERNEL_INITRD_OBJ         := $(BUILD_DIR)/initrd.o
KERNEL_KSLAB_OBJ          := $(BUILD_DIR)/kslab.o
KERNEL_KLOG_OBJ           := $(BUILD_DIR)/klog.o
KERNEL_PANIC_OBJ          := $(BUILD_DIR)/panic.o
KERNEL_LAPIC_OBJ          := $(BUILD_DIR)/lapic.o
STACK_GUARD_OBJ           := $(BUILD_DIR)/stack_guard.o
KERNEL_NC_KREPLY_OBJ         := $(BUILD_DIR)/nc_kreply.o
KERNEL_NC_KCNODE_OBJ         := $(BUILD_DIR)/nc_kcnode.o
KERNEL_NC_KASIDPOOL_OBJ      := $(BUILD_DIR)/nc_kasidpool.o
KERNEL_NC_KSCHEDCTX_OBJ      := $(BUILD_DIR)/nc_kschedctx.o
KERNEL_NC_KUNTYPED_OBJ       := $(BUILD_DIR)/nc_kuntyped.o
KERNEL_SYSCALL_CSPACE_OBJ    := $(BUILD_DIR)/syscall_cspace.o
KERNEL_SYSCALL_SCHED_OBJ     := $(BUILD_DIR)/syscall_sched.o
KERNEL_SYSCALL_UNTYPED_OBJ   := $(BUILD_DIR)/syscall_untyped.o
KERNEL_SYSCALL_REPLY_OBJ     := $(BUILD_DIR)/syscall_reply.o
KERNEL_SYSCALL_CNODE_OPS_OBJ := $(BUILD_DIR)/syscall_cnode_ops.o
KERNEL_SYSCALL_INVOKE_OBJ    := $(BUILD_DIR)/syscall_invoke.o
KERNEL_NC_KTCB_OBJ           := $(BUILD_DIR)/nc_ktcb.o
KERNEL_SYSCALL_TCB_OBJ       := $(BUILD_DIR)/syscall_tcb.o
KERNEL_NC_CSPACE_OBJ         := $(BUILD_DIR)/nc_cspace.o
KERNEL_NC_KVSPACE_OBJ        := $(BUILD_DIR)/nc_kvspace.o
KERNEL_NC_KFRAME_OBJ         := $(BUILD_DIR)/nc_kframe.o
KERNEL_NC_ROOT_BOOTINFO_OBJ  := $(BUILD_DIR)/nc_root_bootinfo.o
KERNEL_NC_KIOSPACE_OBJ    := $(BUILD_DIR)/nc_kiospace.o
KERNEL_NC_KIOPT_OBJ       := $(BUILD_DIR)/nc_kiopagetable.o
KERNEL_NC_KPAGETABLE_OBJ     := $(BUILD_DIR)/nc_kpagetable.o
KERNEL_SYSCALL_FRAME_OBJ     := $(BUILD_DIR)/syscall_frame.o
KERNEL_SYSCALL_IOSPACE_OBJ   := $(BUILD_DIR)/syscall_iospace.o
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
SERVICE_LIFECYCLE_PROBE_ELF := services/lifecycle_probe/lifecycle_probe.elf
SERVICE_PAGER_ELF     := services/pager/pager.elf
SERVICE_TIMER_ELF     := services/timer/timer.elf
SERVICE_PCI_ELF       := services/pci/pci.elf
SERVICE_BLK_ELF       := services/blk/blk.elf
SERVICE_NET_ELF       := services/net/net.elf
SERVICE_FS_ELF        := services/fs/fs.elf
SERVICE_IP_ELF        := services/ip/ip.elf
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
KERNEL_LIFECYCLE_PROBE_BIN_OBJ := $(BUILD_DIR)/lifecycle_probe_bin.o
KERNEL_PAGER_BIN_OBJ      := $(BUILD_DIR)/pager_bin.o
KERNEL_TIMER_BIN_OBJ      := $(BUILD_DIR)/timer_bin.o
KERNEL_PCI_BIN_OBJ        := $(BUILD_DIR)/pci_bin.o
KERNEL_BLK_BIN_OBJ        := $(BUILD_DIR)/blk_bin.o
KERNEL_NET_BIN_OBJ        := $(BUILD_DIR)/net_bin.o
KERNEL_FS_BIN_OBJ         := $(BUILD_DIR)/fs_bin.o
KERNEL_IP_BIN_OBJ         := $(BUILD_DIR)/ip_bin.o
KERNEL_BOOTFIX_BADELF_OBJ := $(BUILD_DIR)/bootfix_badelf_bin.o
KERNEL_FILEBK_FBK_OBJ     := $(BUILD_DIR)/filebk_fbk_bin.o
KERNEL_FILEBK_FBK2_OBJ    := $(BUILD_DIR)/filebk_fbk2_bin.o
KERNEL_FILEBK_ELFSEG_OBJ  := $(BUILD_DIR)/filebk_elfseg_bin.o
KERNEL_FILEBK_SMALL_OBJ   := $(BUILD_DIR)/filebk_small_bin.o
KERNEL_USERBOOT_BIN_OBJ   := $(BUILD_DIR)/userboot_bin.o
KERNEL_DEMO_OBJS :=
KERNEL_DEMO_DEFINES :=
ifeq ($(ENABLE_RUNTIME_SELFTESTS),1)
KERNEL_DEMO_DEFINES      += -DIRIS_ENABLE_RUNTIME_SELFTESTS
endif
KERNEL_OBJS := $(KERNEL_ENTRY_OBJ) $(KERNEL_MAIN_OBJ) $(KERNEL_KSLAB_OBJ) $(KERNEL_PMM_OBJ) $(KERNEL_PAGING_OBJ) $(KERNEL_GDT_OBJ) $(KERNEL_IDT_OBJ) $(KERNEL_PIC_OBJ) $(KERNEL_GDT_FLUSH_OBJ) $(KERNEL_ISR_OBJ) $(KERNEL_FPU_OBJ) $(KERNEL_SCHED_OBJ) $(KERNEL_COREDISP_OBJ) $(KERNEL_LIFECYCLE_OBJ) $(KERNEL_TRAMP_OBJ) $(KERNEL_SYSCALL_DISPATCH_OBJ) $(KERNEL_SYSCALL_IPC_OBJ) $(KERNEL_SYSCALL_VM_OBJ) $(KERNEL_SYSCALL_PROC_OBJ) $(KERNEL_SYSCALL_CAP_OBJ) $(KERNEL_SYSCALL_IRQ_OBJ) $(KERNEL_SYSCALL_DIAG_OBJ) $(KERNEL_SYSCALL_EP_OBJ) $(KERNEL_USERCOPY_OBJ) $(KERNEL_UCOPYASM_OBJ) $(KERNEL_SYSCALLE_OBJ) $(KERNEL_SERIAL_OBJ) $(KERNEL_FBCON_OBJ) $(KERNEL_NC_KOBJECT_OBJ) $(KERNEL_NC_KNOTIF_OBJ) $(KERNEL_NC_KBOOTCAP_OBJ) $(KERNEL_NC_KFAULT_OBJ) $(KERNEL_NC_KIRQCAP_OBJ) $(KERNEL_NC_KIOPORT_OBJ) $(KERNEL_NC_KENDPOINT_OBJ) $(KERNEL_ACPI_OBJ) $(KERNEL_IOMMU_OBJ) $(KERNEL_SMP_OBJ) $(KERNEL_APTRAMP_OBJ) $(KERNEL_TLB_OBJ) $(KERNEL_IRQROUTING_OBJ) $(KERNEL_PHASE3_SELFTEST_OBJ) $(KERNEL_INITRD_OBJ) $(KERNEL_KLOG_OBJ) $(KERNEL_PANIC_OBJ) $(KERNEL_LAPIC_OBJ) $(KERNEL_NC_KREPLY_OBJ) $(KERNEL_NC_KCNODE_OBJ) $(KERNEL_NC_KSCHEDCTX_OBJ) $(KERNEL_NC_KASIDPOOL_OBJ) $(KERNEL_NC_KUNTYPED_OBJ) $(KERNEL_SYSCALL_CSPACE_OBJ) $(KERNEL_SYSCALL_SCHED_OBJ) $(KERNEL_SYSCALL_UNTYPED_OBJ) $(KERNEL_SYSCALL_REPLY_OBJ) $(KERNEL_SYSCALL_CNODE_OPS_OBJ) $(KERNEL_SYSCALL_INVOKE_OBJ) $(KERNEL_NC_KTCB_OBJ) $(KERNEL_SYSCALL_TCB_OBJ) $(KERNEL_NC_CSPACE_OBJ) $(KERNEL_NC_KVSPACE_OBJ) $(KERNEL_NC_KFRAME_OBJ) $(KERNEL_NC_ROOT_BOOTINFO_OBJ) $(KERNEL_NC_KPAGETABLE_OBJ) $(KERNEL_NC_KIOSPACE_OBJ) $(KERNEL_NC_KIOPT_OBJ) $(KERNEL_SYSCALL_FRAME_OBJ) $(KERNEL_SYSCALL_IOSPACE_OBJ) $(KERNEL_USERBOOT_BIN_OBJ) $(KERNEL_SVCMGR_BIN_OBJ) $(KERNEL_KBD_BIN_OBJ) $(KERNEL_VFS_BIN_OBJ) $(KERNEL_INIT_BIN_OBJ) $(KERNEL_CONSOLE_BIN_OBJ) $(KERNEL_FB_SVC_BIN_OBJ) $(KERNEL_SH_BIN_OBJ) $(KERNEL_IRIS_TEST_BIN_OBJ) $(KERNEL_LIFECYCLE_PROBE_BIN_OBJ) $(KERNEL_PAGER_BIN_OBJ) $(KERNEL_TIMER_BIN_OBJ) $(KERNEL_PCI_BIN_OBJ) $(KERNEL_BLK_BIN_OBJ) $(KERNEL_NET_BIN_OBJ) $(KERNEL_FS_BIN_OBJ) $(KERNEL_IP_BIN_OBJ) $(KERNEL_BOOTFIX_BADELF_OBJ) $(KERNEL_FILEBK_FBK_OBJ) $(KERNEL_FILEBK_FBK2_OBJ) $(KERNEL_FILEBK_ELFSEG_OBJ) $(KERNEL_FILEBK_SMALL_OBJ) $(KERNEL_DEMO_OBJS)
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

# -Werror makes the zero-warning policy a build guarantee rather than a
# convention.  README and CONTRIBUTING both state that a diagnostic means a
# broken build; before this line nothing enforced it and a careless commit
# could land warnings silently.  Override for a bisect or a toolchain bump
# with `make IRIS_WERROR=`.
IRIS_WERROR     ?= -Werror
COMMON_WARNINGS := -Wall -Wextra -Wshadow -Wundef $(IRIS_WERROR)
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
TEST_UNIT_CFLAGS := -D__KERNEL__ -Wall -Wextra -Wshadow -std=c11 $(IRIS_WERROR) \
    -Wno-unused-function \
    $(TEST_UNIT_INCS)
TEST_UNIT_SRCS  := \
    tests/kernel/stubs.c \
    kernel/new_core/src/kobject.c \
    kernel/new_core/src/kcnode.c \
    kernel/new_core/src/kuntyped.c \
    kernel/new_core/src/kbootcap.c \
    kernel/new_core/src/kendpoint.c \
    kernel/new_core/src/knotification.c \
    kernel/new_core/src/kreply.c \
    kernel/new_core/src/kschedctx.c \
    kernel/new_core/src/kasidpool.c \
    kernel/new_core/src/cspace.c \
    kernel/new_core/src/kvspace.c \
    kernel/new_core/src/kframe.c \
    kernel/new_core/src/root_bootinfo.c \
    kernel/new_core/src/kpagetable.c \
    kernel/new_core/src/kiopagetable.c \
    kernel/new_core/src/kiospace.c \
    kernel/core/syscall/syscall_cspace.c \
    kernel/core/syscall/syscall_untyped.c \
    kernel/new_core/src/kioport.c \
    kernel/new_core/src/kirqcap.c \
    kernel/new_core/src/ktcb.c \
    kernel/new_core/src/kfault.c \
    kernel/core/irq/irq_routing.c \
    kernel/core/syscall/syscall_cap.c \
    kernel/core/syscall/syscall_tcb.c \
    kernel/core/syscall/syscall_sched.c \
    kernel/core/syscall/syscall_reply.c \
    kernel/core/syscall/syscall_endpoint.c \
    kernel/core/syscall/syscall_proc.c \
    kernel/core/syscall/syscall_frame.c \
    kernel/core/syscall/syscall_iospace.c \
    kernel/core/syscall/syscall_cnode_ops.c \
    kernel/core/syscall/syscall_invoke.c \
    kernel/core/syscall/syscall_irq.c \
    kernel/core/syscall/syscall_vm.c \
    kernel/core/syscall/syscall_ipc.c \
    kernel/core/syscall/syscall_diag.c \
    kernel/core/syscall/syscall_dispatch.c \
    tests/kernel/test_rights.c \
    tests/kernel/test_kobject.c \
    tests/kernel/test_kcnode.c \
    tests/kernel/test_kuntyped.c \
    tests/kernel/test_kendpoint.c \
    tests/kernel/test_knotification.c \
    tests/kernel/test_kreply.c \
    tests/kernel/test_kschedctx.c \
    tests/kernel/test_cspace.c \
    tests/kernel/test_ipc_cspace.c \
    tests/kernel/test_untyped_cspace.c \
    tests/kernel/test_boot_cspace.c \
    tests/kernel/test_vspace_cspace.c \
    tests/kernel/test_kasidpool.c \
    tests/kernel/test_kframe.c \
    tests/kernel/test_mdb.c \
    tests/kernel/test_cnode_depth.c \
    tests/kernel/test_parent_identity.c \
    tests/kernel/test_root_bootinfo.c \
    tests/kernel/test_pagetable.c \
    tests/kernel/test_cnode_guard.c \
    tests/kernel/test_schedctx_refill.c \
    tests/kernel/test_syscall_cspace.c \
    tests/kernel/test_syscall_retype.c \
    tests/kernel/test_syscall_tcb.c \
    tests/kernel/test_syscall_ipc.c \
    tests/kernel/test_syscall_dispatch.c \
    tests/kernel/test_abi.c \
    tests/kernel/test_klog.c \
    kernel/core/klog/klog.c \
    kernel/drivers/fbcon/fbcon.c \
    services/vfs/vfs_ep.c \
    tests/kernel/test_vfs_ep.c \
    tests/kernel/test_main.c
TEST_UNIT_BIN   := $(BUILD_DIR)/test_unit

.PHONY: all dirs run run-headless clean help check check-purity smoke smoke-runtime smoke-runtime-selftests smoke-persist smoke-screen config-sync test-unit

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

# Cleanup of stale artifacts happens at parse time (see BUILD_CONFIG_ON_DISK
# above); this target only records the configuration the build ran with.
config-sync:
	@mkdir -p $(BUILD_DIR)
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

$(KERNEL_UCOPYASM_OBJ): kernel/arch/x86_64/usercopy_asm.S | dirs
	gcc $(KERNEL_ASFLAGS) -c kernel/arch/x86_64/usercopy_asm.S -o $@

$(KERNEL_ISR_OBJ): kernel/arch/x86_64/isr_stubs.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_FPU_OBJ): kernel/arch/x86_64/fpu_switch.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

$(KERNEL_SCHED_OBJ): kernel/core/scheduler/scheduler.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_COREDISP_OBJ): kernel/core/scheduler/core_dispatch.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_LIFECYCLE_OBJ): kernel/core/scheduler/task_lifecycle.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_TRAMP_OBJ): kernel/arch/x86_64/user_trampoline.S | dirs
	gcc $(KERNEL_ASFLAGS) -c $< -o $@

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

$(KERNEL_FBCON_OBJ): kernel/drivers/fbcon/fbcon.c | dirs
	gcc $(KERNEL_CFLAGS) -c kernel/drivers/fbcon/fbcon.c -o $@

$(KERNEL_SERIAL_OBJ): kernel/drivers/serial/serial.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KOBJECT_OBJ): kernel/new_core/src/kobject.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@




$(KERNEL_NC_KNOTIF_OBJ): kernel/new_core/src/knotification.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KBOOTCAP_OBJ): kernel/new_core/src/kbootcap.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KFAULT_OBJ): kernel/new_core/src/kfault.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KIRQCAP_OBJ): kernel/new_core/src/kirqcap.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KIOPORT_OBJ): kernel/new_core/src/kioport.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KENDPOINT_OBJ): kernel/new_core/src/kendpoint.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_EP_OBJ): kernel/core/syscall/syscall_endpoint.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_ACPI_OBJ): kernel/core/acpi/acpi.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_IOMMU_OBJ): kernel/core/iommu/iommu.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SMP_OBJ): kernel/core/smp/smp.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_APTRAMP_OBJ): kernel/arch/x86_64/ap_trampoline.S | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_TLB_OBJ): kernel/core/tlb/tlb.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_IRQROUTING_OBJ): kernel/core/irq/irq_routing.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@


$(KERNEL_PHASE3_SELFTEST_OBJ): kernel/core/phase3_selftest.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_INITRD_OBJ): kernel/core/initrd/initrd.c | dirs
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

$(KERNEL_NC_KASIDPOOL_OBJ): kernel/new_core/src/kasidpool.c | dirs
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

$(KERNEL_SYSCALL_INVOKE_OBJ): kernel/core/syscall/syscall_invoke.c kernel/core/syscall/syscall_priv.h kernel/include/iris/invoke.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KTCB_OBJ): kernel/new_core/src/ktcb.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_TCB_OBJ): kernel/core/syscall/syscall_tcb.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_CSPACE_OBJ): kernel/new_core/src/cspace.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KVSPACE_OBJ): kernel/new_core/src/kvspace.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KFRAME_OBJ): kernel/new_core/src/kframe.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_ROOT_BOOTINFO_OBJ): kernel/new_core/src/root_bootinfo.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KIOSPACE_OBJ): kernel/new_core/src/kiospace.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KIOPT_OBJ): kernel/new_core/src/kiopagetable.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_NC_KPAGETABLE_OBJ): kernel/new_core/src/kpagetable.c | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_IOSPACE_OBJ): kernel/core/syscall/syscall_iospace.c kernel/core/syscall/syscall_priv.h | dirs
	gcc $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_SYSCALL_FRAME_OBJ): kernel/core/syscall/syscall_frame.c kernel/core/syscall/syscall_priv.h | dirs
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
$(BUILD_DIR)/kbd_main.o: services/kbd/main.S kernel/include/iris/ipc_msg.h kernel/include/iris/invoke.h kernel/include/iris/syscall.h | dirs
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

$(BUILD_DIR)/vfs_ep.o: services/vfs/vfs_ep.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_VFS_ELF): $(BUILD_DIR)/vfs_entry.o $(BUILD_DIR)/vfs_main.o $(BUILD_DIR)/vfs_ep.o $(STACK_GUARD_OBJ)
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

$(BUILD_DIR)/init_test.o: services/init/init_test.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/init_bootstrap.o: services/init/init_bootstrap.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/init_launch.o: services/init/init_launch.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_INIT_ELF): $(BUILD_DIR)/init_entry.o $(BUILD_DIR)/init_main.o $(BUILD_DIR)/init_bootstrap.o $(BUILD_DIR)/init_launch.o $(BUILD_DIR)/init_test.o $(BUILD_DIR)/svc_loader.o $(STACK_GUARD_OBJ)
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

# ── lifecycle_probe (ring-3 TEST child for the spawn/kill lifecycle harness) ──
$(BUILD_DIR)/lifecycle_probe_entry.o: services/lifecycle_probe/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/lifecycle_probe_main.o: services/lifecycle_probe/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_LIFECYCLE_PROBE_ELF): $(BUILD_DIR)/lifecycle_probe_entry.o $(BUILD_DIR)/lifecycle_probe_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_LIFECYCLE_PROBE_BIN_OBJ): $(SERVICE_LIFECYCLE_PROBE_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_LIFECYCLE_PROBE_ELF) $@

# ── pager (ring-3 user pager SERVICE, own binary since Phase 28) ──────────────
$(BUILD_DIR)/pager_entry.o: services/pager/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/pager_main.o: services/pager/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_PAGER_ELF): $(BUILD_DIR)/pager_entry.o $(BUILD_DIR)/pager_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_PAGER_BIN_OBJ): $(SERVICE_PAGER_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_PAGER_ELF) $@

# ── timer (ledger A-24: waiting is a service, not a syscall) ─────────────────
$(BUILD_DIR)/timer_entry.o: services/timer/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/timer_main.o: services/timer/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_TIMER_ELF): $(BUILD_DIR)/timer_entry.o $(BUILD_DIR)/timer_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_TIMER_BIN_OBJ): $(SERVICE_TIMER_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_TIMER_ELF) $@

# ── pci (Stage 10: the bus is a service, not a library every driver links) ───
$(BUILD_DIR)/pci_entry.o: services/pci/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/pci_main.o: services/pci/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_PCI_ELF): $(BUILD_DIR)/pci_entry.o $(BUILD_DIR)/pci_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_PCI_BIN_OBJ): $(SERVICE_PCI_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_PCI_ELF) $@

# ── blk (Stage 10: an AHCI disk driver in ring 3) ───────────────────────────
$(BUILD_DIR)/blk_entry.o: services/blk/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/blk_main.o: services/blk/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_BLK_ELF): $(BUILD_DIR)/blk_entry.o $(BUILD_DIR)/blk_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_BLK_BIN_OBJ): $(SERVICE_BLK_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_BLK_ELF) $@

# ── net (Stage 10: an e1000 driver in ring 3) ───────────────────────────────
$(BUILD_DIR)/net_entry.o: services/net/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/net_main.o: services/net/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_NET_ELF): $(BUILD_DIR)/net_entry.o $(BUILD_DIR)/net_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_NET_BIN_OBJ): $(SERVICE_NET_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_NET_ELF) $@

# ── fs (Stage 10: a filesystem that survives the power going off) ───────────
$(BUILD_DIR)/fs_entry.o: services/fs/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_main.o: services/fs/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_FS_ELF): $(BUILD_DIR)/fs_entry.o $(BUILD_DIR)/fs_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_FS_BIN_OBJ): $(SERVICE_FS_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_FS_ELF) $@

# ── ip (Stage 10: ARP/IPv4/UDP as a service above a driver that parses nothing)
$(BUILD_DIR)/ip_entry.o: services/ip/entry.S | dirs
	gcc $(SERVICE_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/ip_main.o: services/ip/main.c | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_IP_ELF): $(BUILD_DIR)/ip_entry.o $(BUILD_DIR)/ip_main.o $(STACK_GUARD_OBJ)
	ld $(SERVICE_LDFLAGS) $^ -o $@

$(KERNEL_IP_BIN_OBJ): $(SERVICE_IP_ELF) | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(SERVICE_IP_ELF) $@

# ── boot-growth test fixtures (Phase 28): non-service initrd blobs ────────────
# badelf.bin is a 256-byte invalid-ELF blob (a known header + zero pad) used by
# the loader failure-path test (T216).  It is *.bin-gitignored, so it is
# generated deterministically here rather than committed — a fresh checkout (CI)
# rebuilds it byte-for-byte instead of failing on a missing fixture.
services/bootfix/badelf.bin: | dirs
	mkdir -p services/bootfix
	printf 'IRIS-BOOTFIX-BADELF-v1' > $@
	truncate -s 256 $@

$(KERNEL_BOOTFIX_BADELF_OBJ): services/bootfix/badelf.bin | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    services/bootfix/badelf.bin $@

# ── file-backed content fixtures (Phase 28 Bloque B) ─────────────────────────
$(KERNEL_FILEBK_FBK_OBJ): services/filebk/fbk.dat | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    services/filebk/fbk.dat $@

$(KERNEL_FILEBK_FBK2_OBJ): services/filebk/fbk2.dat | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    services/filebk/fbk2.dat $@

$(KERNEL_FILEBK_ELFSEG_OBJ): services/filebk/elfseg.dat | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    services/filebk/elfseg.dat $@

$(KERNEL_FILEBK_SMALL_OBJ): services/filebk/small.dat | dirs
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    services/filebk/small.dat $@


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

# The suite is nine translation units split by area (see it_priv.h); one
# pattern rule builds them all, and adding a tenth is adding a file.
IRIS_TEST_SRCS := $(sort $(wildcard services/iris_test/*.c))
IRIS_TEST_OBJS := $(patsubst services/iris_test/%.c,$(BUILD_DIR)/iris_test_%.o,$(IRIS_TEST_SRCS))

$(BUILD_DIR)/iris_test_%.o: services/iris_test/%.c services/iris_test/it_priv.h | dirs
	gcc $(SERVICE_CFLAGS) -c $< -o $@

$(SERVICE_IRIS_TEST_ELF): $(BUILD_DIR)/iris_test_entry.o $(IRIS_TEST_OBJS) $(BUILD_DIR)/svc_loader.o $(STACK_GUARD_OBJ)
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

# seL4 purity charter guard: the legacy handle-table / kslab consumers are
# frozen in scripts/purity_allowlist.txt (it can only shrink).
check-purity:
	bash scripts/check_purity.sh

# The lock hierarchy of the SMP roadmap (§9.1).  Checked statically because a
# lock-order inversion cannot happen on one core: it is invisible until the
# second one starts, and then it is a hang.
check-locks:
	python3 scripts/check_lock_order.py

smoke:
	bash scripts/smoke_local.sh

smoke-runtime: all
	bash scripts/run_qemu_headless.sh

# The budget is 90s and a caller's own is honoured.
#
# It was a hardcoded 35, which stopped being enough twice over: Stage 9 step 5
# added four adversarial tests that wait out eight ticks of REAL time apiece,
# and Stage 10-dma step 6 added a driver that waits out six DMA transfers the
# device schedules a hundred milliseconds apart.  The suite takes about fifty
# seconds on one processor now.  A hardcode also silently ignored the
# `IRIS_QEMU_TIMEOUT_SECS=60` that CI passes on the command line, which is how
# a lane can be tuned and never change.
smoke-runtime-selftests: all
	IRIS_QEMU_TIMEOUT_SECS="$${IRIS_QEMU_TIMEOUT_SECS:-90}" IRIS_QEMU_EXPECT_SELFTESTS=1 \
		IRIS_QEMU_LOG=$(BUILD_DIR)/qemu-headless-selftests.log \
		bash scripts/run_qemu_headless.sh

smoke-full: all
	IRIS_QEMU_TIMEOUT_SECS="$${IRIS_QEMU_TIMEOUT_SECS:-90}" \
		IRIS_QEMU_LOG=$(BUILD_DIR)/qemu-headless-full.log \
		bash scripts/run_qemu_headless.sh

smoke-full-selftests: all
	IRIS_QEMU_TIMEOUT_SECS="$${IRIS_QEMU_TIMEOUT_SECS:-90}" IRIS_QEMU_EXPECT_SELFTESTS=1 \
		IRIS_QEMU_LOG=$(BUILD_DIR)/qemu-headless-full-selftests.log \
		bash scripts/run_qemu_headless.sh

# The one claim that takes two boots to check.  See the script's header.
smoke-persist: all
	bash scripts/check_persistence.sh

# The one claim that cannot be checked over the serial port, because it is the
# claim that the serial port is not needed.  See the script's header.
smoke-screen: all
	bash scripts/check_screen.sh

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
# The Stage 10 services were left out of the line above as each was added,
# so `clean` did not clean them: a stale `services/*/[name].elf` is relinked
# into the kernel by a build that looks clean.
	rm -f $(SERVICE_PCI_ELF) $(SERVICE_BLK_ELF) $(SERVICE_NET_ELF) $(SERVICE_FS_ELF) $(SERVICE_IP_ELF)

# Makefile is a real prerequisite: the source list and the flags live in it, so
# editing either must relink.  Without this, adding a translation unit to
# TEST_UNIT_SRCS leaves the previous binary in place and `make test-unit` runs
# it — reporting a result for a build that no longer exists.  That cost a
# debugging session once; it is one line.
$(TEST_UNIT_BIN): $(TEST_UNIT_SRCS) Makefile | dirs
	gcc $(TEST_UNIT_CFLAGS) $(TEST_UNIT_SRCS) -o $@

test-unit: $(TEST_UNIT_BIN)
	@$(TEST_UNIT_BIN)

# ── Header dependency tracking ──────────────────────────────────────────────
# Generated by -MMD -MP in KERNEL_CFLAGS and SERVICE_CFLAGS.
# Each .o rule writes a .d file alongside itself; we include them here so that
# modifying any header triggers a rebuild of all dependent .o files without
# requiring 'make clean'.
-include $(wildcard $(BUILD_DIR)/*.d)
