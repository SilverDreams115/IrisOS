#ifndef IRIS_NC_KOBJECT_H
#define IRIS_NC_KOBJECT_H

#include <stdint.h>
#include <stdatomic.h>
#include <iris/nc/spinlock.h>

typedef enum {
    KOBJ_PROCESS,
    KOBJ_CHANNEL,
    KOBJ_NOTIFICATION,
    KOBJ_VMO,
} kobject_type_t;

struct KObject;

struct KObjectOps {
    void (*close)(struct KObject *obj);
    void (*destroy)(struct KObject *obj);
};

struct KObject {
    kobject_type_t            type;
    _Atomic uint32_t          refcount;  /* lifecycle — no estado */
    _Atomic uint32_t          active_refs; /* handles/global published refs */
    spinlock_t                lock;      /* estado mutable — no lifecycle */
    const struct KObjectOps  *ops;       /* != NULL para objeto inicializado */
};

/*
 * Invariantes:
 *   - ops != NULL para todo KObject inicializado
 *   - ops->destroy() es la única ruta de destrucción real
 *   - refcount >= 1 mientras exista HandleEntry apuntando al objeto
 *   - ops->destroy() se llama exactamente una vez, cuando refcount llega a 0
 *   - ops->destroy() nunca toca refcount
 *   - refcount y lock protegen cosas distintas — nunca se usan para lo mismo
 */

void kobject_init(struct KObject *obj, kobject_type_t type,
                  const struct KObjectOps *ops);
void kobject_retain(struct KObject *obj);
void kobject_release(struct KObject *obj);
void kobject_active_retain(struct KObject *obj);
void kobject_active_release(struct KObject *obj);

#endif
