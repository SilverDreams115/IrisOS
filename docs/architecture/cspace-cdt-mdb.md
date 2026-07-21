# IRIS — CDT/MDB nativo de CSpace (Fase S3, normativo)

Modelo de derivación de capabilities asociado a los slots de CNode.
Implementa el charter §2.1/A9-A10 (derivación rastreable + revoke recursivo
cross-process) y la Etapa 1 del [roadmap](sel4-convergence-roadmap.md).

## 1. Definiciones exactas

1. **Capability** = el contenido de un slot CSpace ocupado: `(object, rights,
   badge, nodo MDB)`.  La autoridad ES el slot; dos slots al mismo objeto son
   dos autoridades distintas.
2. **Slot** = una entrada `struct KCSlot` dentro del array inline de un
   `struct KCNode`.  Vacío ⇔ `object == NULL` ⇔ nodo MDB vacío.
3. **Identidad interna de slot** = el par (`KCNode*`, índice), materializado
   como puntero directo `struct KCSlot *` (el array vive dentro del storage
   del CNode y es estable durante toda la vida del CNode).  Cada slot lleva
   un back-pointer `mdb_cnode` a su CNode propietario.  Ninguna identidad
   se deriva de PID, índice global ni direcciones de user space.
4. **Metadata MDB por slot** (intrusiva, cero allocation por operación):
   `mdb_parent`, `mdb_first_child`, `mdb_next_sib`, `mdb_prev_sib`
   (punteros a slots), `mdb_cnode` (CNode propietario) y `mdb_flags`
   (`MDB_LEGACY_ROOT`).  Es un árbol explícito parent/first-child/sibling
   con lista de hermanos doblemente enlazada (O(1) unlink).
5. **Raíz** = nodo con `mdb_parent == NULL`.  Toda raíz es hoy una
   `LEGACY_ROOT` (instalada desde un origen no-CSpace: handle, objeto de
   kernel en bootstrap, entrega IPC staged) o una **promovida** (huérfana
   tras la delete de una raíz — contada en `mdb_orphan_promotions`).  Las
   caps canónicas (copy/mint/retype con fuente slot) SIEMPRE tienen padre.
6. **Descendiente** = alcanzable desde un nodo bajando por
   `first_child/next_sib`.  La relación cruza CNodes y procesos: los enlaces
   son punteros a slots, no importan el KProcess propietario.
7. **Copy** = derivar con `RIGHT_SAME_RIGHTS`: nuevo slot con el mismo
   objeto, mismos rights efectivos y badge heredado; hijo MDB de la fuente.
8. **Mint** = derivar con reducción: `effective = src & requested` (colapso
   a NONE → INVALID_ARG); badge según la regla central §3; hijo MDB de la
   fuente.  Exige `RIGHT_DUPLICATE` en la fuente.
9. **Move** = trasplantar la capability y SU NODO: el slot destino hereda
   objeto/rights/badge/padre/hijos/posición entre hermanos; los
   `mdb_parent` de todos los hijos se reapuntan; el slot fuente queda
   completamente vacío.  No crea derivación ni toca refcounts netos
   (transferencia interna balanceada).
10. **Delete** = eliminar SOLO la capability seleccionada.  Sus hijos se
    **reparentan al abuelo** (spliced en la lista de hijos del padre del
    borrado), preservando la autoridad de revocación de todo ancestro
    superviviente.  Si el borrado era raíz, cada hijo se promueve a raíz
    (no hay ancestro superviviente — justificación única) y se cuenta.
11. **Revoke** = eliminar TODO el subárbol descendiente del slot invocado,
    conservando el slot invocado.  Orden determinista: hoja más profunda
    por la izquierda primero (post-orden iterativo, O(1) de estado).
    Atraviesa CNodes y procesos.  Los siblings del invocado y sus
    descendientes NO se tocan.
12. **Delete de nodo intermedio** — regla de reparación: véase (10); el
    invariante preservado es «si A es ancestro de C antes del delete de B
    (A≠B), A sigue siendo ancestro de C después».
