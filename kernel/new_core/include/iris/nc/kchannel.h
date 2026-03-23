#ifndef IRIS_NC_KCHANNEL_H
#define IRIS_NC_KCHANNEL_H

#include <iris/nc/handle.h>
#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <iris/nc/rights.h>
#include <stdint.h>

#define KCHAN_CAPACITY      16  /* messages per channel ring buffer */
#define KCHAN_DATA_SIZE     64  /* bytes of payload per KChanMsg */
#define KCHANNEL_POOL_SIZE  32  /* maximum live KChannel objects system-wide */

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

struct task; /* forward declaration — no circular include */
struct KProcess;

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
    struct task    *waiter;                 /* task blocked on recv, or NULL */
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

#endif
