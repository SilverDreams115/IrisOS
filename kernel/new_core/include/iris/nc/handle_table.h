#ifndef IRIS_NC_HANDLE_TABLE_H
#define IRIS_NC_HANDLE_TABLE_H

#include <iris/nc/handle.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/error.h>

#define HANDLE_TABLE_MAX 1024u

typedef struct {
    struct HandleEntry slots[HANDLE_TABLE_MAX];
    uint32_t           gen[HANDLE_TABLE_MAX];   /* generation por slot */
    uint8_t            used[HANDLE_TABLE_MAX];
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
 */

void         handle_table_init(HandleTable *ht);
handle_id_t  handle_table_insert(HandleTable *ht, struct KObject *obj,
                                 iris_rights_t rights);
iris_error_t handle_table_get_object(HandleTable *ht, handle_id_t id,
                                     struct KObject **out_obj,
                                     iris_rights_t   *out_rights);
iris_error_t handle_table_close(HandleTable *ht, handle_id_t id);
void         handle_table_close_all(HandleTable *ht);

#endif
