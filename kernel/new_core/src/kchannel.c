#include <iris/nc/kchannel.h>
#include <iris/task.h>
#include <stdint.h>

#define POOL_SIZE 32
static struct KChannel pool[POOL_SIZE];
static uint8_t         pool_used[POOL_SIZE];

static void kchannel_destroy(struct KObject *obj) {
    struct KChannel *ch = (struct KChannel *)obj;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (&pool[i] == ch) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kchannel_ops = { .destroy = kchannel_destroy };

struct KChannel *kchannel_alloc(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KChannel *ch = &pool[i];
            /* zero all fields */
            uint8_t *p = (uint8_t *)ch;
            for (uint32_t j = 0; j < sizeof(*ch); j++) p[j] = 0;
            kobject_init(&ch->base, KOBJ_CHANNEL, &kchannel_ops);
            return ch;
        }
    }
    return 0;
}

void kchannel_free(struct KChannel *ch) {
    kobject_release(&ch->base);
}

static void msg_copy(struct KChanMsg *dst, const struct KChanMsg *src) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < sizeof(struct KChanMsg); i++) d[i] = s[i];
}

iris_error_t kchannel_try_recv(struct KChannel *ch, struct KChanMsg *out) {
    spinlock_lock(&ch->base.lock);
    if (ch->count == 0) {
        spinlock_unlock(&ch->base.lock);
        return IRIS_ERR_WOULD_BLOCK;
    }
    msg_copy(out, &ch->buf[ch->head]);
    ch->head  = (ch->head + 1) % KCHAN_CAPACITY;
    ch->count--;
    spinlock_unlock(&ch->base.lock);
    return IRIS_OK;
}

iris_error_t kchannel_send(struct KChannel *ch, const struct KChanMsg *msg) {
    spinlock_lock(&ch->base.lock);
    if (ch->count >= KCHAN_CAPACITY) {
        spinlock_unlock(&ch->base.lock);
        return IRIS_ERR_OVERFLOW;
    }
    msg_copy(&ch->buf[ch->tail], msg);
    ch->tail  = (ch->tail + 1) % KCHAN_CAPACITY;
    ch->count++;

    /* wake blocked receiver */
    struct task *w = ch->waiter;
    if (w && (w->state == TASK_BLOCKED || w->state == TASK_BLOCKED_IPC)) {
        w->state   = TASK_READY;
        ch->waiter = 0;
    }
    spinlock_unlock(&ch->base.lock);
    return IRIS_OK;
}

iris_error_t kchannel_recv(struct KChannel *ch, struct KChanMsg *out) {
    for (;;) {
        iris_error_t r = kchannel_try_recv(ch, out);
        if (r == IRIS_OK) return IRIS_OK;

        spinlock_lock(&ch->base.lock);
        if (ch->count == 0) {
            struct task *t = task_current();
            if (ch->waiter && ch->waiter != t) {
                /* Another task is already blocked on this channel.
                 * Multiple concurrent receivers are not supported — fail fast
                 * rather than silently overwriting the existing waiter. */
                spinlock_unlock(&ch->base.lock);
                return IRIS_ERR_BUSY;
            }
            if (t) {
                ch->waiter = t;
                t->state   = TASK_BLOCKED_IPC;
            }
            spinlock_unlock(&ch->base.lock);
            task_yield();
            /* resumed by sender; outer loop will retry kchannel_try_recv */
        } else {
            spinlock_unlock(&ch->base.lock);
        }
    }
}
