# IRIS — Untyped Object Creation (Fase S1, normativo)

Complementa [`sel4-canonical-object-model.md`](sel4-canonical-object-model.md)
y sucede a `untyped-retype-revoke-hardening.md` (Fase 18) como contrato del
sustrato de asignación.

## KUntyped — identidad y layout

```
struct KUntyped {
    KObject   base;         /* header primero */
    lock;                   /* IRQ-off spinlock: bump/reset */
    phys_base, total_size;  /* rango físico EXACTO, sin overlap (U1/U2) */
    used;                   /* bump offset — solo crece (U6) salvo RESET */
    child_count;            /* objetos/sub-untypeds vivos dentro (U10) */
    is_device;              /* device: sin zero-fill, tipos restringidos */
    alloc_parent;           /* sub-untyped → padre (bookkeeping) */
    generation;             /* +1 por RESET exitoso — testigo de reuse */
}
```

- Tamaño: los boot-Untypeds son bloques buddy (potencia de dos); los
  sub-untypeds usan el contrato explícito "bytes múltiplos de página".
- Estado de asignación: **watermark monotónico** (`used`).  Se eligió sobre
  bitmap/árbol por determinismo, atomicidad trivial, auditabilidad
  (`used_bytes` observable) y porque preserva no-overlap estructuralmente:
  un rango carved nunca se re-entrega antes de RESET, y RESET exige
  `child_count == 0`.  No es un allocator general: es una operación de
  retype sobre una región de autoridad explícita.
- Derivación: `KOBJ_UNTYPED` retype crea sub-untypeds con back-pointer al
  padre; el padre no puede RESET mientras el hijo viva (descendants).

## Invariantes U1–U15

```
U1  una región física pertenece a un solo Untyped raíz (boot drain: bloques
    buddy disjuntos; sub-untypeds carved exclusivamente del padre)
U2  dos Untyped vivos no se solapan (carve exclusivo + no unbump)
U3  un objeto retipado reside completamente dentro del Untyped fuente
    (bloque = header+payload carved del rango; T252)
U4  el objeto respeta tamaño/alineación de su tipo (validación por tipo;
    asserts de alineación ≤ KUNTYPED_ALIGN; físicos: página)
U5  una región no respalda dos objetos vivos (watermark; T253/T259)
U6  retype solo reduce capacidad (bump monotónico)
U7  derivar caps (resolve/derive/mint) no consume memoria física (T262)
U8  delete de una cap no destruye el objeto si quedan caps (T255)
U9  revoke elimina descendants de autoridad (árbol handle-table HOY;
    CDT CSpace = S2, ledger)
U10 la región solo es reutilizable sin objetos ni caps que la retengan
    (child_count gate en RESET; T259)
U11 device Untyped solo produce UNTYPED/FRAME
U12 normal Untyped no produce autoridad de dispositivo
U13 overflow/rangos inválidos fallan antes de mutar estado (validación
    íntegra previa; T254)
U14 retype por lotes es atómico (carve único bajo lock + publicación
    verificada; T253)
U15 fallo parcial no consume memoria (rollback exacto kuntyped_unbump_exact
    + destroy de objetos no publicados; T253)
```

Nota de atomicidad: IRIS es hoy uniprocesador con spinlocks IRQ-off y kernel
no-preemptivo (sin yield dentro de retype), por lo que la secuencia
validar→reservar→inicializar→publicar→commit es atómica frente a cualquier
otro syscall; los locks conservan la disciplina para un futuro SMP.

## SYS_UNTYPED_RETYPE2 (111) — camino canónico

```
RETYPE2(ut, type | count<<32, dest_cnode | slot<<32, obj_arg) → 0 | error
```

- `ut`: cap Untyped (CPtr <1024 o handle ≥1024), RIGHT_WRITE.
- `count`: 0→1, máx 32 (batch ≤128 KiB); UNTYPED/FRAME exigen count=1 en S1.
- `dest_cnode`: 0 = root CNode del caller; si no, cap CNode con RIGHT_WRITE.
- `slot`: primera ranura; `[slot, slot+count)` deben existir y estar vacías;
  slot 0 (CPTR_NULL) se rechaza.
