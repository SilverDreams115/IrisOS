#include <iris/nc/handle.h>

void handle_entry_init(struct HandleEntry *e, struct KObject *obj,
                       iris_rights_t rights, uint32_t gen) {
    kobject_retain(obj);
    kobject_active_retain(obj);
    e->object = obj;
    e->rights = rights;
    e->gen    = gen;
}

void handle_entry_reset(struct HandleEntry *e) {
    if (e->object) {
        kobject_active_release(e->object);
        kobject_release(e->object);
        e->object = 0;
    }
    e->rights = RIGHT_NONE;
    e->gen    = 0u;
}
