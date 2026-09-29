# The program contract

What `proc` gives a program, and what a program may assume.  Stage 10-run
step 2.  Written BEFORE the service, because everything binds to it: a library
compiled once finds its capabilities at fixed slots, and a contract that has to
be broken later breaks every binary built against it.

A **program** is not a service.  A service is an image in the kernel initrd,
spawned by `init` with a manifest written in `init`, holding exactly the
capabilities that manifest names.  A program is an ELF file on a filesystem,
spawned by path, whose authority its spawner chooses at launch — and which
carries a C runtime, so it needs things no service has ever needed: a heap, a
stack with arguments on it, and a thread pointer.

---

## 1. What the tree already does, recorded because nobody had

Two conventions existed and were held by nothing but habit.  They are written
here so that step 2 does not contradict them by accident, and so that the next
person does not have to rediscover them by grepping.

### The address space a task gets

`USER_PRIVATE_BASE` is `0x00_0000_8000_0000_00` and `USER_PRIVATE_SIZE` is one
full PML4 slot, 512 GiB.  Inside it, `kernel/include/iris/paging.h` fixes:

| offset from `USER_PRIVATE_BASE` | what |
|---|---|
| `+0x0010_0000` | `USER_BOOTINFO_BASE` — where the kernel maps the root task's BootInfo |
| `+0x0020_0000` | `USER_TEXT_BASE` — the floor of the loader's ASLR bias |
| `+0x5000_0000` | `USER_VMO_BASE` — the ceiling of that bias, and the floor of everything a task maps for itself |
| top − 32 KiB | `USER_STACK_BASE`..`USER_STACK_TOP`, with one unmapped guard page below |

So a loaded image is confined to `[TEXT_BASE, VMO_BASE)` — 1.25 GiB, randomised
per spawn from RDTSC — and everything a task maps afterwards goes at or above
`VMO_BASE`.  That is the whole of the layout, and it was implicit in two
constants and one function.

### The addresses services pick by hand

There is no allocator: every service names its mappings with constants.  A
survey found **47 of them**, all between `VMO_BASE` and `VMO_BASE + 0x8100_0000`
— and **four addresses with more than one owner**, three of those inside
`iris_test`, which is one process running many tests.  They do not collide
today because each test unmaps before the next runs, which is a property held
by nothing.

That is not a defect this contract fixes.  It is the reason this contract
exists: a program's layout is defined here, once, rather than discovered by the
next person who needs an address.

---

## 2. The initial CSpace

Slots 1..19 keep the meanings `iris/endpoint_proto.h` already gives them, so a
program and a service agree about what a slot number means.  A program is given
a SUBSET; a slot it was not given is empty and resolving it is `NOT_FOUND`,
which is the answer a program that was not granted something should get.

| slot | what | why a program has it |
|---|---|---|
| 2 | `IRIS_CPTR_VFS_EP` | to open files by path — including the ones its interpreter needs |
| 3 | `IRIS_CPTR_CONSOLE_EP` | standard output and standard error |
| 5 | `IRIS_CPTR_OWN_EP` | its own endpoint; a program that serves needs one, and one that does not may delete it |
| 12 | `IRIS_CPTR_OWN_UNTYPED` | its memory budget.  `brk` and `mmap` retype from this and nothing else, so a program's memory is bounded by what it was given |
| 13 | `IRIS_CPTR_OWN_REPLY` | to answer a call |
| 18 | `IRIS_CPTR_OWN_VSPACE` | mapping a frame names the address space; a program that could not name its own could not grow its own heap |
| 19 | `IRIS_CPTR_OWN_TCB` | its own thread, which is how it sets its thread pointer (`TCB_SetTLSBase`) |

**Slots 20..31 are RESERVED and left empty.**  `iris_test` uses them for
fixtures, and a program that is one day run under the test harness must not
find its own capabilities where the harness expects its own.

**Slots 32..63 are the program's own**, free from the first instruction.  This
mirrors what services do from 32 upward.

**Slots 64..127 are the OBJECT TABLE**, and they are the part of this contract
that makes dynamic linking honest.

### The object table

