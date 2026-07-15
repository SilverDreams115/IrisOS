# IRIS — Canonical Kernel Object Model (Fase S1, normativo)

## Objetivo definitivo

> IRIS debe converger hacia un microkernel arquitectónicamente seL4-puro:
> todos los objetos kernel dinámicos nacen de memoria Untyped explícita,
> toda autoridad persistente vive en CSpace, y procesos, objetos de memoria,
> loaders, pagers y políticas de recursos se construyen en userland.

Este documento es **normativo**: define el conjunto final de objetos kernel,
sus tamaños, su ciclo de vida y las excepciones bootstrap.  Ningún mecanismo
nuevo puede introducir un tipo de objeto, un allocator o un camino de
creación que no esté registrado aquí y en
[`sel4-convergence-ledger.md`](sel4-convergence-ledger.md).

**Nota de assurance**: IRIS NO afirma equivalencia de assurance con seL4.
No existe verificación formal mecanizada; la convergencia es arquitectónica.

## Conjunto canónico final

```
Untyped            (KUntyped     — implementado, canónico)
CNode              (KCNode       — implementado, canónico)
TCB                (KTcb+task    — presente; storage aún kslab/estático → migración pendiente)
SchedulingContext  (KSchedContext— presente; retype legacy → migración de ABI pendiente)
Endpoint           (KEndpoint    — implementado, canónico, Untyped-only desde S1)
Notification       (KNotification— implementado, canónico, Untyped-only desde S1)
Reply              (KReply       — implementado, canónico, Untyped-only + explícito desde S1)
Frame              (KFrame       — región física canónica; header sidecar aún kslab)
VSpaceRoot         (KVSpace      — presente; storage aún kslab → migración pendiente)
PageTable          (implícito en paging — objeto explícito pendiente)
IRQControl         (implícito en spawn-cap/IRQ setup — objeto explícito pendiente)
IRQHandler         (KIrqCap      — presente; storage aún kslab)
```

No se identificó ningún objeto adicional estrictamente mecánico que deba
vivir en kernel: puertos de E/S (KIoPort) son autoridad de dispositivo
(equivalente al modelo IO-port-control de seL4/x86) y permanecen; todo lo
demás es política y se compone en userland.

## Clasificación de los objetos actuales

| Current object | Final status | Canonical replacement | Migration phase | Reason |
|---|---|---|---|---|
| KUntyped | CANONICAL | — | S1 (hecho) | sustrato de asignación |
| KCNode | CANONICAL | — | S1 (creación runtime hecha; root-CNode-at-spawn pendiente) | CSpace |
| KEndpoint | CANONICAL | — | S1 (hecho) | IPC síncrono |
| KNotification | CANONICAL | — | S1 (hecho) | señales asíncronas |
| KReply | CANONICAL | — | S1 (hecho, estilo MCS explícito) | reply authority |
| KSchedContext | CANONICAL | — | S2+ (storage ya Untyped vía retype; SYS_SC_CREATE legacy por retirar) | tiempo |
| KTcb / struct task | CANONICAL (TCB) | TCB desde Untyped | S2+ | hilo |
| KFrame | CANONICAL (Frame) | header dentro de Untyped | frame/page-table phase | memoria física |
| KVSpace | CANONICAL (VSpaceRoot) | storage desde Untyped | frame/page-table phase | espacio de direcciones |
| KIrqCap | CANONICAL (IRQHandler) | storage desde Untyped | device phase | enrutado IRQ |
| KIoPort | CANONICAL (arch) | storage desde Untyped | device phase | autoridad de puertos |
| KProcess | LEGACY_TO_REMOVE | process server userland (TCB+CNode+VSpace) | process-server phase | proceso = política |
| KVMO | LEGACY_TO_REMOVE | memory server userland (Frames+pager) | memory-server phase | objeto de memoria = política |
| handle table / handles | LEGACY_TO_REMOVE | CSpace-only invocation | CSpace-only phase | segundo namespace |
| per-process quota domains (VMO/page) | LEGACY_TO_REMOVE | Untyped como presupuesto | con KProcess/KVMO | quota ≠ memoria explícita |
| notification quota | REMOVED (S1) | Untyped | S1 | retirada |
| KBootstrapCap | BOOTSTRAP_EXCEPTION | BootInfo estructurado | root-task phase | autoridad de arranque |
| KInitrdEntry | USERLAND_POLICY | VFS/loader userland | con KProcess | filesystem-aware state |
| process metadata / parent-child / supervision | USERLAND_POLICY | svcmgr/init | ya en userland | política |
| file-backed regions / page cache / private-shared | USERLAND_POLICY | pager+VFS | ya en userland (Fase 28) | política |
| loader metadata | USERLAND_POLICY | svc_loader | ya en userland | política |
| kslab (para objetos dinámicos) | LEGACY_TO_REMOVE | retype de Untyped | por familia (ledger) | allocator oculto |

`NOT_AN_OBJECT`: colas de scheduler, rutas IRQ, buffers klog — estado interno
del kernel, no autoridad. `UNJUSTIFIED`: ninguno detectado en la auditoría S1.

