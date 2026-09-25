/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_IPC_STAGE_H
#define IRIS_IPC_STAGE_H

#include <stdint.h>
#include <iris/ipc_msg.h>

/*
 * The kernel's per-thread message staging (ledger A-33).
 *
 * This used to be `struct IrisMsg` in the shared ABI header, and ring 3 handed
 * the kernel a POINTER to one.  It is not ABI any more — a message arrives in
 * registers and leaves in registers — and what survives is exactly what it
 * always really was: the place a message waits inside the kernel between a
 * sender being queued and a receiver taking it.
 *
 * Nothing in ring 3 can see this, which is why the fields can be whatever
 * suits the transfer rather than whatever suits a struct layout somebody has
 * to keep compatible.
 */
struct ipc_stage {
    uint64_t label;
    uint64_t words[IRIS_MSG_WORDS];
    uint32_t word_count;
    uint32_t buf_len;
    uint32_t attached_handle;  /* what was delivered; on a Call receive, the
                                * reply capability the receiver staged */
    uint32_t attached_rights;
    uint64_t sender_badge;     /* kernel-stamped; never anything the sender said */
    uint32_t attached_cap;     /* a Call's transferred capability, kept apart
                                * from attached_handle because a Call carries
                                * both a reply capability and a gift */
    uint32_t attached_cap_rights;
};

#endif /* IRIS_IPC_STAGE_H */
