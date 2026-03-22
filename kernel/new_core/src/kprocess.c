#include <iris/nc/kprocess.h>
#include <iris/nc/handle_table.h>
#include <stdint.h>

#define POOL_SIZE 16
static struct KProcess pool[POOL_SIZE];
static uint8_t         pool_used[POOL_SIZE];

static void kprocess_destroy(struct KObject *obj) {
    struct KProcess *p = (struct KProcess *)obj;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (&pool[i] == p) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kprocess_ops = {
    .destroy = kprocess_destroy
};

struct KProcess *kprocess_alloc(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KProcess *p = &pool[i];
            uint8_t *raw = (uint8_t *)p;
            for (uint32_t j = 0; j < sizeof(*p); j++) raw[j] = 0;
            kobject_init(&p->base, KOBJ_PROCESS, &kprocess_ops);
            handle_table_init(&p->handle_table);
            p->brk = USER_HEAP_BASE;
            return p;
        }
    }
    return 0;
}

void kprocess_free(struct KProcess *p) {
    kobject_release(&p->base);
}
