# IRIS — CSpace Capability Model (Fase S1)

Consolida el contrato A1 (`a1-authority-namespace-endgame.md`) con el modelo
S1.  Normativo para toda autoridad nueva.

## Namespaces

- **CPtr (< 1024)**: el CSpace es el namespace CANÓNICO de autoridad
  persistente y delegable.  Traversal radix sobre CNodes (potencias de 2,
  `ctzll(slot_count)` bits por nivel); slot 0 = CPTR_NULL.
- **handle (≥ 1024)**: materialización EFÍMERA por proceso (working set).
  Nunca un segundo namespace canónico.  `ACCESS_DENIED` en el CSpace es un
  hard stop — no hay fallback en ninguna dirección.
- Puente sancionado: `SYS_CSPACE_RESOLVE` / `SYS_CNODE_FETCH` (CSpace →
  handle).  La lista de productores de handles es cerrada (A1); Fase S1 no
  añadió ninguno y retiró tres (los create syscalls).

## Nacimiento de autoridad (S1)

Toda autoridad NUEVA de la familia migrada aparece en CSpace:
`SYS_UNTYPED_RETYPE2` publica las caps directamente en slots de un CNode
destino (0 = root del caller).  Invariantes S19/S20/S21: ningún objeto
migrado nace por quota, por handle, ni fuera de CSpace.

Convención S1 adicional: `SYS_CNODE_DELETE(0, slot)` opera sobre el root
CNode del propio caller (descartar autoridad propia no amplifica nada);
espeja el destino 0 de RETYPE2.

## Derivación, badges, revoke

Sin cambios de contrato en S1: mint/derive reducen rights monotónicamente;
badges estampados solo por la autoridad que mintea (una cap badged nunca se
re-badgea); revoke transitivo sobre el árbol de derivación de handles;
copias en CNodes son refs independientes (T127).  El CDT completo sobre
CSpace queda como fase CSpace-only (ledger).

## Reglas para código nuevo

1. Un tipo de objeto nuevo debe ser CSpace-invocable desde el día uno
   (dual resolver, jamás `handle_table_get_object` directo).
2. Prohibido añadir caminos productivos handle-first para objetos canónicos
   (ledger: handle table FROZEN para nuevos productores).
3. Prohibido añadir dual resolution a operaciones nuevas: las operaciones
   S1+ (p. ej. RETYPE2 destino) aceptan cptr-or-handle solo por el puente
   sancionado ya existente; el destino final es CSpace-only.
4. Los helpers legacy que necesiten handles se migran; no se agregan
   conversiones nuevas.