Each object occupies **two** slots: `OBJV + 2i` is its TEXT, `OBJV + 2i + 1`
is its DATA MASTER.  Sixty-four slots is therefore thirty-two objects, and
`AT_IRIS_OBJC` counts objects rather than slots.  Both are `RIGHT_READ` only:
the text because every other process running the library maps the same physical
frame, the data because a program is expected to COPY it into memory of its own
before writing — there is no copy-on-write here and a shared writable data
segment would be silent sharing between processes that believe they are
isolated.

A stock dynamic linker resolves `DT_NEEDED` names against `DT_RPATH`,
`LD_LIBRARY_PATH` and `/lib`.  That is "open any path it can name" arriving
through the loader rather than through the program, and charter §6 refused a
personality that did it — see ledger A-49.

Here the SPAWNER resolves the transitive object set **before the child exists**
and mints one capability per object into slots 64 upward.  The interpreter
reads a table it was given and can reach nothing else: there is no search, no
path, and no environment variable that changes the answer.  A program's library
set is fixed by whoever launched it, visibly, at launch — and every entry stays
a derivation child of the spawner's, so revoking one removes it from every
process that has it.

Sixty-four entries.  A program with more shared objects than that is refused at
spawn with a reason, rather than given a truncated table.

`auxv` carries how many are filled (`AT_IRIS_OBJC`) and what each one is
(`AT_IRIS_OBJV`, below).  A slot past the count is empty.

**Slots 128 and above are the program's own** again.

---

## 3. The initial address space

All offsets from `USER_PRIVATE_BASE`.  The first two rows are what the kernel
and the existing loader already do; the rest is this contract.

| region | range | notes |
|---|---|---|
| program image | `+0x0020_0000 .. +0x4FFF_FFFF` | ELF `PT_LOAD`, randomised by the loader between `USER_TEXT_BASE` and `USER_VMO_BASE`.  Unchanged |
| interpreter image | `+0x5000_0000 .. +0x5FFF_FFFF` | **its own region, on purpose.**  Two `ET_DYN` objects biased out of one range can overlap, and a loader that had to check would be a loader with a failure mode.  Disjoint ranges cannot collide at all |
| heap (`brk`) | `+0x6000_0000 .. +0x6FFF_FFFF` | 256 MiB, grows up from the base.  Bounded by the program's Untyped long before it reaches the ceiling |
| `mmap` | `+0x7000_0000 .. STACK_BASE` | grows up.  Shared objects the interpreter maps land here, and so does anything `malloc` takes for a large allocation |
| stack | top − 32 KiB .. top | existing, with its guard page.  **32 KiB is small for a C program** — musl's own default thread stack is four times that — and growing it is a change to this table, not a decision a program makes |

A program may assume these ranges are its own and that nothing else is mapped
in them at start.  It may **not** assume an address inside one: the image and
the interpreter are randomised, and `mmap` returns what it returns.

---

## 4. The initial stack

System V AMD64 process initialisation, which is what a C runtime's entry
expects to find and therefore not a place to be creative.  At entry, `%rsp`
points at:

```
  rsp →  argc                     (8 bytes)
         argv[0] .. argv[argc-1]  (pointers)
         NULL
         envp[0] .. envp[n-1]     (pointers)
         NULL
         auxv[0].a_type, .a_val   (pairs)
         ...
         AT_NULL, 0
         (the strings argv and envp point at)
```

### The auxiliary vector

The standard entries a dynamic runtime cannot start without:

| entry | value |
|---|---|
| `AT_PHDR` | the program's own program headers, at their mapped address |
| `AT_PHENT`, `AT_PHNUM` | their size and count |
| `AT_BASE` | where the interpreter was loaded |
| `AT_ENTRY` | the program's entry point (the interpreter jumps here when it is done) |
| `AT_PAGESZ` | 4096 |
| `AT_RANDOM` | sixteen bytes a runtime seeds its stack guard from |

And three of this system's own, because a program here needs to be told things
a Linux program reads out of `/proc` or a search path:

| entry | value |
|---|---|
| `AT_IRIS_OBJC` | how many object-table slots are filled |
| `AT_IRIS_OBJV` | the first object-table slot (64; carried rather than assumed, so the table can move without rebuilding every binary) |
| `AT_IRIS_UNTYPED` | the slot holding the program's budget (12; same reason) |

