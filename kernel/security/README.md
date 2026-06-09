# kernel/security — PLACEHOLDER (no implementado)

**Estado:** Vacío. Solo contiene subdirectorios `.gitkeep`.

**Riesgo arquitectónico:** Este directorio contradice el modelo microkernel de IRIS.

En un microkernel serio (seL4, Genode), la "seguridad" no es un subsistema del
kernel: es una consecuencia del modelo de capabilities. El kernel no tiene un
subsistema de audit, auth, identity ni session; esos conceptos viven en
servidores de usuario con acceso limitado por capabilities.

**Estado actual de seguridad en IRIS:**
- Capabilities: implementadas en `kernel/new_core/` (kobject, kcnode, rights.h)
- Control de acceso: por rights bits en cada handle (ver `nc/rights.h`)
- Audit: no implementado (ni en kernel ni en userland)
- Autenticación: no implementado
- Sesiones de usuario: no implementado

**Decisión Phase 0:** No implementar nada aquí.

**Decisión Phase 1+:** Si se necesita audit, implementarlo como servidor de
userland, no como módulo de kernel. Evaluar eliminar este directorio si
sigue vacío después de Phase 2.

Subdirectorios vacíos: `audit/`, `auth/`, `capabilities/`, `identity/`, `session/`
