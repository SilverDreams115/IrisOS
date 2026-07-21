# IRIS — Charter de pureza seL4 (constitucional, normativo)

**Estado**: VIGENTE desde Fase S2 inc.2.
**Rango**: este documento prevalece sobre cualquier otro documento del repo
(README, docs de fase, comentarios) en caso de conflicto.  Solo puede
modificarse en un commit que lo cite explícitamente y actualice el
[ledger](sel4-convergence-ledger.md) en el mismo cambio.
**Documentos hermanos**: el [roadmap de convergencia](sel4-convergence-roadmap.md)
ordena las etapas; el [ledger](sel4-convergence-ledger.md) registra cada
mecanismo transitorio y su condición de retiro; la guarda ejecutable
`make check-purity` (`scripts/check_purity.sh`) congela los consumidores
legacy existentes.

## 1. Identidad oficial

> IRIS es un **microkernel capability-based puro, de implementación propia,
> en convergencia semántica hacia seL4/MCS**, con todos los servicios y
> políticas no esenciales fuera del kernel.

Precisiones vinculantes:

- "seL4 puro" se refiere al **modelo arquitectónico y de autoridad**
  (objetos tipados desde Untyped, CSpace/CPtr, CDT, revoke recursivo,
  ausencia de ambient authority, mecanismo sin política) — no a la ABI ni al
  código de seL4, que IRIS no reutiliza ni promete reproducir.
- IRIS **no** afirma estar formalmente verificado.  Sus invariantes se
  demuestran por construcción + pruebas adversariales, y así debe declararse.
- Toda extensión propia de IRIS debe **preservar la pureza capability-based**;
  una feature que la viole no es una feature, es un defecto de diseño.
- El objetivo final no es un clon de seL4: es una plataforma propia de largo
  plazo construida sobre principios equivalentes, capaz de crecer (drivers,
  almacenamiento, red, personalidad POSIX opcional) **exclusivamente en
  user space**, sin re-contaminar el kernel.
- El modelo híbrido actual (handle table + resolución dual) es
  **exclusivamente transitorio** y está condenado a retiro (roadmap
  Etapas 1–4).  Ninguna decisión futura puede consolidarlo.

## 2. Invariantes no negociables

Cada invariante es una regla de revisión: un cambio que la viole se rechaza
citando este charter.  Los estados "hoy" son honestos: `CUMPLIDO`,
`PARCIAL` (deuda registrada en el ledger) o `PENDIENTE` (etapa del roadmap).

### 2.1 Autoridad

| # | Invariante | Hoy |
|---|---|---|
| A1 | Toda operación sensible exige una capability válida | CUMPLIDO |
| A2 | CSpace es el ÚNICO namespace persistente de autoridad | PARCIAL — handle table viva (Etapas 2–4) |
| A3 | CPtr es el único identificador de capability expuesto productivamente | PARCIAL — ídem |
| A4 | En el estado final no existen handles productivos | PENDIENTE (Etapa 4) |
| A5 | No existe ambient authority | PARCIAL — whitelist ioport, quotas kernel (ledger) |
| A6 | `ACCESS_DENIED` jamás provoca fallback a otro namespace | CUMPLIDO (split <1024/≥1024 sin fallback) |
| A7 | Los rights solo se mantienen o reducen; mint jamás amplifica | CUMPLIDO (`rights_reduce`, colapso a NONE rechazado) |
| A8 | Los badges son identidad sellada por el kernel; un cap badgeado nunca se re-badgea | CUMPLIDO |
| A9 | Toda capability derivada es rastreable hasta su ancestro | CUMPLIDO para la derivación CSpace (MDB/CDT nativo, Fase S3); el árbol handle-tree legacy (`SYS_CAP_DERIVE`) sigue en paralelo, congelado (Etapa 3) |
| A10 | Revoke elimina recursivamente toda autoridad descendiente, incluso cross-process | CUMPLIDO para caps CSpace (`SYS_CSPACE_REVOKE`, Fase S3 — cruza CNodes y procesos, probado T288-T290 + fuzzing); `SYS_CAP_REVOKE` handle-only sigue siendo intra-tabla (Etapa 3) |

### 2.2 Objetos

