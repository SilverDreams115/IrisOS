# The abstract specification — Stage V phases 3 and 4

**Nothing in `IRIS_Authority.thy` has been through Isabelle.** No Isabelle is
installed on the machine this was written on, so every `lemma` in that file is
a theorem STATEMENT. Treat it as an obligation, never as a result.

**Some of those obligations are nonetheless machine-checked**, by a different
prover, against a second formalisation of the same model: `spec/prove.py`,
run by `make spec-prove`. That distinction is the whole subject of this file,
because "proved" and "proved by something other than the thing you are
reading" are different claims and only one of them is true here.

## What is actually proved, as of Stage V

| obligation | status | by what |
|---|---|---|
| the rights algebra (9 properties) | **PROVED** over all 2³² values | `spec/prove.py` (z3) |
| the same, over the 7-bit space, *and the kernel computes it* | **PROVED** by exhaustion | `make spec-rights` |
| A7 preserved by derive, delete-with-reparent, retype | **PROVED** over symbolic state | `spec/prove.py` |
| A8 preserved by the same three | **PROVED** over symbolic state | `spec/prove.py` |
| O2 preserved by the same three | **PROVED** over symbolic state | `spec/prove.py` |
| no child ever names an empty slot, or a non-slot | **PROVED** over symbolic state | `spec/prove.py` |
| A9 — reparenting introduces no NEW cycle | **PROVED** (relative: 6 before → 3 after) | `spec/prove.py` |
| A9 — the tree is acyclic, unbounded | **owed** | needs induction |
| A10 — revoke clears every descendant, spares the root, touches nothing else | **PROVED** to depth 4 | `spec/prove.py` |
| A10 — the same, unbounded | **owed** | needs induction |
| the proved predicates are the tested ones | **CHECKED** over 400 real-trace states | `make spec-bridge` |
| the C refines any of it | **owed**; tested, not proved | `make spec-differential` |

28 obligations, 28 discharged. "Over symbolic state" means: for every CSpace
satisfying the invariant and every legal operation on it, not for every CSpace
a generator built. That is the inductive step of a refinement proof, and it is
the thing neither testing nor exhaustion can give.

Where a row says *bounded* or *relative*, that is the claim — not a shorthand
for the unbounded one. The distinction is not pedantry here; see below.

## The three lanes, and why there are three

```bash
make spec-differential   # the model agrees with the real C over N traces
make spec-rights         # the rights algebra, exhaustive over its space
make spec-prove          # the invariants, over EVERY state
make spec-bridge         # the proved predicates ARE the tested ones
```

The fourth exists because the first three left a hole in the middle. `prove.py`
proved its predicates; `authority_model.py`'s checkers were validated against
the C; **nothing compared the two**, so they could have drifted with every lane
green — and the proved side is the one no hardware ever executes. The
predicates now live once in `spec/invariants.py`, proved symbolically by one
lane and evaluated concretely by the other on states real traces built. Writing
it turned up one genuine asymmetry between the two readings of A7, which
`bridge.py` now checks is unreachable instead of asserting it.

They are a ladder, and the top of it is still not the top of the ladder.
`spec-prove` proves the **model**. What connects that model to `kcnode.c` is
`spec-differential` — thousands of traces where the two agree on the state
and on every return code — and agreement over traces is evidence, not proof.
seL4's claim is *model proved AND C refines model proved*. IRIS's is *model
proved AND C matches model tested*. The gap between those two sentences is
phase 5, and naming it is cheaper than discovering later that nobody did.

## Why the solver's refusals are worth as much as its proofs

`spec/prove.py` carries three guards, because a prover that cannot fail is a
prover that agrees with anything:

* every theorem with premises first checks those premises are **satisfiable**.
  A theorem over contradictory premises is `unsat` for the wrong reason and
  prints exactly like a real proof. Such a row is reported `VACUOUS` and fails
  the lane.
* `unsat` and `unknown` are **not** conflated. The first version of that guard
  reported any non-`sat` premise check as `VACUOUS`, and promptly libelled A9
  — whose premises are satisfiable and merely hard for the default engine. A
  query that cannot be decided is asked again without model-based
  instantiation before anything is reported; only `unsat` is ever relied on
  for a proof, and `unsat` is sound under either configuration.
* the lane asserts that two claims which are FALSE about IRIS's algebra are
  still refutable.

**Four refutations so far, and all four were mine.** Three were the encoding's
domain — `victim = -1` (the no-parent sentinel used as a slot), a parent link
pointing at `-2` (because `wf` said parents are *occupied* and never said they
are *slots*), and an ancestor walk that strolled through `-1` into unconstrained
memory and reported a cycle. A sentinel is not a value, and every walk over a
partial map has to say where it stops.

The fourth was not an artifact and is the most interesting thing in this
directory. "Acyclic to depth 5 before ⟹ acyclic to depth 5 after" is **false**:
the solver produced a 6-cycle, which the premise admits, and reparenting
*shortens* it to 5, where the conclusion sees it. Nothing was created —
something was pulled into view. **A depth-bounded acyclicity invariant is not
inductive under an operation that shortens paths**, and reparenting is exactly
such an operation. That is why A9 is stated relatively, with a doubled premise
that is not slack but arithmetic: a reparented edge is two original edges, so a
cycle of length L after lifts to one of length ≤ 2L before.

It is also the sharpest available argument for finishing the Isabelle. The
bounded proof is genuinely informative and it is genuinely not the invariant.

## How to check the Isabelle itself

```bash
isabelle build -D spec/isabelle
```

Expect failures: the lemmas end in `oops`, deliberately. `sorry` would let a
later theory build on them as if they held; `oops` does not. Turning an `oops`
into a proof is the unit of work that remains, and an SMT discharge next to it
is a reason to believe the statement is true — not a substitute for proving
the statement that is written.

## What it does not model, and why that is stated rather than implied

The CSpace and the derivation tree — which is exactly what phases 1 and 2
validated. IPC, scheduling and memory are listed at the end of the theory as
obligations with no definitions behind them, because the executable model has
reached the CSpace door and no further, and an invariant stated over a state
that does not model it would be decoration.

`P1`–`P3` are a different case: they are statements about **where code
lives**, not about a state, and no refinement proof can discharge them. A
proof shows the kernel does what its specification says; it cannot show that
the specification contains no policy. That judgement stays with review and
with `make check-purity`.
