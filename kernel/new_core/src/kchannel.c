#include <iris/nc/kchannel.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kprocess.h>
#include <iris/task.h>
#include <stdint.h>

static struct KChannel pool[KCHANNEL_POOL_SIZE];
static uint8_t         pool_used[KCHANNEL_POOL_SIZE];

static void kchannel_waiters_clear(struct KChannel *ch) {
    if (!ch) return;
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) ch->waiters[i] = 0;
    ch->waiter_count = 0;
}

static void kchannel_waiters_wake_all(struct KChannel *ch) {
    if (!ch) return;
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) {
        struct task *w = ch->waiters[i];
        if (!w) continue;
        if (w->state == TASK_BLOCKED || w->state == TASK_BLOCKED_IPC)
            w->state = TASK_READY;
        ch->waiters[i] = 0;
    }
    ch->waiter_count = 0;
}

/* Wake exactly one blocked receiver; remove it from the array.
 * Used by send: one message → one receiver, no thundering herd. */
static void kchannel_waiters_wake_one(struct KChannel *ch) {
    if (!ch) return;
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) {
        struct task *w = ch->waiters[i];
        if (!w) continue;
        if (w->state == TASK_BLOCKED || w->state == TASK_BLOCKED_IPC) {
            w->state      = TASK_READY;
            ch->waiters[i] = 0;
            if (ch->waiter_count) ch->waiter_count--;
            return;
        }
    }
}

static int kchannel_waiters_contains(const struct KChannel *ch, const struct task *t) {
    if (!ch || !t) return 0;
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) {
        if (ch->waiters[i] == t) return 1;
    }
    return 0;
}

static iris_error_t kchannel_waiters_enqueue(struct KChannel *ch, struct task *t) {
    if (!ch || !t) return IRIS_ERR_INVALID_ARG;
    if (kchannel_waiters_contains(ch, t)) return IRIS_OK;
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) {
        if (ch->waiters[i]) continue;
        ch->waiters[i] = t;
        ch->waiter_count++;
        return IRIS_OK;
    }
    return IRIS_ERR_TABLE_FULL;
}

void kchannel_cancel_waiter(struct task *t) {
    if (!t) return;

    for (int i = 0; i < KCHANNEL_POOL_SIZE; i++) {
        struct KChannel *ch;

        if (!pool_used[i]) continue;
        ch = &pool[i];

        spinlock_lock(&ch->base.lock);
        for (uint32_t j = 0; j < KCHANNEL_WAITERS_MAX; j++) {
            if (ch->waiters[j] != t) continue;
            ch->waiters[j] = 0;
            if (ch->waiter_count != 0)
                ch->waiter_count--;
        }
        spinlock_unlock(&ch->base.lock);
    }
}

static void queued_handle_reset(struct KChanQueuedHandle *qh) {
    if (!qh || !qh->present) return;
    if (qh->object) {
        kobject_active_release(qh->object);
        kobject_release(qh->object);
    }
    qh->object = 0;
    qh->rights = RIGHT_NONE;
    qh->present = 0;
}

static void kchannel_close(struct KObject *obj) {
    struct KChannel *ch = (struct KChannel *)obj;
    spinlock_lock(&ch->base.lock);
    ch->closed = 1;
    kchannel_waiters_wake_all(ch);
    spinlock_unlock(&ch->base.lock);
}

static void kchannel_destroy(struct KObject *obj) {
    kchannel_close(obj);
    struct KChannel *ch = (struct KChannel *)obj;
    for (uint32_t i = 0; i < KCHAN_CAPACITY; i++)
        queued_handle_reset(&ch->attached[i]);
    for (int i = 0; i < KCHANNEL_POOL_SIZE; i++) {
        if (&pool[i] == ch) { pool_used[i] = 0; return; }
    }
}

static const struct KObjectOps kchannel_ops = {
    .close = kchannel_close,
    .destroy = kchannel_destroy
};

struct KChannel *kchannel_alloc(void) {
    for (int i = 0; i < KCHANNEL_POOL_SIZE; i++) {
        if (!pool_used[i]) {
            pool_used[i] = 1;
            struct KChannel *ch = &pool[i];
            /* zero all fields */
            uint8_t *p = (uint8_t *)ch;
            for (uint32_t j = 0; j < sizeof(*ch); j++) p[j] = 0;
            kobject_init(&ch->base, KOBJ_CHANNEL, &kchannel_ops);
            kchannel_waiters_clear(ch);
            return ch;
        }
    }
    return 0;
}

void kchannel_free(struct KChannel *ch) {
    kobject_release(&ch->base);
}

int kchannel_is_readable(struct KChannel *ch) {
    int r;
    if (!ch) return 0;
    spinlock_lock(&ch->base.lock);
    r = (ch->count > 0) ? 1 : 0;
    spinlock_unlock(&ch->base.lock);
    return r;
}

iris_error_t kchannel_waiters_add_checked(struct KChannel *ch, struct task *t) {
    iris_error_t r;
    if (!ch || !t) return IRIS_ERR_INVALID_ARG;
    spinlock_lock(&ch->base.lock);
    r = kchannel_waiters_enqueue(ch, t);
    spinlock_unlock(&ch->base.lock);
    return r;
}

