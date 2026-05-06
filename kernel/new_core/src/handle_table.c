#include <iris/nc/handle_table.h>

/* genera el siguiente gen para un slot — nunca retorna 0 */
static uint32_t next_gen(uint32_t g) {
    g++;
    if (g == 0u) g = 1u;
    return g;
}

void handle_table_init(HandleTable *ht) {
    spinlock_init(&ht->lock);
    ht->next_hint = 0u;
    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
        ht->used[i]        = 0u;
        ht->gen[i]         = 0u;   /* 0 = nunca usado aún */
        ht->slots[i].object = 0;
        ht->slots[i].rights = RIGHT_NONE;
        ht->slots[i].gen    = 0u;
    }
}

handle_id_t handle_table_insert(HandleTable *ht, struct KObject *obj,
                                 iris_rights_t rights) {
    if (!obj) return HANDLE_INVALID;

    spinlock_lock(&ht->lock);

    /* búsqueda lineal desde next_hint */
    uint32_t start = ht->next_hint;
    uint32_t slot  = HANDLE_TABLE_MAX;

    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
        uint32_t idx = (start + i) % HANDLE_TABLE_MAX;
        if (!ht->used[idx]) {
            slot = idx;
            break;
        }
    }

    if (slot == HANDLE_TABLE_MAX) {
        spinlock_unlock(&ht->lock);
        return HANDLE_INVALID;
    }

    /* primer uso del slot: gen arranca en 1 */
    uint32_t g = (ht->gen[slot] == 0u) ? 1u : next_gen(ht->gen[slot]);
    ht->gen[slot] = g;

    handle_entry_init(&ht->slots[slot], obj, rights, g);
    ht->used[slot] = 1u;
    ht->next_hint  = (slot + 1u) % HANDLE_TABLE_MAX;

    handle_id_t id = handle_id_make(slot, g);
    spinlock_unlock(&ht->lock);
    return id;
}

iris_error_t handle_table_get_object(HandleTable *ht, handle_id_t id,
                                     struct KObject **out_obj,
                                     iris_rights_t   *out_rights) {
    if (!id || !out_obj || !out_rights) return IRIS_ERR_INVALID_ARG;

    uint32_t slot = handle_id_slot(id);
    uint32_t gen  = handle_id_gen(id);

    if (slot >= HANDLE_TABLE_MAX) return IRIS_ERR_BAD_HANDLE;

    spinlock_lock(&ht->lock);

    if (!ht->used[slot] || ht->gen[slot] != gen) {
        spinlock_unlock(&ht->lock);
        return IRIS_ERR_BAD_HANDLE;
    }

    struct KObject *obj = ht->slots[slot].object;
    kobject_retain(obj);          /* referencia fuerte para el caller */
    *out_obj    = obj;
    *out_rights = ht->slots[slot].rights;

    spinlock_unlock(&ht->lock);
    return IRIS_OK;
}

iris_error_t handle_table_replace(HandleTable *ht, handle_id_t id,
                                  struct KObject *new_obj) {
    if (!ht || !id || !new_obj) return IRIS_ERR_INVALID_ARG;

    uint32_t slot = handle_id_slot(id);
    uint32_t gen  = handle_id_gen(id);

    if (slot >= HANDLE_TABLE_MAX) return IRIS_ERR_BAD_HANDLE;

    spinlock_lock(&ht->lock);

    if (!ht->used[slot] || ht->gen[slot] != gen) {
        spinlock_unlock(&ht->lock);
        return IRIS_ERR_BAD_HANDLE;
    }

    iris_rights_t rights = ht->slots[slot].rights;
    handle_entry_reset(&ht->slots[slot]);
    handle_entry_init(&ht->slots[slot], new_obj, rights, gen);

    spinlock_unlock(&ht->lock);
    return IRIS_OK;
}

iris_error_t handle_table_close(HandleTable *ht, handle_id_t id) {
    if (!id) return IRIS_ERR_INVALID_ARG;

    uint32_t slot = handle_id_slot(id);
    uint32_t gen  = handle_id_gen(id);

    if (slot >= HANDLE_TABLE_MAX) return IRIS_ERR_BAD_HANDLE;

    spinlock_lock(&ht->lock);

    if (!ht->used[slot] || ht->gen[slot] != gen) {
        spinlock_unlock(&ht->lock);
        return IRIS_ERR_BAD_HANDLE;
    }

    handle_entry_reset(&ht->slots[slot]);   /* kobject_release interno */
    ht->used[slot] = 0u;
    ht->gen[slot]  = next_gen(ht->gen[slot]); /* invalida IDs viejos */

    spinlock_unlock(&ht->lock);
    return IRIS_OK;
}

void handle_table_close_all(HandleTable *ht) {
    spinlock_lock(&ht->lock);
    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
        if (ht->used[i]) {
            handle_entry_reset(&ht->slots[i]);
            ht->used[i]   = 0u;
            ht->gen[i]    = next_gen(ht->gen[i]);
        }
    }
    spinlock_unlock(&ht->lock);
}
