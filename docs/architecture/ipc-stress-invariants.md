# A1.11 Deterministic IPC fuzz/stress invariants

Status: ACCEPTED — implemented in this phase.  Companion to
`a1-5-ipc-receive-slot.md` (mechanism + A1.10 staged-cap atomicity contract)
and `handle-table-freeze.md` (A1.7 accounting counters).  A1.11 adds no
feature: it is a **deterministic stress/fuzz layer** inside `iris_test`
whose only job is to break IPC/lifecycle/cap-transfer if a bug exists, and
to keep it broken-proof afterwards.  It found and locked one real kernel
bug (deferred-reap slot reuse — see "Bugs found" below).

## Harness

All of it lives in `services/iris_test/main.c` (T107–T112 section).

- **PRNG**: xorshift32, one `g_fz_seed` per test, seeded with a FIXED
  constant (`0xA111010 7/8/9`, `0xA1110110/111`).  The operation sequence is
  bit-identical on every run; "fuzz" here means *pseudo-random mixing*, not
  nondeterminism.  On failure the test prints
  `FZ <test> seed=<s> iter=<i>` plus a short `why`, so any failure is
  reproducible by construction.
- **Workers**: persistent command-driven threads (up to 2), each owning a
  control endpoint.  A command is one blocking `EP_SEND` on the ctl
  endpoint — the rendezvous itself is the synchronization, so the harness
  needs no fragile timing.  The only sleeps are the short
  "let the worker reach its blocking syscall" pauses already used by
  T019/T020/T103, plus bounded yield-polls (`fz_wait`, ≤4000 yields).
  Each worker leaves one process-held KTcb handle behind by design (Ph96);
  every test documents its exact live-handle delta (+N workers).
- **Slot budget**: CSpace direct slots cannot be deleted from userland (no
  root-CNode accessor by design), so delivered/occupied slots are allocated
  monotonically from a budgeted window (`FZ_SLOT_BASE 100` ..
  `FZ_SLOT_LIMIT 240`).  The allocator returns 0 when overdrawn and the
  test fails loudly ("slot budget").  Tests reuse *empty-after-failure*
  slots deliberately — the reuse is itself the "slot stayed empty" assert.
- **Counters**: every test snapshots the A1.7 `SYS_SCHED_INFO` extension
  before/after: live handles, self/global high-water, table max, slot
  deliveries, handle deliveries, TOCTOU fallbacks, reply caps,
  CSPACE_RESOLVE count.  Checks are:
  - **exact** live balance (`after == before + expected KTcbs`);
  - **exact** reply-cap balance where every rendezvous is test-driven
    (T108: flat; T109/T110: counted);
  - **directional** (`>=`) slot/handle delivery deltas (background services
    also move the global counters);
  - the T095 high-water rule `global_hwm * 4 <= max` (max = 256, historic
    hwm = 33 — the bound is unchanged, not invented).

### Reply-cap accounting note (T110)

`svcmgr` emits **one console log line per served LOOKUP_NAME** (OK and
NOT_FOUND alike; REGISTER/UNREGISTER do not log), and `console_ep_write`
is an `EP_CALL` — one extra KReply per served lookup.  T110's exact
reply-cap check counts `exp_reply + exp_log` for this reason.  The first
T110 run measured `delta 53 = 33 rendezvous + 20 lookups` and the term was
*accounted*, not slacked away.  If svcmgr logging placement changes, T110's
"reply count" will fail: update `exp_log` increments to match — do not
convert the check to an inequality.

## Invariants

Each test asserts an explicit subset of:

```text
I1  No authority without cap.
I2  No fallback after ACCESS_DENIED.
I3  No silent slot overwrite.
I4  Receive-slot occupied fails/degrades only per documented contract.
I5  Sender keeps its cap if there was no delivery commit.
I6  Receiver gains no authority if delivery fails.
I7  No staged-cap leak.
I8  No double release.
I9  Reply cap stays one-shot.
I10 A second reply fails without consuming the server's attached cap.
I11 Legacy handle delivery still works when slot = 0.
I12 Receive-slot delivery installs an invocable CPtr.
I13 Lookup NOT_FOUND installs no ghost cap.
I14 Endpoint close wakes waiters correctly.
I15 Process/thread death leaves no dead waiters.
I16 Live handle count returns to the expected baseline.
I17 High-water stays within the documented bound (T095 rule).
I18 Slot/handle delivery counters move in the expected direction.
```

## Test coverage