void kchannel_waiters_remove_task(struct KChannel *ch, struct task *t) {
    if (!ch || !t) return;
    spinlock_lock(&ch->base.lock);
    for (uint32_t i = 0; i < KCHANNEL_WAITERS_MAX; i++) {
        if (ch->waiters[i] != t) continue;
        ch->waiters[i] = 0;
        if (ch->waiter_count) ch->waiter_count--;
    }
    spinlock_unlock(&ch->base.lock);
}

void kchannel_seal(struct KChannel *ch) {
    if (!ch) return;
    spinlock_lock(&ch->base.lock);
    ch->closed = 1;
    kchannel_waiters_wake_all(ch);
    spinlock_unlock(&ch->base.lock);
}

static void msg_copy(struct KChanMsg *dst, const struct KChanMsg *src) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t       *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < sizeof(struct KChanMsg); i++) d[i] = s[i];
}

iris_error_t kchannel_try_recv(struct KChannel *ch, struct KChanMsg *out) {
    return kchannel_try_recv_into_process(ch, 0, out);
}

iris_error_t kchannel_try_recv_into_process(struct KChannel *ch, struct KProcess *proc,
                                            struct KChanMsg *out) {
    spinlock_lock(&ch->base.lock);
    if (ch->count == 0 && ch->closed) {
        spinlock_unlock(&ch->base.lock);
        return IRIS_ERR_CLOSED;
    }
    if (ch->count == 0) {
        spinlock_unlock(&ch->base.lock);
        return IRIS_ERR_WOULD_BLOCK;
    }
    msg_copy(out, &ch->buf[ch->head]);
    if (ch->attached[ch->head].present) {
        if (!proc) {
            spinlock_unlock(&ch->base.lock);
            return IRIS_ERR_INVALID_ARG;
        }

        handle_id_t h = handle_table_insert(&proc->handle_table,
                                            ch->attached[ch->head].object,
                                            ch->attached[ch->head].rights);
        if (h == HANDLE_INVALID) {
            spinlock_unlock(&ch->base.lock);
            return IRIS_ERR_TABLE_FULL;
        }
        out->attached_handle = h;
        out->attached_rights = ch->attached[ch->head].rights;
        queued_handle_reset(&ch->attached[ch->head]);
    } else {
        out->attached_handle = HANDLE_INVALID;
        out->attached_rights = RIGHT_NONE;
    }
    ch->head  = (ch->head + 1) % KCHAN_CAPACITY;
    ch->count--;
    spinlock_unlock(&ch->base.lock);
    return IRIS_OK;
}

iris_error_t kchannel_send(struct KChannel *ch, const struct KChanMsg *msg) {
    return kchannel_send_attached(ch, msg, 0, RIGHT_NONE);
}

iris_error_t kchannel_send_attached(struct KChannel *ch, const struct KChanMsg *msg,
                                    struct KObject *obj, iris_rights_t rights) {
    spinlock_lock(&ch->base.lock);
    if (ch->closed) {
        spinlock_unlock(&ch->base.lock);
        return IRIS_ERR_CLOSED;
    }
    if (ch->count >= KCHAN_CAPACITY) {
        spinlock_unlock(&ch->base.lock);
        return IRIS_ERR_OVERFLOW;
    }
    msg_copy(&ch->buf[ch->tail], msg);
    ch->attached[ch->tail].object = obj;
    ch->attached[ch->tail].rights = obj ? rights : RIGHT_NONE;
    ch->attached[ch->tail].present = obj ? 1u : 0u;
    ch->tail  = (ch->tail + 1) % KCHAN_CAPACITY;
    ch->count++;

    /* wake one blocked receiver: one message, one wakeup, no thundering herd */
    kchannel_waiters_wake_one(ch);
    spinlock_unlock(&ch->base.lock);
    return IRIS_OK;
}

iris_error_t kchannel_recv(struct KChannel *ch, struct KChanMsg *out) {
    struct task *t = task_current();
    return kchannel_recv_into_process(ch, t ? t->process : 0, out);
}

iris_error_t kchannel_recv_into_process(struct KChannel *ch, struct KProcess *proc,
                                        struct KChanMsg *out) {
    for (;;) {
        iris_error_t r = kchannel_try_recv_into_process(ch, proc, out);
        if (r == IRIS_OK || r == IRIS_ERR_CLOSED) return r;
        if (r == IRIS_ERR_TABLE_FULL || r == IRIS_ERR_INVALID_ARG) return r;

        spinlock_lock(&ch->base.lock);
        if (ch->closed && ch->count == 0) {
            spinlock_unlock(&ch->base.lock);
            return IRIS_ERR_CLOSED;
        }
        if (ch->count == 0) {
            struct task *t = task_current();
            if (t) {
                r = kchannel_waiters_enqueue(ch, t);
                if (r != IRIS_OK) {
                    spinlock_unlock(&ch->base.lock);
                    return r;
                }
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

uint32_t kchannel_live_count(void) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < KCHANNEL_POOL_SIZE; i++) {
        if (pool_used[i]) live++;
    }
    return live;
}
