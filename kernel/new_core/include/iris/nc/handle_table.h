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
 * Invariantes:
 *   - HANDLE_INVALID nunca asignado
 *   - gen[slot] >= 1 para slots usados al menos una vez
 *   - gen[slot] salta 0 en wrap
 *   - handle_table_get_object retorna referencia fuerte (retenida)
 *   - Toda operación toma table->lock internamente
 *   - HANDLE_TABLE_MAX es el techo absoluto — sin crecimiento
 *   - handle_table_close_all es la única forma de vaciado masivo
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