| Test | Seed / iters | Scenario | Invariants | Failure paths |
|---|---|---|---|---|
| T107 | 0xA1110107 / 48 | one endpoint + one worker; PRNG mix of EP_SEND / EP_NB_SEND / EP_RECV with notification AND endpoint caps into fresh, occupied, invalid and legacy slots; rights-degraded staging | I1-I8, I11, I12, I16-I18 | occupied slot (fail-fast, occupant object unchanged), out-of-range slot (INVALID_ARG, endpoint untouched), TRANSFER-less staging (ACCESS_DENIED, no fallback, receiver later gets clean cap-less msg), NB with no receiver |
| T108 | 0xA1110108 / 16 | fresh endpoint per round; blocking senders/callers with staged caps, declared-slot and legacy receivers; endpoint closed mid-flight; two-waiter rounds; post-stress health rendezvous | I5-I8, I14, I16, I17 (+I4) | close cancels staged send / call / recv; both waiters of one close; canceled declared slot stays empty and reusable; zero KReplies minted across the test |
| T109 | 0xA1110109 / 20 | worker EP_CALLs, main serves; every call gets a SECOND reply attempt; cap replies into declared fresh slot / legacy slot; occupied reply slot | I3-I7, I9, I10, I12, I16-I18 | second reply with attached cap (NOT_FOUND, server cap kept), occupied reply-slot fail-fast before any send (no message, no KReply, occupant same object), exact reply-cap balance |
| T110 | 0xA1110110 / 18 | svcmgr register / re-register / lookup(slot, legacy, NOT_FOUND) / occupied-slot lookup / unregister / full re-register cycles over three temp names with exactly-tracked state | I1, I3, I4, I11-I13, I16-I18 | re-register BUSY (rejected cap closed by svcmgr), NOT_FOUND with declared slot (same reusable slot stays empty), occupied fail-fast then legacy still works, unregister removes authority, pool reuse without ghosts |
| T111 | 0xA1110111 / 4+2 | six lifecycle_probe children: coverage-forced prefix (all four classes) + PRNG tail — rslot+notification, rslot+endpoint cap, kill-before-delivery, legacy slot 0 | I1, I5, I6, I11, I12, I15-I18 | child killed with declared slot (no dead waiter, sender cap survives), endpoint-cap landing + child-death CSpace release, legacy landing |
| T112 | fixed 24 cycles | spawn → natural exit → IMMEDIATE respawn churn (regression lock for the deferred-reap bug) | I15-I17 | spawn NO_MEMORY under churn, exit-code corruption, parent handle-book drift |

Coverage strategy in T111: pure `%4` draws over 6 rounds could miss a class
for some seed, so the first four rounds force one of each class and only the
tail is PRNG-chosen.  Deterministic AND fully covered — prefer this pattern
over cranking iterations when class coverage matters.

## Bugs found

### Deferred-reap slot reuse leaked exited processes (fixed)

- **Symptom**: `SYS_PROCESS_CREATE` → `IRIS_ERR_NO_MEMORY` once T111's six
  extra children ran; measurement showed spawn/**kill** cycles leak 0 pages
  while spawn/**exit** cycles leaked ~20-22 pages each, with the deferred
  reaper actually running for only ~2 of 10 exits.
- **Root cause**: a self-exiting task defers its reap (it still runs on its
  own kernel stack / CR3), parking as `TASK_DEAD` in the reap queue.  The
  task-slot allocators treated any `TASK_DEAD` slot as free; an immediate
  respawn recycled the slot, `task_reset_slot` wiped `t->process`, and the
  reaper's `state != TASK_DEAD` guard silently skipped the stale entry —
  leaking the child's KProcess, address space, sched_ctx ref and live
  count.  The kill path (`task_kill_external`) reaps inline and was immune.
- **Fix**: `task.awaiting_reap` flag set by `task_exit_current`; the slot
  allocators skip dead-but-unreaped slots; the reaper's `task_reset_slot`
  zeroes the flag, returning the slot to the pool.  No ABI change.
- **Locked by**: T112 (failed `spawn` on its second cycle pre-fix,
  deterministically at its suite position); T111 remains the pressure test
  that exposed it.

The A1.5/A1.10 IPC contracts themselves survived every stress unchanged —
no receive-slot, reply-cap or staged-atomicity adjustment was needed.

## Remaining gaps

- **Caller death during EP_CALL with a live KReply** is only covered
  indirectly (T074/T075 server-death paths, T108 close-cancel).  A worker
  cannot be killed cleanly from userland mid-call today (threads have no
  external kill; killing a whole helper *process* mid-call would need a
  second spawned caller process — candidate for a T113).
- **Reply-cap transfer of the KReply itself** (RIGHT_TRANSFER on reply
  handles) is untouched by the stress.
- **Multi-endpoint contention** (many endpoints, interleaved waiters) is
  only exercised pairwise; a table-pressure variant near `FZ_SLOT_LIMIT`
  and near handle-table max would sharpen I17.
- **SMP**: the whole suite (and the reap-queue reasoning) is single-CPU;
  every A1.11 test must be revisited when SMP lands (the reap queue has an
  explicit SMP TODO).
- svcmgr stress trusts the current "one log line per lookup" behavior (see
  accounting note) — intentional coupling, revisit if svcmgr logging moves.

## How to extend without flaking

1. **Fixed seed, printed on failure** — never seed from time/TSC.
2. **Rendezvous is the sync**: command handoff over a blocking endpoint;
   never "sleep long enough".  Short pauses are allowed only to let a
   worker *reach* its blocking syscall (T019 pattern), and results are
   collected with bounded yield-polls.
3. **Budget everything**: iterations, retry loops, CSpace slots.  A test
   that cannot fail loudly on budget exhaustion will fail mysteriously.
4. **Exact where you own the traffic, directional where you don't**: live
   handles and reply caps can be exact (count every rendezvous, including
   service-internal ones like svcmgr's log calls); global delivery counters
   only get `>=` bounds.
5. **Force coverage, then randomize** (T111 pattern) instead of raising
   iteration counts to chase coverage probabilistically.
6. **Document expected KTcb deltas** for worker threads (+1 each, Ph96).
7. New thresholds must cite data (T095: hwm 33 vs max 256) — do not invent
   margins.
