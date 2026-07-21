# IRIS — seL4 Convergence Ledger (normativo)

Registro de deuda híbrida: todo mecanismo no-seL4 que sigue vivo, quién lo
usa, su reemplazo y su fase de retiro.

**Marco normativo**: este ledger implementa el
[charter de pureza seL4](iris-sel4-purity-charter.md) (constitucional) y el
[roadmap de convergencia](sel4-convergence-roadmap.md) (orden por
dependencias).  Las "removal phases" de la tabla se leen contra las Etapas
del roadmap.  La guarda ejecutable `make check-purity` congela los
consumidores legacy de handle table / kslab: la allowlist solo decrece.

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
| `struct task tasks[TASK_MAX]` (pool estático) | backing de kstack + arch-context + scheduler linkage | scheduler, thread create | TCB payload desde Untyped; array → registro de punteros/generation | S2 (run-queue index→pointer + productive-path Untyped source) | sí — no nuevos consumidores fuera del scheduler | ACTIVE_LEGACY (pool estático acotado, NO kslab; storage de TCB runtime — REMOVE pendiente) |
| `task_rsp[TASK_MAX]` (array RSP index-keyed) | RSP de kernel por slot, paralelo al array | scheduler context switch | `struct task.saved_krsp` | S2 inc.2 | — | REMOVED (Fase S2 inc.2 — primera indirección del scheduler) |
| run-queue `next[TASK_MAX]`/`queued[TASK_MAX]` + `(t - tasks)`/`&tasks[idx]` | identidad de run-queue por índice de array | rq_enqueue/remove/dequeue | listas intrusivas por puntero (`t->rq_next`/`rq_queued`) | S2 inc.2B | — | REMOVED (Fase S2 inc.2B Bloque A — run queue 100% por puntero) |
| `tasks[j]` timeout scans (tick/idle) + slot allocation | iteración sobre el array de backing | scheduler_tick / sched_handle_idle / task alloc | iteración sobre `ktcb_registry[]` (punteros+generation) | S2 inc.2 Etapa C | — | REMOVED como identidad (Fase S2 Etapa C — todo va por `ktcb_registry[i].tcb`) |
| `KTcbRegistrySlot ktcb_registry[TASK_MAX]` | registro de referencias (tcb*/generation/occupied/bootstrap), NO payload | scheduler/alloc/lookup | mismo registro; capacidad transitoria | — | límite TASK_MAX transitorio | TRANSITIONAL_IMPLEMENTATION_CAPACITY (Etapa C) |
| `struct task tasks[TASK_MAX]` (payload estático) | backing real del TCB (registros/kstack ptr/scheduler state) apuntado por `registry[i].tcb` | registry (scaffolding) | KTCB canónico en Untyped (Etapa D) | S2 inc.2 Etapa D | sí — scaffolding, sin consumidores nuevos | ACTIVE_LEGACY (scaffolding; REMOVE en Etapa D, salvo idle bootstrap) |
| `SYS_THREAD_SET_SC` (85) | self-bind SC | código sched existente | `SYS_SC_BIND(sc,tcb)` por CPtr | — | sí — congelado | FROZEN (Fase S2 inc.1) |
| `struct KTcb` wrapper (kslab) | objeto TCB cap-visible en heap, separado del task | — | `struct task` ES el KTCB (KObject en offset 0) | S2 inc.2 | — | REMOVED (Fase S2 inc.2 — una estructura, una identidad) |
| thread-create ejecutable vía pool + handle (`SYS_THREAD_CREATE`/`task_create_user_impl`) | la ruta de EJECUCIÓN de threads nace del pool estático y publica un handle en la tabla del proceso | spawn/loader, iris_test threads | TCB retipado (`RETYPE2(KOBJ_TCB)`, ya existente) + `TCB_CONFIGURE` con caps CSpace/VSpace (Etapa 5/6, post-CDT) | Etapa 5/6 | sí — ningún camino nuevo de creación de threads | ACTIVE_LEGACY (la única ruta ejecutable; el TCB retipado es cap-completo pero inactivo hasta TCB_CONFIGURE) |
| idle task (backing estático, registry slot 0) | TCB bootstrap fuera de Untyped, sin objeto cap-visible | scheduler | root-task TCB del BootInfo (Etapa 5) | Etapa 5 | sí — excepción bootstrap aislada, jamás retipada ni reusada | BOOTSTRAP_EXCEPTION |
| derivación handle-tree para tipos migrados (EP/Notif/Reply/CNode/TCB/SC) | árbol de derivación oculto en handle table | SYS_CAP_DERIVE | CDT/MDB nativo en slots de CNode | S2 (Bloque D) | sí | ACTIVE_LEGACY (contador `legacy_handle_derivation_migrated` observable, debe → 0) |
| root CNode at `kprocess_alloc` (kslab) | CNode runtime fuera de Untyped | todo spawn | spawner aporta CNode retipado (process-server) | process-server | sí | ACTIVE_LEGACY |
| root CNode alcanzable solo vía `cspace_root_h` (handle) | la RAÍZ del CSpace se localiza a través de la handle table | todo resolver + Fase S3: `cspace_resolve_slot`, `cspace_own_root`, `SYS_CSPACE_MINT_INTO` (allowlist ampliada +3 citando charter §3 en el mismo commit) | BootInfo/root-task entrega el root CNode como cap estructural | Etapa 5 | sí — solo lecturas del root para resolución | ACTIVE_LEGACY |
| implicit page-table allocation (PMM reserve en map) | memoria kernel oculta por mapping | paging | PageTable objects desde Untyped | frame/page-table | sí | ACTIVE_LEGACY |
| KFrame header sidecar (kslab) | metadata fuera de la región | frame retype | header dentro de Untyped | frame/page-table | sí | ACTIVE_LEGACY |
| process-level fault record (único por proceso) | pertenece al TCB | fault delivery (Fase 20/25) | fault por-TCB / fault EP | process-server | sí | ACTIVE_LEGACY |
| `SYS_PROCESS_VSPACE` (107) | autoridad de proceso → VSpace por handle | supervisors/pager tests | CSpace mint del VSpace cap | process-server | sí | ACTIVE_LEGACY |
| `KBootstrapCap` | autoridad de arranque monolítica | userboot/init/svcmgr/tests | BootInfo estructurado + caps finas | root-task/BootInfo | sí | ACTIVE_LEGACY |
| `KInitrdEntry` + `SYS_INITRD_*` | filesystem-aware kernel state | loader | VFS/loader userland | process-server | sí | ACTIVE_LEGACY |
| kernel stacks / PML4 desde reserva PMM | asignación fuera de Untyped | task/process create | TCB/VSpace desde Untyped | process/frame phases | sí | ACTIVE_LEGACY |
| `KChannel` | — | — | endpoints | Fase 13 | — | REMOVED |
| whitelist ioport hardcodeada (`kioport_whitelist`, syscall_priv.h) | política de dispositivo en kernel | kbd/console/fb/userboot vía svcmgr | caps de ioport finas emitidas por la root task (BootInfo) | Etapa 5 | sí — ninguna entrada nueva sin cita al charter §2.6/P3 | ACTIVE_LEGACY (bootstrap temporal) |
| fallback TOCTOU receive-slot→handle (`syscall_ipc_deliver_cap_routed`) | degradación de entrega CSpace a handle | entrega IPC con slot declarado que pierde la carrera | instalación CSpace-only con CDT (la carrera se resuelve en el árbol) | Etapa 2 | sí — contado (`iris_ipc_stat_toctou_fallbacks`), nunca un patrón | ACTIVE_LEGACY (única degradación permitida, condenada) |

