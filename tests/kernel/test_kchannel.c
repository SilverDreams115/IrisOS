#include "framework.h"
#include <iris/nc/kchannel.h>
#include <iris/nc/kobject.h>
#include <string.h>

void test_kchannel(void) {
    TEST_SUITE("kchannel");

    struct KChannel *ch = kchannel_alloc();
    ASSERT_NOT_NULL(ch);
    ASSERT_EQ(ch->count, 0u);
    ASSERT_EQ(ch->closed, 0);

    /* send a message to an unowned channel (no process quota involved) */
    struct KChanMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type       = 0xBEEFu;
    msg.data_len   = 4u;
    msg.data[0]    = 0xAA;
    iris_error_t r = kchannel_send(ch, &msg);
    ASSERT_EQ((int)r, (int)IRIS_OK);
    ASSERT_EQ(ch->count, 1u);

    /* try_recv retrieves the message */
    struct KChanMsg out;
    memset(&out, 0, sizeof(out));
    r = kchannel_try_recv(ch, &out);
    ASSERT_EQ((int)r, (int)IRIS_OK);
    ASSERT_EQ(ch->count, 0u);
    ASSERT_EQ((int)out.type, (int)0xBEEFu);
    ASSERT_EQ((int)out.data[0], (int)0xAA);

    /* try_recv on empty channel returns WOULD_BLOCK */
    r = kchannel_try_recv(ch, &out);
    ASSERT_EQ((int)r, (int)IRIS_ERR_WOULD_BLOCK);

    /* fill the ring buffer to capacity */
    for (uint32_t i = 0; i < KCHAN_CAPACITY; i++) {
        msg.type = i;
        r = kchannel_send(ch, &msg);
        ASSERT_EQ((int)r, (int)IRIS_OK);
    }
    ASSERT_EQ(ch->count, (uint32_t)KCHAN_CAPACITY);

    /* one more send on a full channel returns BUSY */
    msg.type = 0xFFFFu;
    r = kchannel_send(ch, &msg);
    ASSERT_TRUE(r != IRIS_OK);

    /* drain and verify FIFO order */
    for (uint32_t i = 0; i < KCHAN_CAPACITY; i++) {
        r = kchannel_try_recv(ch, &out);
        ASSERT_EQ((int)r, (int)IRIS_OK);
        ASSERT_EQ((int)out.type, (int)i);
    }
    ASSERT_EQ(ch->count, 0u);

    /* seal: marks closed; subsequent send returns CLOSED */
    kchannel_seal(ch);
    ASSERT_EQ(ch->closed, 1);
    r = kchannel_send(ch, &msg);
    ASSERT_EQ((int)r, (int)IRIS_ERR_CLOSED);

    /* try_recv on sealed empty channel returns CLOSED */
    r = kchannel_try_recv(ch, &out);
    ASSERT_EQ((int)r, (int)IRIS_ERR_CLOSED);

    /* release (no owner, so no quota ops) */
    kobject_release(&ch->base);
}
