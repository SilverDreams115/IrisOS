# kernel/fs — PLACEHOLDER (no implementado)

**Estado:** Vacío. Solo contiene subdirectorios `.gitkeep`.

**Riesgo arquitectónico:** Este directorio contradice el modelo microkernel de IRIS.

En IRIS, el filesystem es responsabilidad del servidor VFS (`services/vfs/`),
no del kernel. El kernel no implementa ni debe implementar ningún filesystem:
su única interfaz de "storage" es el initrd, manejado por `kernel/core/initrd/`.

**Subdirectorios y su realidad:**

- `irisfs/` — Placeholder de un hipotético filesystem nativo IRIS. No existe código.
  El equipo no debe comenzar irisfs hasta que el servidor VFS tenga una interfaz
  estable y un caso de uso concreto (persistencia vs. RAM).
- `ramfs/` — Placeholder de ramfs en kernel. El initrd cumple esta función hoy.
  Un ramfs de userland (en VFS server) sería la ubicación correcta.
- `vfs/` — El VFS real está en `services/vfs/`, NO aquí. Este directorio está vacío.

**Decisión Phase 0:** No implementar nada aquí.

**Decisión futura:** Evaluar eliminar `kernel/fs/` completamente. El VFS vive en
`services/vfs/`. Si se necesita un filesystem en kernel (ej. para debug temprano),
documentarlo como excepción justificada antes de agregar código.