- `obj_arg`: CNODE → num_slots (pot. de 2, ≤4096); UNTYPED/FRAME → bytes.

Validaciones previas (sin mutación): CPtr fuente válido y de tipo Untyped;
rights; tipo permitido (manifiesto cerrado, T251); size/count válidos;
multiplicaciones sin overflow; capacidad; alineación; CNode destino válido
y escribible; slots vacíos; restricción device/normal; límites (`KCNODE_MAX_SLOTS`,
`KUNTYPED_RETYPE_MAX_*`).

Commit:

```
validate everything
reserve complete range          (kuntyped_alloc_children_atomic: un lock)
initialize every object         (placement en la región, zero-filled)
prepare every capability
publish all destination slots   (una sección crítica del CNode)
commit Untyped state
```

Fallo en cualquier punto ⇒ ninguna cap publicada, ningún objeto vivo,
ningún byte consumido, ningún slot mutado, sin drift de contadores.

Las capabilities aparecen **directamente en CSpace**; no se crean handles ni
se devuelve autoridad por índices o punteros.

## SYS_UNTYPED_RETYPE (87) — legacy TRANSITIONAL

Restringido a los tipos NO migrados: `UNTYPED`, `FRAME`, `SCHED_CONTEXT`.
La familia migrada (ENDPOINT/NOTIFICATION/CNODE/REPLY) devuelve
`NOT_SUPPORTED` (S20 — ningún objeto migrado nace por handle).  Registrado
en el ledger como MIGRATING; se retira con la fase CSpace-only.

## SYS_UNTYPED_RESET (88)

`child_count == 0` → `used = 0`, `generation++`, contadores de reclaim/reuse.
`BUSY` en caso contrario (S13).  La reutilización nunca expone estado previo:
los bloques se cero-rellenan al destruir Y al carve (S28), y la identidad de
capability impide que un protocolo viejo alcance el objeto nuevo (S29/T259);
`generation` es el testigo observable.  No quedan pointers kernel directos
que sobrevivan a la reutilización física: colas de EP/notification se
desencolan en close/cancel, `pending_kreply`/`ep_reply_obj` se limpian en
teardown, y las rutas IRQ retienen la notification (child_count > 0 hasta
des-registrar) — por eso no se añadió una generación por-objeto (regla:
no agregar generation donde la identidad de la cap ya cierra el stale path).

## SYS_UNTYPED_QUERY (112) — instrumentación (nunca autoridad)

- kind 1: global — `live_untypeds, retype_count, retype_failures,
  reset_count, reclaimed_bytes, reuse_count, overlap_denials`.
- kind 2: por-Untyped — `phys_base, total, used, generation, child_count,
  is_device` (RIGHT_READ sobre el cap).
- kind 3: gauges por tipo migrado — `endpoints/notifications/replies/cnodes
  live` (los high-water/retype/destroy por tipo se derivan de los contadores
  globales + gauges; los contadores por tipo de Fase 18 siguen en
  SYS_SCHED_INFO ext3).

Estructuras versionadas (`version`, `struct_size`).  No se creó ningún
syscall de resource-domain nuevo ni creció `SYS_RESOURCE_INFO`.

## Bootstrap y delegación

```
kernel (PMM drain) → userboot [slots 16..]
userboot → init      [slot 12, un bloque]
init     → svcmgr    [slot 12, sub-untyped 256 KiB]
init     → iris_test [slot 55, sub-untyped 8 MiB]
svcmgr   → por servicio: EP/notification retipados del pool + reply
           sub-untyped de 4 KiB por servicio (RESET+retype en cada respawn)
```

Least authority: cada servicio recibe sus endpoints, notifications y reply
objects minteados — nunca el Untyped raíz.  El pager y el VFS no poseen
Untyped global (verificado por los report-slot masks, T156/T162/T201+).

## Fallos y pruebas

Failure paths cubiertos por T253 (batch parcial/capacidad), T254 (validación
completa, stale cap, dest inválido, device/normal), T259 (retención por cap
viva, reuse limpio), T262 (stress determinista con modelo shadow exacto).
Provenance: T252 (consumo exacto de región + child_count + kslab delta 0).
Retiro legacy: T260 y T125/T126 adaptados.  Servicios reales: T261.
