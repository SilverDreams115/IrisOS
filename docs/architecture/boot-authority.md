# IRIS — Boot Authority (Fase S1)

Authority chain from the kernel down to the services, with explicit Untyped
as the object-creation budget.

## Chain

```
kernel_main
  ├─ drains the free PMM (minus IRIS_PMM_KERNEL_RUNTIME_RESERVE) into
  │  boot KUntypeds → userboot's CSpace (slots BOOT_CPTR_UNTYPED_START..)
  │  + legacy handles (compat, ledger)
  └─ userboot: first image (de facto root task)
userboot
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

The seL4-style structured BootInfo (a typed list of Untypeds + initial caps in
a single block) is the root-task phase's work (ledger: `KBootstrapCap`
ACTIVE_LEGACY). Today the contract is the set of well-known slots from
`endpoint_proto.h` + `boot_info.h`.