## Regla central (S1)

Para cualquier objeto migrado:

```
la memoria retipada ES el almacenamiento del objeto kernel
```

- El header (`struct KObject`: tipo, refcounts, lock, ops) es el primer campo
  del payload y vive DENTRO de la región retipada (asserts en
  `syscall_untyped.c`).
- No hay metadata sidecar dinámica en kslab para objetos migrados.
- El bloque retipado es `KUNTYPED_ALIGN` (64 B, back-pointer al padre) +
  `align64(sizeof(objeto))`; al destruir, el bloque se cero-rellena y
  decrementa `child_count` del Untyped fuente.

## Tamaños y estados (S1)

Contrato de tamaño: granularidad `KUNTYPED_ALIGN = 64 B` (contrato explícito
equivalente al size_bits de seL4; los tamaños exactos son
`sizeof(struct K*)`, fijados por asserts de compilación y visibles en la
tabla de abajo como bloque consumido = 64 + align64(sizeof)).

| Object | Payload | Alignment | Retype source | Initial state | Destruction precondition |
|---|---|---|---|---|---|
| Endpoint | sizeof(KEndpoint) | ≤64 | Untyped normal | IDLE, colas vacías | refcount 0 (todas las caps + refs kernel liberadas) |
| Notification | sizeof(KNotification) | ≤64 | Untyped normal | bits=0, sin waiters | refcount 0 |
| Reply | sizeof(KReply) | ≤64 | Untyped normal | free (caller=NULL, staged=0) | refcount 0 |
| CNode(n) | KCNODE_ALLOC_SIZE(n), n pot. de 2 ≤4096 | ≤64 | Untyped normal | slots vacíos | refcount 0 (close libera slots) |
| SchedContext | sizeof(KSchedContext) | ≤64 | Untyped normal | budget por defecto | refcount 0 |
| Sub-Untyped | arg bytes (≥4096, múltiplo de página) | página | Untyped normal/device | used=0, gen=0 | refcount 0 y sin hijos |
| Frame | arg bytes (≥4096, múltiplo de página) | página | Untyped normal/device | sin mapear | refcount 0, mapped_count 0 |

`maximum count per retype`: 32 objetos y 128 KiB por batch
(`KUNTYPED_RETYPE_MAX_COUNT/MAX_BYTES`); UNTYPED/FRAME siempre count=1 en S1.
`zeroing`: todo bloque de Untyped normal se cero-rellena al carve y al
destruir. `device`: un Untyped device solo produce UNTYPED/FRAME (U11/U12).

## Autoridad

- Creación: poseer un cap Untyped con RIGHT_WRITE + slots CSpace destino
  vacíos.  **Nunca** una quota numérica, un handle ni `RIGHT_MANAGE` sobre
  un proceso (S19/S20).
- Toda capability creada por retype aparece directamente en CSpace (S21);
  `SYS_CSPACE_RESOLVE` es el único puente sancionado CSpace→handle
  (materialización efímera, contrato A1).
- Rights por defecto al nacer: EP `R|W|DUP|XFER`; Notification
  `R|W|WAIT|DUP|XFER`; Reply `R|W|XFER|DUP` (DUP solo para que el supervisor
  lo minte al hijo y luego SUELTE su copia); CNode/SC/Untyped/Frame
  `R|W|DUP|XFER`.

## Ciclo de vida (delete / revoke / reuse)

Ver [`kernel-object-lifetime.md`](kernel-object-lifetime.md).  Resumen:
- delete de una cap = liberar ese slot/handle; el objeto vive mientras
  queden caps o refs kernel (S10).
- la última cap dispara `close` (despierta waiters con CLOSED — S25/S26/S27)
  y, sin refs kernel, `destroy` (el bloque vuelve, cero-relleno, a la región).
- `SYS_UNTYPED_RESET` reclama la región solo con `child_count == 0` (S13) y
  bumpa `generation` (testigo de reuse, S12/S28).
- revoke transitivo: el árbol de derivación vive HOY en la handle table
  (`SYS_CAP_REVOKE`); un CDT completo sobre CSpace es trabajo S2+ (ledger).

## Excepciones bootstrap (enumeradas, estáticas, no-allocator)

1. Imagen del kernel, stacks iniciales, metadata de boot (estático).
2. Root CNode por proceso: fabricado por `kprocess_alloc` desde kslab.
   Acotado (1 por proceso), pero crece con procesos → clasificado
   ACTIVE_LEGACY ligado a KProcess; primer objetivo de la fase process-server.
3. Reserva PMM del kernel (`IRIS_PMM_KERNEL_RUNTIME_RESERVE`): page tables,
   kernel stacks, PML4, metadata KVMO — allocators internos legacy,
   registrados en el ledger; no disponibles para crear objetos canónicos.
4. Fixtures de selftests (phase3, host tests): bloques estáticos con header
   de untyped-child y parent NULL; solo builds de test.

Ninguna excepción puede usarse para crear objetos después del bootstrap ni
actuar como allocator alternativo del modelo nuevo.
