# userland/ — PLACEHOLDER REDUNDANTE

**Estado:** Vacío. Solo contiene subdirectorios `.gitkeep`.

**Problema:** Este directorio contradice la estructura real del proyecto.

En IRIS, el código de userland vive en `services/`, no en `userland/`.
Los servicios actuales son:

| Directorio              | Descripción                      |
|-------------------------|----------------------------------|
| `services/userboot/`    | Primer proceso de usuario        |
| `services/svcmgr/`      | Service manager                  |
| `services/init/`        | Proceso init                     |
| `services/vfs/`         | Servidor VFS                     |
| `services/console/`     | Servidor de consola              |
| `services/fb/`          | Servidor de framebuffer          |
| `services/kbd/`         | Servidor de teclado              |
| `services/sh/`          | Shell interactivo                |
| `services/iris_test/`   | Servicio de pruebas              |

**Subdirectorios `userland/` y su realidad:**

- `apps/` — No existe ninguna aplicación. Las "apps" son servicios en `services/`.
- `init/` — El init real está en `services/init/`.
- `libc/` — No existe libc. Los servicios usan directamente las syscalls de IRIS.
  Una libc completa es trabajo de Phase 3+.
- `services/` — Los servicios reales están en el directorio raíz `services/`, no aquí.
- `shell/` — El shell real está en `services/sh/`.

**Decisión Phase 0:** No implementar nada aquí.

**Recomendación:** Evaluar eliminar `userland/` completamente en Phase 1.
La estructura correcta ya existe en `services/`.
