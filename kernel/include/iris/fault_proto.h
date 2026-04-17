#ifndef IRIS_FAULT_PROTO_H
#define IRIS_FAULT_PROTO_H

/*
 * Fault notification protocol — message sent over the registered exception
 * handler channel when a ring-3 task triggers a hardware exception.
 *
 * Wire layout (KChanMsg.data[64]):
 *   offset  0: uint32_t vector     — x86 exception vector (0-31)
 *   offset  4: uint32_t task_id    — task id of the faulting task
 *   offset  8: uint64_t rip        — instruction pointer at fault
 *   offset 16: uint32_t error_code — CPU error code (0 if N/A for this vector)
 *   offset 20: uint32_t _pad
 *   offset 24: uint64_t cr2        — fault address (#PF only; 0 otherwise)
 *
 * After sending the notification the kernel kills the faulting task.
 * The handler is responsible for any recovery (e.g. spawning a replacement).
 */
#define FAULT_MSG_NOTIFY     0xF0000001u
#define FAULT_OFF_VECTOR      0   /* uint32_t: exception vector */
#define FAULT_OFF_TASK_ID     4   /* uint32_t: faulting task id */
#define FAULT_OFF_RIP         8   /* uint64_t: rip at fault */
#define FAULT_OFF_ERROR      16   /* uint32_t: error code */
#define FAULT_OFF_CR2        24   /* uint64_t: #PF address (vector==14 only) */
#define FAULT_MSG_LEN        32u

#endif /* IRIS_FAULT_PROTO_H */
