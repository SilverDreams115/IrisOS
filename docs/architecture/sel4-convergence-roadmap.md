# IRIS — Roadmap normativo de convergencia seL4 (por dependencias, sin fechas)

Ordena las etapas hacia el [charter de pureza](iris-sel4-purity-charter.md).
Cada etapa declara su **precondición técnica** (qué debe estar cerrado antes)
y su **criterio de cierre** (qué debe ser demostrable al terminar).  El
[ledger](sel4-convergence-ledger.md) mapea cada mecanismo transitorio a su
etapa de retiro.  Ninguna etapa puede declararse cerrada mientras su camino
productivo dependa del mecanismo que retira (charter §3.10).

## Etapa 0 — Consolidación de TCB  ✅ CERRADA (Fase S2 inc.2)

- Incremento abierto cerrado y commiteado; working tree limpio.
- KTCB canónico: `struct task` ES el objeto (KObject en offset 0); wrapper
  eliminado; cinco lifetimes separados (cap / objeto / ejecución / registro /
  storage) sin refcount ambiguo.
- Lifecycle estable: TERMINATED ≠ destruido; el destructor es el único
  liberador de storage; run queues por puntero; registry con generación.
- Storage retipable: `RETYPE2(KOBJ_TCB)` crea TCBs canónicos (inactivos,
  `configured=0`) con storage dentro del Untyped y cap directa en CSpace;
  la familia migrada queda {EP, Notif, Reply, CNode, SC, TCB}.
- Ningún handle nuevo: la creación por RETYPE2 no publica handles; la guarda
  `make check-purity` congela los consumidores existentes.
- Deuda registrada: la RUTA DE EJECUCIÓN de threads (SYS_THREAD_CREATE)
  sigue naciendo del pool estático + handle; su sustituto (TCB_CONFIGURE
  sobre un TCB retipado) requiere caps de CSpace/VSpace como argumentos y
  se define en Etapa 5/6 (post-CDT).  Idle task = excepción bootstrap
  aislada (registry slot 0, jamás retipada ni reusada).

## Etapa 1 — CDT/MDB  ← SIGUIENTE

Precondición: Etapa 0 (cerrada).
Contrato de entrada detallado: ver §"Contrato CDT" al final.

- Metadata de derivación asociada a slots de CNode (no a handles).
- Relaciones parent/child globales (cross-CNode, cross-process).
- copy/mint/move/delete sobre slots manteniendo el árbol.
- Revocación recursiva cross-process con cleanup determinista.
- Rollback exacto ante fallos parciales de cualquier operación de árbol.
- `legacy_handle_derivation_migrated` debe converger a 0.

Cierre: revoke de un ancestro elimina toda la descendencia en cualquier
CSpace del sistema, bajo suite adversarial (cadenas, ciclos de mint,
muerte concurrente del holder, revoke durante IPC staged).

## Etapa 2 — Cap transfer CSpace-only

Precondición: Etapa 1 (el staging necesita el CDT para registrar la
derivación de la cap entregada).

- Origen por CPtr (retira el peek handle-only de
  `syscall_ipc_stage_cap_peek_badged`).
- Destino: receive slot (ya existente) como único camino.
- Staged transfer sobre slots con la misma atomicidad peek/commit.
- Eliminar la degradación TOCTOU slot→handle (el contador
  `iris_ipc_stat_toctou_fallbacks` debe quedar estructuralmente en 0).

## Etapa 3 — Derive y revoke CSpace-only

Precondición: Etapas 1–2.

- Retirar `SYS_CAP_DERIVE`/`SYS_CAP_REVOKE` handle-only (o redefinirlos
  sobre slots) y el árbol `derivation_parent[]` de la handle table.
- Migrar los consumidores productivos; guardas de no-regresión (los números
  de syscall retirados quedan reservados → NOT_SUPPORTED).

## Etapa 4 — Retiro del namespace dual

Precondición: Etapas 2–3 (ya no queda autoridad que solo viva en handles).

- Eliminar la discriminación por rango (<1024 / ≥1024).
- Eliminar la resolución handle de todos los dual-resolvers.
- Eliminar los productores de handles del bootstrap (inserción dual de
  kernel_main, materialización `SYS_CSPACE_RESOLVE`).
- Eliminar la handle table cuando tenga cero consumidores; la allowlist de
  `check_purity` debe quedar vacía.

## Etapa 5 — Bootstrap seL4-like

Precondición: Etapa 4 (las caps iniciales ya solo pueden ser CSpace).

- Sustituir `KBootstrapCap` monolítico por BootInfo estructurado.
- Root task con: root CNode, TCB inicial, VSpace inicial, IRQ control cap,
  ASID/PCID control, lista de Untypeds, caps finas por dispositivo.
