# The IRIS 1.0 ABI

**The normative contract is `kernel/include/iris/abi.h`, and the thing that
makes it a contract is `tests/kernel/test_abi.c`.**  This document is for
readers; the header is for callers and the test is for the build.  If the two
ever disagree, the header and the test are right and this file is stale — which
is the failure this whole stage exists to fix, so it is worth saying first.

## Why the stage existed

The paragraph that opened the roadmap's Stage 10-abi said IRIS had "96 live
syscalls".  That was true when it was written and had been wrong since ledger
A-32 converted the surface to invocations.  The ceiling section of the same
document described IRIS as having "numbered syscalls, each resolving its own
arguments" — also true once, also wrong for several stages, and sitting in the
one section whose job is to keep the document honest about its limits.

Nothing was checking. That is the whole diagnosis, and every decision below
follows from it.

## The surface

| | |
|---|---|
| **syscall numbers** | four: `SYS_INVOKE` (144), `SYS_EXIT` (1), `SYS_YIELD` (3), `SYS_CLOCK_GET` (62) |
| **invocation labels** | 77, numbered 0..76, contiguous |
| **reserved numbers** | 136, permanently refused |

The three numbered calls that are not `SYS_INVOKE` are there for the reason
seL4 keeps `seL4_Yield`: they name no capability, so there is nothing for them
to be a method OF.  Everything else is `SYS_INVOKE(cptr, label, a1..a4)` — a
capability, a label that selects a method on it, and arguments.

**A label is not authority.**  It says which method; the capability says
whether you may.  A label sent to the wrong kind of capability is refused by
the method's own type check, not by its number, which is why the grouping in
`iris/invoke.h` is documentation and not enforcement.

**Contiguity is asserted, not assumed.**  A hole would be a label that LOOKS
assigned — below the count, so a caller reading the header expects a method —
and answers `NOT_SUPPORTED`, which is what a label past the end also answers.
In a frozen ABI those two must not be confusable.

**A number is never reused.**  Neither is a label.  A caller built against an
older kernel must get a refusal, not somebody else's method.

## Versioning

`abi_major` and `abi_minor` are in **BootInfo**, not behind an invocation.

The version is a fact about the KERNEL, so there is no capability to invoke it
on; a syscall for it would have been a fifth numbered door in all but name.
BootInfo is already what the root task is handed for exactly the things it
cannot ask anyone for.

**MAJOR** changes when something that used to work stops: a number that
reached a method, a label retired, an argument's meaning changed, a struct
field moved, an error code changed for a case that already had one.

**MINOR** changes when the surface grows in a way an existing caller cannot
notice: a label appended, a struct field appended, a flag bit defined whose
unknown state was already refused, an error code for a case that previously
could not happen.

The root task halts the boot on a major mismatch.  Without that, a caller
discovers the mismatch one `NOT_SUPPORTED` at a time, from a method that used
to exist, with no author.

## The three rules that make growth safe

Each promotes a precedent that already existed in the tree to a rule.

**1. A versioned struct is a PREFIX.**  Every struct the kernel writes into
user memory starts with `version` and `struct_size`; the caller declares how
many bytes it can accept and the kernel writes the smaller of the two.  Fields
are only ever APPENDED.  `struct iris_untyped_query` is where the pattern
started; `struct iris_root_bootinfo` is where it matters most.

**2. An unknown bit is REFUSED, not ignored.**  A flags argument checks
`flags & ~KNOWN` and answers `IRIS_ERR_INVALID_ARG`.  This is what lets a later
minor version define such a bit: an old kernel refuses the new caller loudly
instead of silently doing the old thing.  `SYS_FRAME_MAP`'s uncached bit was
added under this rule, and `tests/kernel/test_kframe.c` FR-61 pins the
boundary moving.

**3. An error code is part of the contract.**  Wrong type is `WRONG_TYPE`;
right type without the authority is `ACCESS_DENIED`; a label that names no
method is `NOT_SUPPORTED` (ledger A-30).  Changing which of these a case
produces is a MAJOR change even though the call still fails, because callers
branch on them.

## What this is not

**Not binary compatibility with seL4.**  It was never sought.  IRIS has seL4's
authority model, object model and execution model, and its own ABI.  seL4 code
does not build against IRIS and never will; that is a registered, deliberate
divergence in the charter and a different statement from "IRIS is seL4".

**Not a promise about anything above the kernel.**  Service endpoint protocols
(`*_ep_proto.h`) are between a service and its clients, not part of this
surface.  They version themselves or they do not; the kernel has no opinion.

## What freezing it found

`handle_id_t` and `HANDLE_INVALID` survived in 1341 places across 42 files,
naming a namespace Stage 4 deleted.  Most of that was cosmetic.  One place was
not: `sys_sc_configure` read `handle_id_t sc_h = (handle_id_t)arg0`, truncating
a 64-bit capability argument to 32 bits and widening it again at the resolver —
so a value ABOVE the CPtr/handle boundary, which is exactly the bit pattern
that boundary exists to refuse, was folded back INSIDE the valid range.

The type is `iris_cptr_t` everywhere now, `nc/handle.h` is `nc/cptr.h`, and
`CPTR_NULL` and `IRIS_CPTR_NULL` are one name instead of two.
