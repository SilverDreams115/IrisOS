#ifndef IRIS_IPC_MSG_H
#define IRIS_IPC_MSG_H

#include <stdint.h>

#define IRIS_MSG_WORDS    4u
#define IRIS_IPC_BUF_SIZE 256u  /* max extra payload bytes per endpoint IPC */
#define IRIS_MSG_NO_CAP   0u    /* attached_handle value meaning "no capability" */

/*
 * Inline IPC message — 64 bytes.
 *
 *   label / words[]: the primary inline payload (up to 4 machine words).
 *   word_count:      how many of words[] carry valid data (0..IRIS_MSG_WORDS).
 *   buf_len:         bytes of extra bulk payload (Ph69); 0 = none.
 *   buf_uptr:        On SEND: user pointer to the bulk source buffer.
 *                    On RECV (output): user pointer where bulk data was written.
 *   attached_handle: capability handle being transferred (Ph68); IRIS_MSG_NO_CAP = none.
 *   attached_rights: rights to grant on the received cap (Ph68).
 */
struct IrisMsg {
    uint64_t label;
    uint64_t words[IRIS_MSG_WORDS];
    uint32_t word_count;
    uint32_t buf_len;
    uint64_t buf_uptr;
    uint32_t attached_handle;  /* handle_id_t — uint32_t; 0 = IRIS_MSG_NO_CAP */
    uint32_t attached_rights;  /* iris_rights_t — uint32_t */
};
/* sizeof = 8 + 32 + 4 + 4 + 8 + 4 + 4 = 64 bytes */

#endif /* IRIS_IPC_MSG_H */
