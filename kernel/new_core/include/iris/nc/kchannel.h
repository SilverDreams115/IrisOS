#ifndef IRIS_NC_KCHANNEL_H
#define IRIS_NC_KCHANNEL_H

/*
 * kchannel.h — IPC channel type.
 *
 * KChanMsg is the userland-visible IPC wire format and is always available.
 * struct KChannel and the channel API are kernel-internal (__KERNEL__ only).
 * Userland code (services) includes this header for KChanMsg only.
 */

#include <iris/nc/handle.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <stdint.h>

#define KCHAN_CAPACITY      32  /* messages per channel ring buffer */
#define KCHAN_DATA_SIZE     64  /* bytes of payload per KChanMsg */
#define KCHANNEL_POOL_SIZE  64  /* maximum live KChannel objects system-wide */

/*
 * KChanMsg — IPC message wire format.
 * Used by both kernel and userland; must remain stable.
 */
struct KChanMsg {
    uint32_t type;
    uint32_t sender_id;
    uint8_t  data[KCHAN_DATA_SIZE];
    uint32_t data_len;
    handle_id_t    attached_handle;   /* send: local handle to move, 0 = none
                                       * recv: installed handle in receiver table */
    iris_rights_t  attached_rights;   /* send: requested rights for moved handle
                                       * recv: granted rights */
};

#ifdef __KERNEL__
/*
 * Kernel-internal channel structures and API.
 * Not visible to userland services.
 */
#include <iris/nc/kobject.h>
#include <iris/task.h>

struct task;    /* forward declaration — no circular include */
struct KProcess;

#define KCHANNEL_WAITERS_MAX TASK_MAX /* bounded recv wait-set size per channel */

struct KChanQueuedHandle {
    struct KObject *object;
    iris_rights_t   rights;
    uint8_t         present;
};

struct KChannel {
    struct KObject  base;                   /* must be first */
    struct KChanMsg buf[KCHAN_CAPACITY];
    struct KChanQueuedHandle attached[KCHAN_CAPACITY];
    uint32_t        head;
    uint32_t        tail;
    uint32_t        count;
    uint8_t         closed;
    uint8_t         waiter_count;
    struct task    *waiters[KCHANNEL_WAITERS_MAX]; /* bounded blocked recv wait-set */
};

struct KChannel *kchannel_alloc     (void);
void             kchannel_free      (struct KChannel *ch);

iris_error_t     kchannel_send      (struct KChannel *ch, const struct KChanMsg *msg);
iris_error_t     kchannel_send_attached(struct KChannel *ch, const struct KChanMsg *msg,
                                        struct KObject *obj, iris_rights_t rights);
iris_error_t     kchannel_recv      (struct KChannel *ch, struct KChanMsg *out);
iris_error_t     kchannel_try_recv  (struct KChannel *ch, struct KChanMsg *out);
iris_error_t     kchannel_recv_into_process(struct KChannel *ch, struct KProcess *proc,
                                            struct KChanMsg *out);
iris_error_t     kchannel_try_recv_into_process(struct KChannel *ch, struct KProcess *proc,
                                                struct KChanMsg *out);
uint32_t         kchannel_live_count(void);
#endif /* __KERNEL__ */

#endif /* IRIS_NC_KCHANNEL_H */
