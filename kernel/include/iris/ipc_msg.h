#ifndef IRIS_IPC_MSG_H
#define IRIS_IPC_MSG_H

#include <stdint.h>

#define IRIS_MSG_WORDS 4u

/* Inline IPC message — 48 bytes, fits in two cache lines. */
struct IrisMsg {
    uint64_t label;
    uint64_t words[IRIS_MSG_WORDS];
    uint32_t word_count;
    uint32_t _pad;
};

#endif /* IRIS_IPC_MSG_H */
