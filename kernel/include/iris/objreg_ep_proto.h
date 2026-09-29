/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_OBJREG_EP_PROTO_H
#define IRIS_OBJREG_EP_PROTO_H

#include <stdint.h>

/*
 * objreg — the shared-object registry.
 *
 * Stage 10-run step 4.  It is the service that makes dynamic linking WORTH
 * anything: it loads an object once and mints a read-only capability for its
 * text to every process that needs it, so a library's code exists in memory
 * exactly once however many programs are running it.
 *
 * ── What is shared and what is not, and why the line is where it is ────────
 *
 * TEXT is shared.  One frame, mapped read+execute into every consumer, and a
 * consumer cannot write it because it was never given `RIGHT_WRITE` — not
 * because a page-table bit says so, though that too.  Both are true and the
 * capability is the one that matters: a holder with no write right cannot
 * obtain one, cannot ask for one, and cannot mint one for anybody else.
 *
 * DATA is not.  There is no copy-on-write in this system and there never will
 * be — demand paging was deliberately eliminated — so a shared writable data
 * segment would be silent sharing between processes that believe they are
 * isolated, which is the exact failure the whole model exists to prevent.  The
 * registry therefore publishes the data segment as a READ-ONLY master, and the
 * CONSUMER copies it into a frame from its OWN budget.  That puts the cost of
 * a library's data on the process that has it, where a budget can see it, and
 * it is what a dynamic loader with no COW has to do anyway.
 *
 * ── The registry can take it back ──────────────────────────────────────────
 *
 * Every capability it hands out is a derivation child of the master it holds,
 * so `CSpace_Revoke` on the master removes the object from every process at
 * once.  That is not a feature bolted on; it is what the MDB already does, and
 * it is the reason a registry is a better place for a library than a file each
 * process opens for itself.
 */

/* Slots in the registry's own CSpace.  1..19 keep the meanings
 * `endpoint_proto.h` gives them. */
#define OBJREG_SLOT_CTRL_EP   5u
#define OBJREG_SLOT_REPLY     6u
#define OBJREG_SLOT_VFS_EP    7u
#define OBJREG_SLOT_CONSOLE_EP 8u   /* it says what it loaded, and what it did not */

/*
 * OBJREG_OP_OPEN — load an object by path, once.
 *   Request:  payload = NUL-terminated path
 *   Reply OK: words[0] = object id
 *             words[1] = text bytes (the mapped size of the R+X segment)
 *             words[2] = data bytes (the mapped size of the R+W segment)
 * Opening a path that is already loaded answers the SAME id and loads nothing.
 * That is the whole point: a second consumer must not produce a second copy.
 */
#define OBJREG_OP_OPEN        0x7501u

/*
 * OBJREG_OP_TEXT — a READ-only capability for the object's text.
 *   Request:  words[0] = object id
 *   Reply OK: transfers the capability; words[0] = text bytes
 * READ only, and no WRITE and no TRANSFER: a consumer may map it and may ask
 * where it physically is (`Frame_GetAddress` needs READ), and may do nothing
 * else with it.
 */
#define OBJREG_OP_TEXT        0x7502u

/*
 * OBJREG_OP_DATA — a READ-only capability for the object's data MASTER.
 *   Request:  words[0] = object id
 *   Reply OK: transfers the capability; words[0] = data bytes
 * The consumer copies this into memory of its own.  It is deliberately not
 * writable: a writable master would be one segment shared by every process
 * that loaded the library, which is the thing this design refuses.
 */
#define OBJREG_OP_DATA        0x7503u

/*
 * OBJREG_OP_REVOKE — take the object back from everyone at once.
 *   Request:  words[0] = object id
 *   Reply OK: words[0] = capabilities destroyed under the text master
 *             words[1] = ...and under the data master
 * The COUNT, not a status: `CSpace_Revoke` answers how many it destroyed, and
 * zero is a perfectly good answer for an object nobody had taken yet.
 * A `CSpace_Revoke` on the masters.  Every capability derived from them —
 * in this service's callers, in their children, anywhere the derivation tree
 * reaches — is gone when this returns.  The masters themselves remain, so the
 * object can be handed out again.
 */
#define OBJREG_OP_REVOKE      0x7504u

/*
 * OBJREG_OP_INFO — what the registry is holding.
 *   Reply OK: words[0] = objects loaded
 *             words[1] = bytes of text held (which is the bytes NOT duplicated
 *                        per consumer, and therefore the number this service
 *                        exists to make large)
 */
#define OBJREG_OP_INFO        0x7505u

/*
 * ── What a CONSUMER says back, and why it is a protocol at all ─────────────
 *
 * Proving the sharing takes a rendezvous, not just a report.  The revoke
 * happens between the second round and the third, and without the parent being
 * able to say WHEN, there is no way to know which side of it a report came
 * from — a consumer that answered "still mine" could simply have been asked
 * too early.  So each round is a Call: the consumer says what it sees and
 * blocks until the parent has done the next thing.
 *
 *   MAPPED: words[0] = the PHYSICAL address of the library's text
 *           words[1] = the physical address of this process's own data copy
 *           words[2] = which consumer is speaking
 *   BEFORE: words[0] = what the object capability identifies as (a frame)
 *   AFTER:  words[0] = the error asking now gives (it is gone)
 */
#define OBJREG_ROUND_MAPPED   0x7601u
#define OBJREG_ROUND_BEFORE   0x7602u
#define OBJREG_ROUND_AFTER    0x7603u
/* What the parent replies with to release a consumer into the next round. */
#define OBJREG_ROUND_GO       0x7604u

#define OBJREG_REP_OK         0x7580u
#define OBJREG_REP_ERR        0x7581u

/* How many objects it can hold.  Small, and a limit rather than a growth
 * policy: a registry that allocated a table would need somewhere to allocate
 * it from, and this service's budget is for the objects themselves. */
#define OBJREG_MAX_OBJECTS    4u

#endif /* IRIS_OBJREG_EP_PROTO_H */
