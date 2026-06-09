# kernel/lib — PLACEHOLDER (no implementado)

**Estado:** Vacío. Solo contiene subdirectorios `.gitkeep`.

**Propósito previsto:** Utilidades compartidas por el kernel que no encajan en
ningún subsistema específico.

**Subdirectorios y su realidad:**

- `bitmap/` — No implementado. El planificador usa una bitmap manual en CpuRunQueue.mask[4].
  Si se generaliza, la implementación iría aquí.
- `elf/` — No implementado. El kernel no parsea ELF actualmente; los servicios se
  cargan como binarios planos desde el initrd. Si se agrega un loader ELF al kernel,
  va aquí.
- `printf/` — No implementado como lib separada. El kernel usa serial.c + klog.c
  directamente. Si se necesita un printf freestanding, va aquí.
- `string/` — No implementado. El kernel usa operaciones manuales de copia de memoria.
  Si se agrega memcpy/memset como función, va aquí.

**Decisión Phase 0:** No implementar. Los usos actuales no requieren esta abstracción.

**Decisión futura:** Implementar solo si hay duplicación clara entre dos subsistemas.
No crear por anticipación.
