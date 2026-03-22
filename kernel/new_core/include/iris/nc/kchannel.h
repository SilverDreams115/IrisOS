#ifndef IRIS_NC_KCHANNEL_H
#define IRIS_NC_KCHANNEL_H

#include <iris/nc/kobject.h>
#include <iris/nc/error.h>
#include <stdint.h>

#define KCHAN_CAPACITY  16
#define KCHAN_DATA_SIZE 64

struct KChanMsg {
    uint32_t type;
    uint32_t sender_id;
    uint8_t  data[KCHAN_DATA_SIZE];
    uint32_t data_len;
};

struct task; /* forward declaration — no circular include */

struct KChannel {
    struct KObject  base;                   /* must be first */
    struct KChanMsg buf[KCHAN_CAPACITY];
    uint32_t        head;
    uint32_t        tail;
    uint32_t        count;
    uint8_t         closed;
    struct task    *waiter;                 /* task blocked on recv, or NULL */
};

struct KChannel *kchannel_alloc     (void);
void             kchannel_free      (struct KChannel *ch);

iris_error_t     kchannel_send      (struct KChannel *ch, const struct KChanMsg *msg);
iris_error_t     kchannel_recv      (struct KChannel *ch, struct KChanMsg *out);
iris_error_t     kchannel_try_recv  (struct KChannel *ch, struct KChanMsg *out);

#endif
