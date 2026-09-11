# Syscall ABI Contract

## Purpose

Defines the current syscall compatibility surface implemented by the live IRIS
tree.

This document is descriptive, not aspirational. If code and docs disagree,
code wins until the docs are corrected.

## The shape of the surface

Since ledger **A-32** there are **three syscall numbers**, and each is there
because it invokes nothing:

| Number | Name | Why it is a number |
|---|---|---|
| 1 | `SYS_EXIT` | a thread ending itself names no object |
| 3 | `SYS_YIELD` | seL4 keeps `seL4_Yield` for exactly this reason |
| 62 | `SYS_CLOCK_GET` | A-27 established it is unprivileged on this architecture; retiring it would have bought nothing, so it was answered rather than removed |

Everything else is an **invocation**:

```
SYS_INVOKE(cptr, label, a1 … a7)      /* number 144 */
```

The capability says WHAT is being acted on, the label says WHICH method, and
neither can be given without the other. That is the property the syscall
number could never have: a number named a method and nothing else.

Every other number in the table answers `IRIS_ERR_NOT_SUPPORTED` — the
dispatcher's switch has no case for it. A number is never reused.

## Register convention

| Register | Raw arg | For an invocation |
|---|---|---|
| `rax` | — | the syscall number |
| `rdi` | arg0 | the capability |
| `rsi` | arg1 | the label |
| `rdx` | arg2 | a1 |
| `r10` | arg3 | a2 |
| `r8` | arg4 | a3 |
| `r9` | arg5 | a4 |
| `r15` | arg6 | a5 (ledger A-33) |
| `r14` | arg7 | a6 (ledger A-33) |
| `r13` | arg8 | a7 (ledger A-33) |

`rcx` and `r11` are the machine's: `syscall` puts the user's RIP and RFLAGS
there, and `sysret` takes them back.

Nine argument registers is what the MESSAGE ABI needs, not what a method
needs: a send carries a MessageInfo word, four message registers, a capability
to transfer and a receive slot, and an invocation's own `cptr`/`label` sit in
front of all of it. No method takes more than three arguments of its own.