13. **Destrucción de subárbol** (revoke): por cada slot víctima, bajo lock:
    unlink del árbol + vaciado del slot; fuera del lock: release de las
    referencias del objeto (active + lifecycle).  El cierre/destrucción del
    objeto (despertar bloqueados, devolver storage al Untyped) ocurre por
    su lifecycle normal cuando las referencias caen — la desaparición de
    AUTORIDAD nunca espera al storage.
14. **Untyped**: `RETYPE2` con fuente CPtr registra cada cap creada como
    hija del SLOT del untyped fuente.  Con fuente handle (legacy), las caps
    nacen `LEGACY_ROOT` (métrica `mdb_legacy_roots`; frontera I.1 congelada).
    Revocar el slot de un untyped elimina toda su descendencia de caps; los
    objetos mueren cuando sus referencias drenan y sus destructores
    devuelven el storage (child_count--).  `UNTYPED_RESET` sigue exigiendo
    `child_count == 0`: child_count es la verdad del LIFECYCLE DE OBJETOS
    (un objeto puede seguir vivo por handles o por tasks bloqueados sin que
    exista slot alguno); el CDT es la verdad de la AUTORIDAD.  Reset jamás
    puede reutilizar storage con objetos vivos porque el destructor es el
    único que decrementa child_count.
15. **Lifecycle de objetos**: el MDB solo mueve referencias (retain/release
    por slot).  La última referencia dispara el destructor del objeto; un
    TCB en ejecución conserva la referencia de ejecución del scheduler, así
    que revocar toda su autoridad NO libera su backing (queda sin autoridad
    → termina → destructor).  Autoridad, cierre funcional, destrucción y
    liberación de storage son cuatro eventos distintos (charter O3-O5).
16. **Teardown de procesos**: `handle_table_close_all` suelta el root CNode;
    el close del CNode borra cada slot con la primitiva de DELETE (reparent),
    por lo que los descendientes en otros procesos sobreviven y quedan
    reparentados a ancestros supervivientes; ningún enlace apunta al CNode
    muerto (todos sus slots salen del grafo antes de liberar su storage).
17. **Locking**: un lock global del MDB (`mdb_lock`, irq-spinlock) protege
    TODOS los enlaces de derivación y las mutaciones de ocupación de slots.
    Orden fijo: `mdb_lock → cn->lock` (nunca al revés).  Los lectores del
    resolver (kcnode_fetch / cspace walk) siguen usando solo `cn->lock`: no
    leen enlaces MDB.  PROHIBIDO ejecutar callbacks destructivos
    (kobject_active_release / kobject_release) con `mdb_lock` tomado — todo
    release ocurre tras soltar el lock (los destructores de CNode reentran
    en el MDB).  El lock no duerme.  Es la estrategia S3 (uniprocesador);
    su refinamiento por-CNode es prerrequisito de SMP (Etapa 9), no de esta
    fase — pero la corrección ya NO depende de "no hay preemption": depende
    del lock.
18. **Complejidad**: copy/mint/install O(1); move O(hijos directos);
    delete O(hijos directos) (splice); revoke O(nodos del subárbol) con
    O(subárbol) adquisiciones de lock (una por víctima — ventanas IRQ-off
    cortas); validador O(slots del conjunto²) solo en tests.
19. **Límites estructurales**: sin allocation dinámica (los nodos viven en
    los slots); profundidad acotada solo por el número de slots vivos; el
    validador impone una cota de sanidad (2^20) contra ciclos.
20. **Divergencias temporales respecto a seL4** (ledger):
    - raíces LEGACY (origen handle) — se retiran con Etapas 2-4;
    - `SYS_CAP_DERIVE/SYS_CAP_REVOKE` handle-only siguen existiendo
      (árbol paralelo de la handle table, congelado) — Etapa 3;
    - IPC cap-transfer instala LEGACY_ROOT (origen handle) — Etapa 2;
    - sin badged-CNode guards ni CDT sobre Untyped→Untyped anidado más
      allá de alloc_parent;
    - delete reparenta (seL4-MDB lo hace implícito por orden de lista);
      semántica equivalente para revocación, documentada aquí.