Their numbers are allocated from the private `AT_` range and are defined in
`iris/program_abi.h` beside this document.

---

## 4b. What `proc` fills in TODAY, and what is a zero on purpose

Written because a contract that describes the finished system and a service
that implements half of it disagree silently, and the half that is missing is
invisible to a program that reads a field and gets a plausible number.

| the contract says | today | when it changes |
|---|---|---|
| slots 2, 3 | filled: the VFS and the console, `RIGHT_WRITE` | — |
| slots 12, 18, 19 | filled by the loader, once a budget slot is named | — |
| slot 5 (own endpoint) | filled **when the spawner hands one over**: `PROC_OP_SPAWN` takes a capability and mints it here.  A spawner that wants to hear from what it starts gives it a channel; one that does not leaves the slot empty.  `proc` never creates an endpoint per child out of memory nothing reclaims | — |
| slot 13 (own reply) | **EMPTY.**  A program that CALLS needs no reply object — only a server does — and one that means to serve retypes one from its own budget | if a program ever needs one before it can allocate |
| slots 64..127, `AT_IRIS_OBJC` | filled, one object per PAIR of slots: text at `OBJV + 2i`, data master at `OBJV + 2i + 1`, both `RIGHT_READ` only.  `proc` resolves the set from `objreg` BEFORE the child exists.  Today a spawn names at most one object; the table's shape is what step 5 fills | — |
| `AT_BASE` | **0.**  There is no interpreter yet, and an ELF with a `PT_INTERP` is REFUSED with `PROC_STEP_INTERP` rather than started unrelocated | step 5 |
| `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_ENTRY` | filled, from the file's own account of where its headers are: `PT_PHDR` when it has one, else the `PT_LOAD` whose file range contains `e_phoff`.  A program whose headers are in NEITHER is refused, because a wrong `AT_PHDR` sends a runtime walking arbitrary memory | — |
| `AT_RANDOM` | filled, pointing at sixteen bytes at the BOTTOM of the program's own stack — the one place in the region the stack builder provably never reaches | — |

`AT_ENTRY` and the bias are obtained by ASKING: the loader randomises the image
and the thread's entry register is the only thing that knows where it landed,
so `proc` reads it back with `TCB_ReadRegs` and computes `bias = rip - e_entry`.
That is exact, and it costs one syscall instead of another return value out of
the loader.

### A program links with its own script

`services/link_program.ld`, not `link_service.ld`.  One line differs — the
image starts at `SIZEOF_HEADERS` rather than at 0 — and that line is what puts
the ELF header and the program header table inside the first `PT_LOAD`, which
is what makes `AT_PHDR` a mapped address rather than a file offset.  A service
never needed it because nothing ever asks a service where its own headers are.

---

## 5. What a program may NOT assume

- **That it can `fork`.**  It cannot; charter §6 registers the absence as
  permanent.  `posix_spawn` is the whole of it.
- **That it can open a path it was not given.**  `IRIS_CPTR_VFS_EP` reaches
  what the VFS will grant it, and the VFS answers to whoever holds it.  A
  program with no VFS capability has no filesystem, and that is a thing a
  spawner may choose.
- **That a capability it was given cannot be taken away.**  Every one is a
  derivation child of the spawner's; revocation reaches it.  A program that
  caches a mapping and assumes it stays is a program that will fault.
- **That an unimplemented call will do something plausible.**  It answers
  `ENOSYS`, loudly and for ever.

---

## 6. What this contract reserves, and why

Reserved ranges are cheap now and expensive later.

- **CSpace 20..31** — the test harness's fixtures.
- **CSpace 64..127** — the object table, sized for 64 objects, so adding a
  shared library never moves a program's own slots.
- **`AT_IRIS_*`** — the two indirections (`OBJV`, `UNTYPED`) exist so the
  table and the budget can move without rebuilding a binary.  A program that
  hardcodes 64 works today and breaks later; one that reads `AT_IRIS_OBJV`
  does not.
- **The gap between the heap ceiling and the `mmap` floor** — the heap's
  256 MiB is a region, not a limit somebody has to enforce; what actually
  bounds it is the Untyped, and the region only has to be large enough that
  the two allocators never argue.
