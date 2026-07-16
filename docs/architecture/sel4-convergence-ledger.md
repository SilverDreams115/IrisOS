# IRIS — seL4 Convergence Ledger (normativo)

Registro de deuda híbrida: todo mecanismo no-seL4 que sigue vivo, quién lo
usa, su reemplazo y su fase de retiro.

**Regla**: `ningún mecanismo marcado FROZEN puede recibir nuevos
consumidores`.  Añadir un consumidor a una entrada FROZEN/ACTIVE_LEGACY es
un defecto de revisión.  Los estados son:
`ACTIVE_LEGACY` (en uso, sin migración en curso) · `MIGRATING` (migración
parcial) · `FROZEN` (prohibidos nuevos usos) · `RETIRED` (número/símbolo
reservado, sin funcionalidad) · `REMOVED` (borrado).

| Legacy mechanism | Why non-seL4 | Current users | Replacement | Removal phase | New uses forbidden | State |
|---|---|---|---|---|---|---|
| `KProcess` | proceso como objeto kernel = política en kernel | spawn/loader, supervision, fault info, accounting | process server userland (TCB+CNode+VSpace+Untyped) | process-server | sí | FROZEN |
| `KVMO` (+`SYS_VMO_CREATE`/`SYS_VMO_CREATE_FOR`/`SYS_VMO_MAP*`) | objeto de memoria con política (owner, quota, file-backing) | loader, pager, tests | memory server (Frames + pager) | memory-server | sí | FROZEN |
| `kslab` para objetos dinámicos | heap global oculto | KProcess, KVMO, KFrame header, KVSpace, KTcb, KIrqCap, KIoPort, KBootstrapCap, KInitrdEntry, KUntyped header, root CNode, handle table | retype de Untyped | por familia (ver filas) | sí — ningún objeto canónico nuevo puede nacer de kslab | MIGRATING |
| kslab para KEndpoint/KNotification/KReply/CNode runtime | ídem | — | RETYPE2 | S1 | — | REMOVED |
| notification owner quota (`KPROCESS_NOTIFICATION_QUOTA`) | quota numérica como fuente de creación | — | Untyped es el presupuesto | S1 | — | REMOVED |
| per-process VMO/page quotas (Fase 29) | resource-domain paralelo a la memoria explícita | KVMO/paging | Untyped | con KProcess/KVMO | sí | LEGACY_FOR_KPROCESS_KVMO (ACTIVE_LEGACY) |
| payer selection (`SYS_VMO_CREATE_FOR`) | accounting por payer | svc_loader | delegación de Untyped | con KVMO | sí | ACTIVE_LEGACY |
| `SYS_RESOURCE_INFO` campos notifs_* | espejo de quota retirada | tests (leen 0) | — (aditivo, congelado en 0) | con KProcess | sí | TRANSITIONAL_DIAGNOSTICS |
| handle table / dual resolution | segundo namespace de autoridad | todos los syscalls dual-resolver; materialización A1 | CSpace-only invocation | CSpace-only ABI | nuevos caminos handle-first para objetos canónicos: PROHIBIDOS (guard: T251/T260; revisión) | FROZEN para nuevos productores (lista A1 cerrada) |
| `SYS_ENDPOINT_CREATE` (73) | fabricación global sin Untyped | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_NOTIFY_CREATE` (19) | ídem + quota + handle | — | RETYPE2 | S1 | — | RETIRED |
| `SYS_CNODE_CREATE` (80) | ídem | — | RETYPE2 | S1 | — | RETIRED |
| implicit reply allocation (kreply en EP_CALL) | el kernel fabricaba autoridad por llamada | — | reply objects explícitos (recv arg2) | S1 | — | REMOVED |
| `SYS_UNTYPED_RETYPE` (87) handle-publishing | publica autoridad como handle | tests/authority suite (UNTYPED/FRAME/SC) | RETYPE2 | CSpace-only ABI | para tipos migrados: ya rechaza | MIGRATING |
| `SYS_SC_CREATE` (83) | create global de SC | ninguno | RETYPE2 + SC_CONFIGURE + SC_BIND | S2 | — | RETIRED (Fase S2) |
| `kschedctx_alloc` (kslab SC) | payload SC en heap global | ninguno | RETYPE2 (`kschedctx_alloc_at`) | S2 | sí | REMOVED (Fase S2) |
| `struct task tasks[TASK_MAX]` (pool estático) | backing de kstack + arch-context + scheduler linkage | scheduler, thread create | TCB desde Untyped (userland aporta) | S2 (resto) / process-server | sí — no nuevos consumidores fuera del scheduler | ACTIVE_LEGACY (pool estático acotado, NO kslab, NO allocator dinámico) |
| `KTcb` payload (kslab) | objeto TCB cap-visible en heap | thread create | retype KOBJ_TCB desde Untyped | S2 (resto) | sí | ACTIVE_LEGACY (kslab; migración pendiente increment 2) |
| derivación handle-tree para tipos migrados (EP/Notif/Reply/CNode/TCB/SC) | árbol de derivación oculto en handle table | SYS_CAP_DERIVE | CDT/MDB nativo en slots de CNode | S2 (Bloque D) | sí | ACTIVE_LEGACY (contador `legacy_handle_derivation_migrated` observable, debe → 0) |
| root CNode at `kprocess_alloc` (kslab) | CNode runtime fuera de Untyped | todo spawn | spawner aporta CNode retipado (process-server) | process-server | sí | ACTIVE_LEGACY |
| implicit page-table allocation (PMM reserve en map) | memoria kernel oculta por mapping | paging | PageTable objects desde Untyped | frame/page-table | sí | ACTIVE_LEGACY |
| KFrame header sidecar (kslab) | metadata fuera de la región | frame retype | header dentro de Untyped | frame/page-table | sí | ACTIVE_LEGACY |
| process-level fault record (único por proceso) | pertenece al TCB | fault delivery (Fase 20/25) | fault por-TCB / fault EP | process-server | sí | ACTIVE_LEGACY |
| `SYS_PROCESS_VSPACE` (107) | autoridad de proceso → VSpace por handle | supervisors/pager tests | CSpace mint del VSpace cap | process-server | sí | ACTIVE_LEGACY |
| `KBootstrapCap` | autoridad de arranque monolítica | userboot/init/svcmgr/tests | BootInfo estructurado + caps finas | root-task/BootInfo | sí | ACTIVE_LEGACY |
| `KInitrdEntry` + `SYS_INITRD_*` | filesystem-aware kernel state | loader | VFS/loader userland | process-server | sí | ACTIVE_LEGACY |
| kernel stacks / PML4 desde reserva PMM | asignación fuera de Untyped | task/process create | TCB/VSpace desde Untyped | process/frame phases | sí | ACTIVE_LEGACY |
| `KChannel` | — | — | endpoints | Fase 13 | — | REMOVED |

## Guard de no-regresión

- T251 fija el manifiesto cerrado de tipos creables por RETYPE2.
- T260 fija el retiro de los create syscalls y su no-efecto.
- T125/T126 fijan el rechazo de la familia migrada en el retype legacy.
- Los asserts `IRIS_KOBJ_* == KOBJ_*` fijan la ABI de tipos.
- Revisión: cualquier PR que añada `kslab_alloc` para un tipo canónico,
  un `SYS_*_CREATE` nuevo, o un resolver handle-first nuevo para objetos
  canónicos debe rechazarse citando este ledger.
