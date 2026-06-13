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
        ht->used[i]               = 0u;
        ht->gen[i]                = 0u;
        ht->badge[i]              = 0u;   /* Fase 9 */
        ht->derivation_parent[i]  = HANDLE_INVALID;
        ht->slots[i].object       = 0;
        ht->slots[i].rights       = RIGHT_NONE;
        ht->slots[i].gen          = 0u;
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

/* Fase 9: insertion preserving an explicit badge (cap transfer, slot
 * materialization, dup).  Identical to handle_table_insert otherwise.
 * Handle-side badges are 32-bit (parallel array — see handle_table.h). */
handle_id_t handle_table_insert_badged(HandleTable *ht, struct KObject *obj,
                                       iris_rights_t rights, uint64_t badge) {
    handle_id_t id = handle_table_insert(ht, obj, rights);
    if (id == HANDLE_INVALID || badge == 0u) return id;
    spinlock_lock(&ht->lock);
    {
        uint32_t slot = handle_id_slot(id);
        if (ht->used[slot] && ht->gen[slot] == handle_id_gen(id))
            ht->badge[slot] = (uint32_t)badge;
    }
    spinlock_unlock(&ht->lock);
    return id;
}

/* Fase 9: read the badge of a live handle (0 = unbadged / not found). */
uint64_t handle_table_get_badge(HandleTable *ht, handle_id_t id) {
    if (!ht || !id) return 0u;
    uint32_t slot = handle_id_slot(id);
    uint32_t gen  = handle_id_gen(id);
    if (slot >= HANDLE_TABLE_MAX) return 0u;
    spinlock_lock(&ht->lock);
    uint64_t b = (ht->used[slot] && ht->gen[slot] == gen)
                 ? (uint64_t)ht->badge[slot] : 0u;
    spinlock_unlock(&ht->lock);
    return b;
}

handle_id_t handle_table_insert_derived(HandleTable *ht, struct KObject *obj,
                                         iris_rights_t rights,
                                         handle_id_t parent_handle) {
    if (!obj) return HANDLE_INVALID;

    spinlock_lock(&ht->lock);

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

    uint32_t g = (ht->gen[slot] == 0u) ? 1u : next_gen(ht->gen[slot]);
    ht->gen[slot] = g;

    handle_entry_init(&ht->slots[slot], obj, rights, g);
    ht->used[slot]              = 1u;
    ht->derivation_parent[slot] = parent_handle;
    ht->next_hint               = (slot + 1u) % HANDLE_TABLE_MAX;

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

    handle_entry_reset(&ht->slots[slot]);
    ht->used[slot]              = 0u;
    ht->gen[slot]               = next_gen(ht->gen[slot]);
    ht->badge[slot]             = 0u;   /* Fase 9: no stale identity */
    ht->derivation_parent[slot] = HANDLE_INVALID;

    spinlock_unlock(&ht->lock);
    return IRIS_OK;
}

void handle_table_close_all(HandleTable *ht) {
    spinlock_lock(&ht->lock);
    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
        if (ht->used[i]) {
            handle_entry_reset(&ht->slots[i]);
            ht->used[i]              = 0u;
            ht->gen[i]               = next_gen(ht->gen[i]);
            ht->badge[i]             = 0u;   /* Fase 9 */
            ht->derivation_parent[i] = HANDLE_INVALID;
        }
    }
    spinlock_unlock(&ht->lock);
}

void handle_table_revoke_children(HandleTable *ht, handle_id_t parent_h) {
    if (!parent_h) return;

    /* BFS worklist — O(N) total vs O(N×depth) multi-pass.
     * to_revoke[]: 1 byte per slot, 1024 bytes on stack.
     * worklist[]:  one uint32_t slot index per entry; at most HANDLE_TABLE_MAX. */
    uint8_t  to_revoke[HANDLE_TABLE_MAX];
    uint32_t worklist [HANDLE_TABLE_MAX];
    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) to_revoke[i] = 0u;
    uint32_t wl_head = 0u, wl_tail = 0u;

    spinlock_lock(&ht->lock);

    /* Seed: direct children of parent_h */
    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
        if (!ht->used[i]) continue;
        if (ht->derivation_parent[i] == parent_h) {
            to_revoke[i] = 1u;
            worklist[wl_tail++] = i;
        }
    }

    /* BFS: for each marked slot find its children and mark them */
    while (wl_head < wl_tail) {
        uint32_t    cur   = worklist[wl_head++];
        handle_id_t cur_h = handle_id_make(cur, ht->gen[cur]);
        for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
            if (!ht->used[i] || to_revoke[i]) continue;
            if (ht->derivation_parent[i] == cur_h) {
                to_revoke[i] = 1u;
                worklist[wl_tail++] = i;
            }
        }
    }

    /* Sweep: close all marked slots */
    for (uint32_t i = 0u; i < HANDLE_TABLE_MAX; i++) {
        if (!to_revoke[i]) continue;
        handle_entry_reset(&ht->slots[i]);
        ht->used[i]              = 0u;
        ht->gen[i]               = next_gen(ht->gen[i]);
        ht->derivation_parent[i] = HANDLE_INVALID;
    }

    spinlock_unlock(&ht->lock);
}
