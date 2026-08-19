# IRIS — Boot Authority (Fase S1)

Authority chain from the kernel down to the services, with explicit Untyped
as the object-creation budget.

## Chain

```
kernel_main
  ├─ drains the free PMM (minus IRIS_PMM_KERNEL_RUNTIME_RESERVE) into
  │  boot KUntypeds → userboot's CSpace ONLY (slots BOOT_CPTR_UNTYPED_START..;
  │  the legacy handle half is deleted — Stage 4)
  ├─ describes every grant in the BootInfo region and maps it read-only
  │  into userboot (address in RBX — Stage 5 Etapa 1)
  └─ userboot: first image (de facto root task)
userboot  (reads BootInfo; validates it against its own CSpace)
  └─ init: spawn cap (slot 6) + ONE boot Untyped (slot 12)
init  (resolves slot 12 once; g_init_untyped_h)
  ├─ console: own EP (retype) in slot 5 + reply in slot 13 + KIoPort
  ├─ svcmgr: discovery EP (retype) + spawn cap + 256 KiB sub-untyped → slot 12
  ├─ test fixtures (wrong-type notification, watch notif) via retype
  └─ iris_test: 8 MiB sub-untyped → slot 55 (+ badged service caps)
svcmgr  (pool = slot 12)
  ├─ per catalog service: EP master + IRQ notification retyped from the pool
  ├─ per server service: a 4 KiB reply sub-untyped; on each (re)boot:
  │  RESET + retype of fresh reply object(s) → mint into slots 13(/14)
  │  and drop their handles (close-wakes-caller intact on child death)
  └─ its own reply object in its slot 13 (discovery EP)
```

## Least authority

- No service receives the root Untyped; only init/svcmgr manage bounded pools
  (explicit administrators).
- The pager resolves faults with no global Untyped; the VFS serves files with
  no global Untyped (masks verified in T156/T162/T201+; the pager's reply
  arrives minted by its supervisor).
- Reply objects travel minted (slot 13/14); the supervisor never keeps a copy.

## BootInfo

**Stage 5 Etapa 1: the structured BootInfo exists.**  The kernel writes a
`struct iris_root_bootinfo` (`kernel/include/iris/root_bootinfo.h`) — initial
caps by CPtr, root-CNode shape, and every boot Untyped with its physical region
— and maps it read-only / non-executable into the root task, whose address
arrives in RBX.  `userboot` validates it against the CSpace it describes and
halts the boot on disagreement, so the chain above is now something the root
task READS rather than something it and the kernel separately assume.

The well-known slots of `endpoint_proto.h` remain the contract for the SPAWNED
services: a child's authority is the pre-start mint table its spawner passed,
which is already an explicit list.  BootInfo is for the root task, which has no
spawner.  Splitting `KBootstrapCap` into fine-grained caps published in their
own BootInfo slots is Etapa 2
(`docs/architecture/stage5-root-task-bootinfo.md`).
