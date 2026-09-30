# The abstract specification — Stage V phase 3

**Nothing here has been checked.** No Isabelle is installed on the machine
this was written on, so not one lemma in `IRIS_Authority.thy` has been
through a prover. They are theorem STATEMENTS. Treat every `lemma` as an
obligation, never as a result.

That is worth saying twice because the whole point of this stage is the
difference between a property that is reviewed and a property that is
proved, and a directory of unchecked theory files sitting next to a charter
full of MET rows is exactly how that difference gets blurred.

## Why it is written anyway

The same model — the same state, the same operations, the same invariants —
exists as `spec/authority_model.py`, and that one **is** validated: it is
driven through the real `kcnode.c`, `cspace.c`, `kobject.c` and
`syscall_cspace.c` over thousands of random traces and agrees with them on
the resulting state and on every return code. `spec/rights_model.py` goes
further and proves the rights algebra by exhaustion over all 32896 points of
its input space.

So this is not a guess about what the kernel does. It is the same
specification in the language phase 4's refinement needs, with the Python one
as its executable shadow — which is the order `seL4` used and the reason its
C could be written against something.

## How to check it

```bash
isabelle build -D spec/isabelle
```

Expect failures: the lemmas end in `oops`, which is deliberate. `sorry` would
let a later theory build on them as if they held; `oops` does not. Turning an
`oops` into a proof is the unit of work in phase 4.

## What it does not model, and why that is stated rather than implied

The CSpace and the derivation tree — which is exactly what phases 1 and 2
have validated. IPC, scheduling and memory are listed at the end of the
theory as obligations with no definitions behind them, because the executable
model has reached the CSpace door and no further, and an invariant stated
over a state that does not model it would be decoration.

`P1`–`P3` are a different case: they are statements about **where code
lives**, not about a state, and no refinement proof can discharge them. A
proof shows the kernel does what its specification says; it cannot show that
the specification contains no policy. That judgement stays with review and
with `make check-purity`.