| # | Invariante | Hoy |
|---|---|---|
| O1 | Todo objeto canónico nace de Untyped vía retype | PARCIAL — EP/Notif/Reply/CNode/SC/TCB por RETYPE2; ejecución de TCB, Frame header, VSpace, page tables pendientes (Etapas 0/6) |
| O2 | El storage del objeto pertenece al Untyped que lo originó | CUMPLIDO para la familia RETYPE2 |
| O3 | La última capability no destruye un objeto con ejecución activa | CUMPLIDO — el scheduler posee una referencia de ejecución propia |
| O4 | Un objeto terminado sigue siendo observable mientras exista una cap válida | CUMPLIDO (TCB TERMINATED responde GET_INFO) |
| O5 | El storage no se reutiliza hasta que: ejecución terminada ∧ referencias activas liberadas ∧ capabilities desaparecidas ∧ fuera de todo registro interno ∧ reaper completado | CUMPLIDO (destructor = único liberador de backing) |
| O6 | Reset/revoke de Untyped respeta descendencia y lifecycle | CUMPLIDO (`child_count != 0 → BUSY`; generación como testigo de reuso) |

### 2.3 IPC

| # | Invariante | Hoy |
|---|---|---|
| I1 | La transferencia de caps usa CSpace como origen y destino | PARCIAL — destino sí (receive slots); origen aún handle (Etapa 2) |
| I2 | Una transferencia fallida deja el estado equivalente al anterior | CUMPLIDO (staging peek/commit A1.9/A1.10) |
| I3 | La cap fuente no se consume antes de una entrega confirmada | CUMPLIDO |
| I4 | Reply es one-shot | CUMPLIDO (KReply explícito; doble REPLY → NOT_FOUND) |
| I5 | La identidad del emisor es infalsificable | CUMPLIDO (badge sellado; reply fuerza badge 0) |
| I6 | Cierre, muerte, cancelación y rollback tienen semántica determinista | CUMPLIDO (probado por lifecycle/estrés/fuzzing) |
| I7 | IPC no degrada silenciosamente a handles | PARCIAL — el fallback TOCTOU slot→handle existe, es contado (`iris_ipc_stat_toctou_fallbacks`) y se retira en Etapa 2 |

### 2.4 Scheduling

| # | Invariante | Hoy |
|---|---|---|
| S1 | TCB y SchedulingContext son objetos separados | CUMPLIDO |
| S2 | El TCB describe ejecución, no autoridad global de proceso | CUMPLIDO (KProcess separado, condenado — Etapa 7) |
| S3 | El SC representa presupuesto/política temporal delegable | CUMPLIDO (budget/period; donación pendiente — Etapa 8) |
| S4 | Bind/unbind de SC son capability-gated | CUMPLIDO (`SYS_SC_BIND` por CPtr; `THREAD_SET_SC` FROZEN) |
| S5 | El kernel no contiene política de servicios | CUMPLIDO (catálogo/restart/manifiestos en svcmgr) |

### 2.5 Memoria

| # | Invariante | Hoy |
|---|---|---|
| M1 | Frames, page tables, VSpace convergen a creación desde Untyped | PENDIENTE (Etapa 6; headers sidecar en ledger) |
| M2 | La autoridad de mapear procede de capabilities | CUMPLIDO (Frame/VSpace caps, RIGHT_MANAGE) |
| M3 | El kernel no asigna memoria de usuario implícitamente | CUMPLIDO (sin demand paging de kernel; pager ring-3) |
| M4 | Todo fallo parcial tiene rollback exacto | CUMPLIDO en RETYPE2/quotas; regla general para todo camino nuevo |
| M5 | La memoria compartida exige delegación explícita | CUMPLIDO (VMO share / file grants) |

### 2.6 Política

| # | Invariante | Hoy |
|---|---|---|
| P1 | Descubrimiento, restart, FS, pager, drivers, quotas de servicio y manifests viven en user space | CUMPLIDO |
| P2 | El kernel implementa mecanismo, no política de producto | PARCIAL — quotas por proceso y whitelist ioport en kernel (ledger; Etapas 6/7) |
| P3 | Una whitelist hardcodeada solo se tolera como bootstrap temporal con entrada en el ledger | CUMPLIDO (entrada añadida) |

## 3. Prohibiciones permanentes

Prohibido desde ya, sin excepción ni "temporalmente":

1. Añadir **nuevos productores de handles** (ningún syscall productivo nuevo
   retorna handles; ningún objeto canónico nuevo se inserta en la handle
   table).
