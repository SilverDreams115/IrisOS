#include <iris/nameserver.h>
#include <iris/nc/spinlock.h>
#include <iris/nc/kobject.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <stdint.h>

typedef struct {
    char          name[NS_NAME_LEN];
    struct KObject *obj;
    struct KProcess *owner;
    iris_rights_t  rights;
    uint8_t        used;
} NsEntry;

static NsEntry    ns_table[NS_MAX_ENTRIES];
static spinlock_t ns_lock;

/* ── helpers ─────────────────────────────────────────────────────── */

static int ns_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void ns_strncpy(char *dst, const char *src, uint32_t n) {
    uint32_t i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static uint32_t ns_strnlen(const char *s, uint32_t max) {
    uint32_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

/* ── public API ──────────────────────────────────────────────────── */

void ns_init(void) {
    spinlock_init(&ns_lock);
    for (int i = 0; i < NS_MAX_ENTRIES; i++) {
        ns_table[i].used    = 0;
        ns_table[i].obj     = 0;
        ns_table[i].owner   = 0;
        ns_table[i].rights  = 0;
        ns_table[i].name[0] = '\0';
    }
}

iris_error_t ns_register(const char *name, struct KObject *obj,
                          iris_rights_t rights, struct KProcess *owner) {
    if (!name || !obj) return IRIS_ERR_INVALID_ARG;
    if (ns_strnlen(name, NS_NAME_LEN) >= NS_NAME_LEN)
        return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&ns_lock);

    for (int i = 0; i < NS_MAX_ENTRIES; i++) {
        if (ns_table[i].used && ns_strcmp(ns_table[i].name, name) == 0) {
            spinlock_unlock(&ns_lock);
            return IRIS_ERR_ALREADY_EXISTS;
        }
    }

    for (int i = 0; i < NS_MAX_ENTRIES; i++) {
        if (!ns_table[i].used) {
            ns_strncpy(ns_table[i].name, name, NS_NAME_LEN);
            kobject_retain(obj);
            ns_table[i].obj    = obj;
            ns_table[i].owner  = owner;
            ns_table[i].rights = rights;
            ns_table[i].used   = 1;
            spinlock_unlock(&ns_lock);
            return IRIS_OK;
        }
    }

    spinlock_unlock(&ns_lock);
    return IRIS_ERR_TABLE_FULL;
}

handle_id_t ns_lookup(const char *name, struct task *t,
                      iris_rights_t req_rights) {
    if (!name || !t || !t->process) return HANDLE_INVALID;

    spinlock_lock(&ns_lock);

    for (int i = 0; i < NS_MAX_ENTRIES; i++) {
        if (ns_table[i].used && ns_strcmp(ns_table[i].name, name) == 0) {
            struct KObject *obj    = ns_table[i].obj;
            iris_rights_t   rights = rights_reduce(ns_table[i].rights, req_rights);
            kobject_retain(obj);
            spinlock_unlock(&ns_lock);
            handle_id_t h = handle_table_insert(&t->process->handle_table, obj, rights);
            kobject_release(obj); /* table holds the reference */
            return h;
        }
    }

    spinlock_unlock(&ns_lock);
    return HANDLE_INVALID;
}

iris_error_t ns_unregister(const char *name) {
    if (!name) return IRIS_ERR_INVALID_ARG;

    spinlock_lock(&ns_lock);

    for (int i = 0; i < NS_MAX_ENTRIES; i++) {
        if (ns_table[i].used && ns_strcmp(ns_table[i].name, name) == 0) {
            kobject_release(ns_table[i].obj);
            ns_table[i].obj     = 0;
            ns_table[i].owner   = 0;
            ns_table[i].used    = 0;
            ns_table[i].name[0] = '\0';
            spinlock_unlock(&ns_lock);
            return IRIS_OK;
        }
    }

    spinlock_unlock(&ns_lock);
    return IRIS_ERR_NOT_FOUND;
}

void ns_unregister_owner(struct KProcess *owner) {
    if (!owner) return;

    spinlock_lock(&ns_lock);
    for (int i = 0; i < NS_MAX_ENTRIES; i++) {
        if (!ns_table[i].used || ns_table[i].owner != owner) continue;
        kobject_release(ns_table[i].obj);
        ns_table[i].obj     = 0;
        ns_table[i].owner   = 0;
        ns_table[i].rights  = 0;
        ns_table[i].used    = 0;
        ns_table[i].name[0] = '\0';
    }
    spinlock_unlock(&ns_lock);
}
