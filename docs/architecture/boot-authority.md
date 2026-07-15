# IRIS — Boot Authority (Fase S1)

Cadena de autoridad desde el kernel hasta los servicios, con Untyped
explícito como presupuesto de creación de objetos.

## Cadena

```
kernel_main
  ├─ drena el PMM libre (menos IRIS_PMM_KERNEL_RUNTIME_RESERVE) en
  │  boot-KUntypeds → CSpace de userboot (slots BOOT_CPTR_UNTYPED_START..)
  │  + handles legacy (compat, ledger)
  └─ userboot: primera imagen (root task de facto)
userboot
  └─ init: spawn cap (slot 6) + UN boot-Untyped (slot 12)
init  (resuelve slot 12 una vez; g_init_untyped_h)
  ├─ console: EP propio (retype) en slot 5 + reply en slot 13 + KIoPort
  ├─ svcmgr: EP discovery (retype) + spawn cap + sub-untyped 256 KiB → slot 12
  ├─ fixtures de test (notification wrong-type, watch notif) por retype
  └─ iris_test: sub-untyped 8 MiB → slot 55 (+ caps de servicio badged)
svcmgr  (pool = slot 12)
  ├─ por servicio de catálogo: EP master + IRQ-notification retipados del pool
  ├─ por servicio servidor: reply sub-untyped de 4 KiB; en cada (re)boot:
  │  RESET + retype de reply object(s) fresco(s) → mint en slots 13(/14)
  │  y suelta sus handles (close-wakes-caller intacto en muerte del hijo)
  └─ su propio reply object en su slot 13 (discovery EP)
```

## Least authority

- Ningún servicio recibe el Untyped raíz; solo init/svcmgr administran
  pools acotados (administradores explícitos).
- El pager resuelve faults sin Untyped global; el VFS sirve archivos sin
  Untyped global (masks verificados en T156/T162/T201+; el reply del pager
  llega minteado por su supervisor).
- Los reply objects viajan minteados (slot 13/14); el supervisor nunca
  retiene copia.

## BootInfo

El BootInfo estructurado estilo seL4 (lista tipada de Untypeds + caps
iniciales en un único bloque) es trabajo de la fase root-task (ledger:
`KBootstrapCap` ACTIVE_LEGACY).  Hoy el contrato es el conjunto de slots
well-known de `endpoint_proto.h` + `boot_info.h`.
