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
| the same, over the 7-bit space | **PROVED** by exhaustion, and the kernel computes it | `make spec-rights` |
| A7 preserved by derive | **PROVED** over symbolic state | `spec/prove.py` |
| A8 preserved by derive | **PROVED** over symbolic state | `spec/prove.py` |
| A7/A8 preserved by delete-with-reparent | **PROVED** over symbolic state | `spec/prove.py` |
| no child names an empty slot, ever | **PROVED** over symbolic state | `spec/prove.py` |
| A10 — revoke spares its root | **PROVED** (one step) | `spec/prove.py` |
| A10 — revoke reaches every descendant | **owed** | needs induction |
| A9 — acyclicity under delete | **owed** | needs induction |
| O2 — retype and untyped arithmetic | **owed**, not modelled | — |
| the C refines any of it | **owed**; tested, not proved | `make spec-differential` |

"Over symbolic state" means: for every CSpace satisfying the invariant and
every legal operation on it, not for every CSpace some generator built. That
is the inductive step of a refinement proof, and it is the thing neither
testing nor exhaustion can give.

## The three lanes, and why there are three

```bash
make spec-differential   # the model agrees with the real C over N traces
make spec-rights         # the rights algebra, exhaustive over its space
make spec-prove          # the invariants, over EVERY state
```

They are a ladder, and the top of it is still not the top of the ladder.
`spec-prove` proves the **model**. What connects that model to `kcnode.c` is
`spec-differential` — thousands of traces where the two agree on the state
and on every return code — and agreement over traces is evidence, not proof.
seL4's claim is *model proved AND C refines model proved*. IRIS's is *model
proved AND C matches model tested*. The gap between those two sentences is
phase 5, and naming it is cheaper than discovering later that nobody did.

## Why the solver's refusals are worth as much as its proofs

`spec/prove.py` carries two guards, because a prover that cannot fail is a
prover that agrees with anything:

* every theorem with premises first checks those premises are **satisfiable**.
  A theorem over contradictory premises is `unsat` for the wrong reason and
  prints exactly like a real proof. Such a row is reported `VACUOUS` and fails
  the lane.
* the lane asserts that two claims which are FALSE about IRIS's algebra are
  still refutable.

Both earned their place. The delete theorem came back **REFUTED** twice on
first run, with a counterexample naming `victim = -1` — which is the
encoding's sentinel for "no parent", not a slot. The solver had found a hole
in the model's domain, not in the kernel. It is fixed (`slot()`), and the
episode is why the table above is per-obligation rather than a count.

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