2. Añadir **nuevos consumidores de handles** (ningún camino productivo nuevo
   llama a `handle_table_get_object` ni al resolver dual; guardado por
   `make check-purity`).
3. Crear objetos canónicos directamente desde **kslab** (guardado por
   `check_purity`; la lista cerrada de usos bootstrap está en el ledger).
4. Añadir **identificadores globales que confieran autoridad**.
5. Usar **PID, índice, dirección o puntero** como sustituto de una capability.
6. Introducir syscalls que acepten autoridad por **dos namespaces** (los
   dual-resolvers existentes son legacy congelado, no un patrón a imitar).
7. Incorporar **fallbacks de CPtr a handle** (el fallback TOCTOU de receive
   slots es la única excepción, contada y condenada — Etapa 2).
8. Confiar en **nombres de servicios** como autoridad (los nombres son
   descubrimiento; la autoridad es la cap entregada).
9. Añadir **política de restart, filesystem o drivers al kernel**.
10. Declarar **una migración terminada** mientras el camino productivo siga
    dependiendo del mecanismo anterior.

La allowlist de consumidores legacy (`scripts/purity_allowlist.txt`) solo
puede **decrecer**.  Crecerla exige modificar este charter y el ledger en el
mismo commit, con justificación técnica escrita.

## 4. Estado final obligatorio del capability model

El capability model se declara COMPLETO únicamente cuando todo esto sea
cierto y esté probado:

- [x] CDT/MDB nativo asociado a slots de CNode (parent/child global) —
      **Fase S3** (`docs/architecture/cspace-cdt-mdb.md`); revoke recursivo
      cross-process incluido.  Falta retirar el árbol handle-tree paralelo.
- [ ] Invocación CSpace-only: cero resolución dual, cero discriminación por
      rango de valor.
- [x] Revoke recursivo cross-process con rollback/cleanup determinista —
      **Fase S3** (`SYS_CSPACE_REVOKE`).
- [ ] Cap transfer por CPtr (origen y destino en CSpace) — destino CSpace
      (receive slots) ya; ORIGEN sigue handle (Etapa 2).
- [x] derive/mint/copy/move/delete/revoke operando sobre slots — **Fase S3**
      (primitivas `kcnode_slot_*`); `SYS_CSPACE_MINT`/`MINT_INTO`/`REVOKE`.
- [ ] Cero handles productivos; handle table eliminada o reducida a cero
      consumidores.
- [ ] Bootstrap con capabilities finas (BootInfo estructurado; sin
      `KBootstrapCap` monolítico).
- [ ] Todos los objetos canónicos nacidos de Untyped (incluido el TCB en
      ejecución, page tables, VSpace, headers de Frame).
- [ ] Ningún objeto de autoridad identificado por PID o índice global.
- [ ] Suite adversarial de lifecycle y revocación (creación, muerte cruzada,
      revocación en cadena, reuso de storage, stale caps) como gate
      permanente.

## 5. Prioridad rectora

```text
corrección de lifecycle
→ pureza de autoridad
→ atomicidad
→ aislamiento
→ separación mecanismo/política
→ extensibilidad
→ rendimiento
→ funcionalidades de sistema
```

Ninguna funcionalidad nueva justifica conservar una desviación estructural.
Cualquier divergencia semántica respecto a seL4 debe estar: (1) documentada,
(2) justificada técnicamente, (3) aislada, (4) cubierta por pruebas y
(5) marcada como temporal o deliberada — en el ledger si es temporal, en
este charter si es deliberada.

## 6. Divergencias deliberadas registradas

| Divergencia | Justificación | Estado |
|---|---|---|
| Sin verificación formal | fuera de alcance del proyecto; se compensa con gates adversariales | Deliberada permanente |
| ABI propia (no seL4) | IRIS no busca compatibilidad binaria | Deliberada permanente |
| `SYS_REPLY` separado (sin ReplyRecv combinado) | simplicidad del camino síncrono actual; revisar en Etapa 8 (MCS) | Deliberada, revisable |
| Reply objects con DUPLICATE (supervisor los minta al hijo) | patrón de supervisión de IRIS; documentado en RETYPE2 | Deliberada, revisable en Etapa 1 (CDT) |
| Untyped RESET (bump reset con child_count==0) además de revoke | útil como primitiva de reuso; el revoke real llega con CDT | Temporal hasta Etapa 1, luego revisable |