A caller that passes fewer arguments than the widest form **must still present
defined registers — zero.** A method that later reads a further argument reads
that register, and what the compiler left in it is not zero. Zero has a
defined meaning wherever an argument has been added ("no destination", "my own
budget"), so a stub that zeroes the tail degrades to the previous behaviour
instead of resolving garbage.

## The label space

`kernel/include/iris/invoke.h` holds it: **62 labels**, `INV_INVALID` = 0, one
FLAT list. seL4's `enum invocation_label` is flat for a reason worth
restating — a type-scoped label space forces the dispatcher to learn the
capability's type before it can pick the method, which is a CSpace walk the
method then repeats. Flat labels route on the label alone and the method does
the one walk it always did.

The type is checked where it always was: inside the method, by the resolver
that fetches the capability with the type it requires. A label sent to the
wrong kind of capability answers `IRIS_ERR_WRONG_TYPE` (ledger A-30) — which
says what is actually wrong, and is a better answer than seL4's
`IllegalOperation` for the same mistake.

| Group | Labels |
|---|---|
| TCB | 1–14: Suspend, Resume, SetPriority, Exit, GetInfo, ReadRegs, WriteRegs, Configure, Watch, SetFaultHandler, SetTimeoutHandler, ExitCode, SetIPCBuffer, BindNotification |
| Endpoint | 15–21: Send, NBSend, Recv, NBRecv, Call, CancelBadgedSends, ReplyRecv |
| Notification | 22–24: Signal, Wait, Poll |
| Reply | 25: Send |
| Untyped | 26–30: Info, Query, Reset, Retype, SetDeviceBudget |
| CNode | 31–32: Delete, Swap |
| Scheduling context | 33–37: Bind, Consumed, YieldTo, Configure, SetOnCaller |
| Frame | 38–40: Map, Unmap, Size |
| PageTable | 41: Map |
| ASIDPool | 42: Assign |
| IRQ | 43–45: SetNotification, Ack, Clear |
| IOPort | 46–47: In, Out |
| Boot authority | 48–56: FramebufferInfo, InitrdCount, InitrdFrame, IOPortNarrow, CreateIOPort, CreateIRQCap, KlogDrain, SchedInfo, Poweroff |
| Slot methods | 57–62: CapIdentify, CapSameObject, CSpaceMint, CSpaceMove, CSpaceRevoke, CSpaceSetGuard |

The last group acts on the SLOT rather than on what it holds, which is why
those labels are valid whatever the capability is.

## The message ABI

Since ledger **A-33** a message is a MessageInfo word plus message registers,
and a payload longer than that lives in the sending thread's registered IPC
buffer. There is no message struct in the ABI and no pointer to one.
`kernel/include/iris/ipc_msg.h` is the layout; `docs/ipc.md` is the prose.

## Error model

- every syscall returns a signed long in the architectural ABI sense
- success is non-negative
- failure is a negative `iris_error_t`

Kernel implementation note: the dispatcher still moves return values through
`uint64_t` internally. That does not change the external contract.

## Memory budgets

Every allocation names the Untyped it is carved from. `Untyped_Retype` names
its budget by construction, and `Boot_InitrdFrame` takes one as an argument
(0 = the caller's own). `SYS_VMO_CREATE` and `SYS_INITRD_VMO` were the other
two and went with the VMO; `SYS_PROCESS_CREATE` went with the process object
(Stage 7-proc).

Everything carved from an Untyped is a **child** of it, so `Untyped_Reset`
refuses while it lives and reclaims the whole region once it does not. A bump
allocator does not rewind: reclamation is by RESET, which is why spawners
recycle budgets rather than sizing them for a whole run.

## Composed objects

The kernel does not create an address space, a paging level or a CSpace. A
holder retypes each from its own Untyped and passes it in.

| Method | Arguments | What the caller supplies |
|---|---|---|
| `TCB_Configure` | tcb, cspace, vspace | the CSpace root and the address space the thread runs in, both retyped by the caller. This is `seL4_TCB_Configure`: since Stage 7-proc there is no identity check against a third object, and threads sharing a CSpace and a VSpace is what a "process" IS |
| `PageTable_Map` | pt, vspace, vaddr | a `IRIS_KOBJ_PAGE_TABLE` to fill the first level missing for `vaddr` |

A map whose walk is incomplete answers `IRIS_ERR_MISSING_TABLE` and names
nothing else — the holder supplies the level and retries. A task that maps
therefore needs a budget to retype levels from, which is why
`svc_load_minted_ws` takes the slot to mint the child its own.

## Current architectural reading

- file I/O is not a kernel surface
- service discovery is not a kernel namespace surface
- ELF loading is not a kernel spawn surface
- **process construction is not a syscall at all**: a ring-3 loader retypes a
  VSpace, a root CNode and a TCB from a budget it holds, configures the thread
  with the first two, writes its registers and resumes it
- accounting is not a call about a process either — a budget answers for
  itself (`Untyped_Info` / `Untyped_Query`)
- hardware access is capability-gated, and the capability is the argument
- **naming a method requires naming the object**, which is the whole of A-32

## Top hardening-risk families

1. endpoint send/recv/call/reply paths, including staged capability transfer
2. frame map/unmap and the page-table walk
3. retype, configure and resume — the thread-construction path
4. notification wait paths
5. `Boot_KlogDrain`, `Untyped_Query` and the other user-buffer write-back paths

## Canonical sources

- `kernel/include/iris/invoke.h` — the label space
- `kernel/include/iris/ipc_msg.h` — the message ABI
- `kernel/include/iris/syscall.h` — the three numbers, and the retirement notes
- `kernel/core/syscall/syscall_invoke.c` — what each label dispatches to
- `kernel/core/syscall/syscall_dispatch.c` — what the numbered door still does
- `kernel/include/iris/endpoint_proto.h` — endpoint/CPtr service ABI
- `kernel/include/iris/vfs_ep_proto.h`, `kbd_ep_proto.h`, `console_ep_proto.h`
  — the service protocols