- Aquí se define TCB_CONFIGURE/TCB_WRITE_REGS (ejecución de TCBs retipados)
  porque sus argumentos (CSpace root, VSpace, fault EP) ya existen como caps.

## Etapa 6 — Memoria y objetos restantes

Precondición: Etapa 1 (ownership/derivación); puede solaparse con 5.

- Page-table objects retipados desde Untyped (retira la reserva PMM de
  paging_map).
- VSpace canónico desde Untyped; headers de Frame dentro de la región.
- Retirar las rutas kslab de objetos restantes (lista del ledger).
- Convertir o retirar KVMO; separar memoria de archivo y anónima en
  servicios de usuario (pager/VFS ya dan la base).

## Etapa 7 — Retiro de KProcess

Precondición: Etapas 5–6 (un proceso = composición TCB+CSpace+VSpace).

- Process server en user space; creación y política de procesos fuera del
  kernel; PID deja de conferir autoridad; quotas por dominio → política del
  process server.

## Etapa 8 — Scheduling MCS completo

Precondición: Etapas 0–2 (SC/TCB canónicos + IPC CSpace-only).

- SC delegation y donación durante IPC donde corresponda; timeouts;
  replenishment; semántica de prioridad revisada; pruebas de presupuestos.
- Revisar aquí la divergencia "sin ReplyRecv combinado" (charter §6).

## Etapa 9 — SMP

Precondición dura: namespace de autoridad único (4), CDT (1), lifecycle (0),
IPC CSpace-only (2), y un modelo de locking documentado.

- Re-derivar TODA propiedad de atomicidad que hoy dependa del kernel
  no-preemptivo monoprocesador (catálogo: staging IPC, RETYPE2, bind de
  reply, teardown).  Ownership de run queues por CPU.  Ninguna corrección
  puede seguir argumentándose "porque el kernel es no-preemptivo".

## Etapa 10 — Plataforma de propósito general

Precondición: microkernel consolidado (0–9 según aplique).

- Drivers user-space; PCI/ACPI/IOMMU; almacenamiento; FS persistente; red;
  personalidad POSIX opcional por servidores/librerías; seguridad avanzada;
  rendimiento; hardware real.  Nada de esto entra antes: charter §5.

---

## Contrato de entrada del incremento CDT/MDB (Etapa 1)

Lo que el incremento CDT debe implementar, definido ahora para que Etapa 0
no le herede ambigüedad:

**Estructuras.**  Metadata de derivación POR SLOT de CNode (no por handle):
enlace al slot padre + lista/anillo de hijos (estilo MDB de seL4: lista
doblemente enlazada ordenada por profundidad, o árbol explícito).  El storage
de la metadata vive en el propio slot (CNode ya nace de Untyped — sin kslab).

**Relaciones.**  Original (retype/mint desde Untyped) vs derivado
(copy/mint/transfer).  La derivación cruza CNodes y procesos.  El Untyped
progenitor es el ancestro raíz de todo objeto retipado (child_count se
integra o reconcilia con el árbol).

**Operaciones.**  `copy` (mismos rights), `mint` (rights↓ + badge una vez),
`move` (traslada el slot conservando su posición en el árbol), `delete`
(slot individual; si es la última cap del objeto, destruye), `revoke`
(elimina recursivamente TODOS los descendientes del slot, en cualquier
CSpace; el slot revocado sobrevive).

**Invariantes.**  (1) rights de un hijo ⊆ rights del padre; (2) badge
inmutable tras el primer badgeo; (3) delete de un padre NO huérfana el
árbol (reparent o barrido, elegir y documentar — seL4 usa el MDB para
esto); (4) revoke es atómico respecto a IPC staged: una cap en staging
peek revocada no se entrega (el commit falla limpio); (5) rollback exacto
si una operación de árbol falla a medias.

**Integración CNode.**  `kcnode_mint*/fetch/delete/swap` mantienen el árbol;
el teardown de un CNode (close) hace delete de cada slot vía el árbol, no
solo release de refcounts.

**Integración IPC.**  La entrega a receive-slot registra la cap entregada
como hija de la cap fuente (prepara Etapa 2).

**Integración Untyped.**  retype registra los slots destino como originales
del Untyped; RESET exige árbol vacío (sustituye/refina child_count);
revoke del Untyped = revocar todos sus originales.

**Integración teardown.**  La muerte de un proceso hace delete de todos sus
slots a través del árbol; las caps que OTROS procesos derivaron de las
suyas quedan donde el modelo elegido lo defina (documentar: seL4 las
conserva — la derivación no impone lifetime del holder).

**Pruebas necesarias.**  Cadena A→B→C cross-process + revoke en A; revoke
durante transferencia staged; delete del intermedio; mint con rights↓ y
re-badge denegado; muerte del holder intermedio; estrés de
retype/revoke/reset con verificación de gauges y de no-UAF; guarda de que
`legacy_handle_derivation_migrated` → 0.