## Checkpoint C.1 — Versioned user-buffer ABI (Fase S2)

`SYS_UNTYPED_QUERY` (arg0 = kind|version<<16|size<<32) y `SYS_RESOURCE_INFO`
(arg2 = user_size) conocen el tamaño declarado por el caller y escriben como
máximo `min(user_size, kernel_size)` (prefix-compatible): un caller
antiguo/menor no puede desbordarse.  Header mínimo (8 B) y versión no soportada
→ `IRIS_ERR_INVALID_ARG` sin escribir.  Helper `copy_versioned_to_user`.
Auditoría de queries versionadas:

| Query | Version | Size field | Copy bound | Prefix-compat | Action |
|---|---|---|---|---|---|
| SYS_UNTYPED_QUERY (1..4) | arg0 bits16-31 | arg0 high32 | min(user,kernel) | sí | HARDENED |
| SYS_RESOURCE_INFO | struct.version | arg2 | min(user,kernel) | sí | HARDENED |
| SYS_TCB_GET_INFO (iris_tcb_info) | — | fija | sizeof fija | n/a | FIXED-SIZE (estable, no crece) |
| SYS_PROCESS_FAULT_INFO | — | FAULT_MSG_LEN fija | fija | n/a | FIXED-SIZE |
| SYS_SCHED_INFO ext tiers | tier-gated | `want` acotado | acotado | parcial | REVISADO (acota por tier) |

Test: T283 (QABI1–10 + guard canaries).  Nuevos campos futuros en un struct de
query ya no pueden desbordar un caller que declara su tamaño.

## Guard de no-regresión

- T251 fija el manifiesto cerrado de tipos creables por RETYPE2.
- T260 fija el retiro de los create syscalls y su no-efecto.
- T125/T126 fijan el rechazo de la familia migrada en el retype legacy.
- Los asserts `IRIS_KOBJ_* == KOBJ_*` fijan la ABI de tipos.
- Revisión: cualquier PR que añada `kslab_alloc` para un tipo canónico,
  un `SYS_*_CREATE` nuevo, o un resolver handle-first nuevo para objetos
  canónicos debe rechazarse citando este ledger.
