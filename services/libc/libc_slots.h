/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_LIBC_SLOTS_H
#define IRIS_LIBC_SLOTS_H

#include <iris/program_abi.h>

/*
 * libc_slots.h — every CSpace slot this runtime takes, in one place.
 *
 * They used to be three `#define`s in three files, each with a comment saying
 * the program would never name one.  That is a convention, and a convention
 * between a library and a program that cannot see it is a collision waiting
 * for the first program that allocates a slot of its own: nothing would fail
 * at the point of the mistake, and the symptom would be a descriptor that
 * turned into somebody else's capability.
 *
 * So the contract states the split — `IRIS_PROG_SLOT_RT_LO..HI` in
 * `program_abi.h` — and these are the runtime's half of it, asserted against
 * that range by `tests/kernel/test_program_abi.c`.  A slot added here that
 * falls outside fails the build's own test rather than a program months later.
 */

/* The registered IPC buffer, and the page table its mapping needed. */
#define LIBC_SLOT_IPCBUF     36u
#define LIBC_SLOT_IPCBUF_PT  37u
/* The heap's page-table scratch.  Its FRAMES take slots from
 * IRIS_PROG_SLOT_FREE2_LO upward, well clear of this range. */
#define LIBC_SLOT_PT         38u

/*
 * Descriptors.  A descriptor IS the slot its capability lives in, so this
 * range is also the open-file limit: twenty-four, which is a real limit and is
 * stated rather than discovered.
 */
#define LIBC_SLOT_FD_LO      40u
#define LIBC_SLOT_FD_HI      63u

#endif /* IRIS_LIBC_SLOTS_H */
