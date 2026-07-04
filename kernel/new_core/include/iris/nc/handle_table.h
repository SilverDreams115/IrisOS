#ifndef IRIS_NC_HANDLE_TABLE_H
#define IRIS_NC_HANDLE_TABLE_H

#include <iris/nc/handle.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>

#define HANDLE_TABLE_MAX 1024u

typedef struct {
    struct HandleEntry slots[HANDLE_TABLE_MAX];
    uint32_t           gen[HANDLE_TABLE_MAX];              /* generation por slot */
    uint32_t           badge[HANDLE_TABLE_MAX];            /* Fase 9: per-cap badge
                                                            * (32-bit, 0 = unbadged) */
    uint8_t            used[HANDLE_TABLE_MAX];
    handle_id_t        derivation_parent[HANDLE_TABLE_MAX]; /* HANDLE_INVALID = root cap */
    uint32_t           next_hint;
    spinlock_t         lock;
} HandleTable;

/*
 * Invariants:
 *   - HANDLE_INVALID is never assigned to a live slot
 *   - gen[slot] >= 1 for slots that have been used at least once
 *   - gen[slot] wraps by skipping 0
 *   - handle_table_get_object returns a strong (retained) reference
 *   - Every operation acquires table->lock internally
 *   - HANDLE_TABLE_MAX is the absolute ceiling — no dynamic growth
 *   - handle_table_close_all is the only bulk-close path
 *
 * A1 closeout — role of this table (docs/architecture/
 * a1-authority-namespace-endgame.md):
 *   The handle table is the process's EPHEMERAL working set, not a
 *   canonical authority namespace.  Persistent, delegable authority lives
 *   in the process's CSpace (root CNode) and is invoked by CPtr (< 1024);
 *   handle ids (slot | gen << HANDLE_GEN_SHIFT, always >= 1024) are
 *   materializations with a closed producer list: object-creation returns,
 *   handle-layer ops (DUP/DERIVE), IPC cap delivery, one-shot reply caps
 *   (intentional ephemeral exception), SYS_CSPACE_RESOLVE / SYS_CNODE_FETCH
 *   (the sanctioned CSpace→handle bridge), and kernel bootstrap.
 *   Do NOT route new persistent authority through this table: new object
 *   types get a dual resolver (cspace_or_handle_resolve_*) so they are
 *   CSpace-invocable; handle-only resolution of a persistent cap is a
 *   design regression.
 */

void         handle_table_init(HandleTable *ht);
handle_id_t  handle_table_insert(HandleTable *ht, struct KObject *obj,
                                 iris_rights_t rights);
/* Fase 9: insertion preserving an explicit badge; read a handle's badge
 * (0 = unbadged or not found). */
handle_id_t  handle_table_insert_badged(HandleTable *ht, struct KObject *obj,
                                        iris_rights_t rights, uint64_t badge);
uint64_t     handle_table_get_badge(HandleTable *ht, handle_id_t id);
handle_id_t  handle_table_insert_derived(HandleTable *ht, struct KObject *obj,
                                          iris_rights_t rights,
                                          handle_id_t parent_handle);
iris_error_t handle_table_get_object(HandleTable *ht, handle_id_t id,
                                     struct KObject **out_obj,
                                     iris_rights_t   *out_rights);
iris_error_t handle_table_replace(HandleTable *ht, handle_id_t id,
                                  struct KObject *new_obj);
iris_error_t handle_table_close(HandleTable *ht, handle_id_t id);
void         handle_table_close_all(HandleTable *ht);
void         handle_table_revoke_children(HandleTable *ht, handle_id_t parent_h);

#endif
