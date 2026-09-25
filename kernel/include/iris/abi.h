/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_ABI_H
#define IRIS_ABI_H

/*
 * abi.h — the IRIS 1.0 application binary interface, declared in one place.
 *
 * Stage 10-abi exists because "the correct state for a system in convergence
 * is the wrong state to ship".  Everything the kernel offers ring 3 was
 * DESCRIBED across three headers and a hundred and forty comments, and a
 * description spread that thin is not a contract: nobody can read it, nothing
 * can check it, and the only way to learn the surface was to read the
 * dispatcher.  This file is the contract.  The tests named at the bottom are
 * what make it one.
 *
 * ── The surface ────────────────────────────────────────────────────────────
 *
 * FOUR syscall numbers.  That is the whole numbered door:
 *
 *   SYS_INVOKE (144)     every operation on every object
 *   SYS_EXIT (1)         a thread ends itself
 *   SYS_YIELD (3)        a thread gives up the rest of its slice
 *   SYS_CLOCK_GET (62)   read the cycle counter
 *
 * The last three are here for the reason seL4 keeps `seL4_Yield`: they name no
 * capability, so there is nothing for them to be a method OF.  Every other
 * operation is an INVOCATION — a capability, a label that selects a method on
 * it, and up to four arguments.  A label is not authority: it says which
 * method, and the capability says whether you may.
 *
 * SEVENTY-SEVEN labels, 0 through IRIS_ABI_LABEL_MAX, with no holes.  Label 0
 * (`INV_INVALID`) names nothing and is refused exactly as an unassigned number
 * is.  The rest are in `iris/invoke.h`, grouped by the object type they act
 * on, and the grouping is documentation only: a label sent to the wrong kind
 * of capability is refused by the method's own type check, not by its number.
 *
 * ONE HUNDRED AND THIRTY-SIX reserved numbers.  Every number this kernel ever
 * exported and no longer honours stays reserved FOREVER and answers
 * IRIS_ERR_NOT_SUPPORTED.  A number is never reused, for the same reason a
 * label is never reused: a caller built against an older kernel must get a
 * refusal, not somebody else's method.  They keep their `SYS_*` names and
 * their history in `iris/syscall.h`, which is where the names are needed —
 * tests assert that each one is refused, and a name is how a test says which.
 *
 * Numbers that were NEVER exported are not "reserved", they are simply
 * unassigned, and they are refused by the same default.  So the property worth
 * freezing is not a list at all: it is that everything which is not one of the
 * four is refused, whatever it is called and whether or not it has a name.
 * That is what the test asserts, over every number the dispatcher can see.
 *
 * ── Versioning ─────────────────────────────────────────────────────────────
 *
 * MAJOR changes when something that used to work stops working: a syscall
 * number that used to reach a method, a label retired, an argument's meaning
 * changed, a struct field moved, an error code changed for a case that already
 * had one.
 *
 * MINOR changes when the surface GROWS in a way an existing caller cannot
 * notice: a new label at the end, a new struct field appended, a new flag bit
 * in a field whose unknown bits were already refused, a new error code for a
 * case that previously could not happen.
 *
 * Both are facts about the KERNEL, not about any one object, so they ride in
 * BootInfo — the structure the kernel already uses to hand the root task
 * everything else it cannot ask for.  There is no "what version are you"
 * invocation, and adding one would have been a fifth numbered door in all but
 * name: the version is not a property of any capability, so there is nothing
 * to invoke it on.
 *
 * ── The three rules that make growth safe ──────────────────────────────────
 *
 * 1. A VERSIONED STRUCT is a prefix.  Every struct the kernel writes into user
 *    memory starts with `version` and `struct_size`, a reader states how many
 *    bytes it can accept, and the kernel writes the smaller of the two.  A
 *    field is only ever APPENDED.  `struct iris_untyped_query` is where the
 *    pattern started and `struct iris_root_bootinfo` is where it matters most.
 *
 * 2. UNKNOWN BITS ARE REFUSED, not ignored.  A flags argument checks
 *    `flags & ~KNOWN` and answers IRIS_ERR_INVALID_ARG.  This is what lets a
 *    later minor version DEFINE such a bit: an old kernel refuses the new
 *    caller loudly instead of silently doing the old thing, which is the
 *    difference between a caller that can detect the kernel it is on and one
 *    that cannot.  `SYS_FRAME_MAP`'s uncached bit was added under this rule.
 *
 * 3. AN ERROR CODE IS PART OF THE CONTRACT.  A capability of the wrong type is
 *    WRONG_TYPE; a capability of the right type without the authority is
 *    ACCESS_DENIED; a label that names no method is NOT_SUPPORTED (ledger
 *    A-30).  Changing which of these a case produces is a MAJOR change, even
 *    though the call still fails, because callers branch on them.
 *
 * ── What this is NOT ───────────────────────────────────────────────────────
 *
 * Not binary compatibility with seL4.  It was never sought.  IRIS has seL4's
 * authority model, object model and execution model, and its own ABI — which
 * is a registered, deliberate divergence in the charter, and is a different
 * statement from "IRIS is seL4".
 */

/*
 * 1.0.
 *
 * MAJOR is 1 because the surface described above is the one being frozen.
 * MINOR is 0 and goes up when the surface grows under the three rules.
 */
#define IRIS_ABI_VERSION_MAJOR 1u
#define IRIS_ABI_VERSION_MINOR 0u

/* The numbered door, in full.  A test asserts that nothing else dispatches. */
#define IRIS_ABI_SYSCALL_COUNT 4u

/*
 * The invocation label space: 0..IRIS_ABI_LABEL_MAX, contiguous.
 *
 * Contiguity is asserted rather than assumed.  A hole would be a label that
 * LOOKS assigned — it is below the count, so a caller reading the header would
 * expect a method — and answers NOT_SUPPORTED, which is the same answer a
 * number past the end gives.  The two must not be confusable in a frozen ABI.
 */
#define IRIS_ABI_LABEL_MAX 76u

/*
 * How many numbers have been retired since IRIS started exporting any.
 *
 * Asserted, so that retiring one more without saying so here fails the build's
 * test rather than quietly changing what "the surface" means.
 */
#define IRIS_ABI_RESERVED_NUMBERS 136u

#endif /* IRIS_ABI_H */