## 2. Primitivas canónicas (kcnode.c — únicas mutadoras de slots)

```text
kcnode_slot_install_root(cn, idx, obj, rights, badge, excl, legacy)
kcnode_slot_install_derived(cn, idx, obj, rights, badge, src_slot)  [excl]
kcnode_slot_move(src_cn, src_idx, dst_cn, dst_idx)                  [dst excl]
kcnode_slot_delete(cn, idx)          — reparent hijos, libera refs fuera de lock
kcnode_slot_revoke(cn, idx)          — subárbol post-orden, conserva el slot
kcnode_swap → dos moves con slot temporal lógico (mismo CNode)
kcnode_obj_close → slot_delete de cada slot ocupado
```

Ningún syscall ni TU manipula `cn->slots[]` directamente.  `kcnode_mint*`
se conservan como wrappers de `install_root(legacy=1)` para los caminos
legacy existentes (bootstrap, proc_cspace_mint, receive-slot, CNODE_MOVE).

## 3. Regla central de badge (una sola función)

`mdb_badge_derive(src_badge, requested, obj_type)`:
- `requested == 0` → hereda `src_badge` (0 sigue 0);
- `src_badge != 0 && requested != src_badge` → `ACCESS_DENIED` (jamás
  re-badge);
- `src_badge == 0 && requested != 0` → solo `KOBJ_ENDPOINT` /
  `KOBJ_NOTIFICATION` (`INVALID_ARG` para el resto).

Usada por TODA derivación (SYS_CSPACE_MINT, SYS_CSPACE_MINT_INTO y el
legacy proc_cspace_mint, que delega en ella).

## 4. Superficie de syscalls (Fase S3)

| Syscall | Semántica |
|---|---|
| `SYS_CSPACE_MINT (114)` | copy/mint slot→slot en el propio CSpace.  arg0 = src CPtr (solo CSpace); arg1 = dest CNode CPtr (0 = root) \| dest slot << 32; arg2 = rights (RIGHT_SAME_RIGHTS ⇒ copy) \| badge << 32.  Instalación exclusiva. |
| `SYS_CSPACE_REVOKE (115)` | revoca los descendientes del slot en arg0 (CPtr propio); el slot sobrevive.  Retorna el número de nodos revocados. |
| `SYS_CSPACE_MINT_INTO (116)` | mint cross-process: arg0 = proceso destino (dual, RIGHT_WRITE — patrón de target existente); arg1 = slot destino en su root CNode; arg2 = src CPtr (solo CSpace del caller); arg3 = rights \| badge << 32.  Instalación exclusiva; hijo MDB del slot fuente del caller. |

`SYS_CNODE_DELETE` conserva su número y adquiere la semántica delete-con-
reparent.  `SYS_CNODE_SWAP` repara enlaces.  `SYS_CNODE_MOVE` (handle→slot)
sigue produciendo `LEGACY_ROOT`.

## 5. Invariantes estructurales (validador `kcnode_mdb_validate`)

B.3-1..14 del contrato de fase: slot vacío ⇔ metadata vacía; sin ciclos;
un padre como máximo; listas de hermanos bidireccionales coherentes;
`parent.first_child` alcanza exactamente a sus hijos; siblings sobreviven a
revoke ajeno; move conserva descendencia; delete ≠ revoke; reuse de slot no
hereda metadata; ningún enlace a CNode destruido (garantizado por close);
fallo ⇒ grafo intacto.  El validador opera sobre un conjunto cerrado de
CNodes (tests host y fuzzing model-based); no añade coste al camino
productivo.

## 6. Atomicidad

Toda operación sigue `validar → preparar → commit bajo mdb_lock → efectos
de lifecycle fuera del lock`.  Un fallo antes del commit no muta nada
(instalaciones exclusivas re-verifican ocupación bajo el lock).  Revoke es
una secuencia de deletes de hoja individualmente atómicos con orden
determinista — un corte a mitad deja un subárbol válido más pequeño, nunca
enlaces colgantes.  El rollback de publicación de RETYPE2 borra
exactamente los nodos hoja recién instalados (sin hijos por construcción).
