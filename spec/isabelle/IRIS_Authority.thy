(*  Title:      IRIS_Authority.thy
    Author:     IRIS, Stage V phase 3

    The ABSTRACT SPECIFICATION of IRIS's authority model.

    ── Status, stated first because it is the most important line here ──────

    **UNCHECKED.**  No Isabelle is installed on the machine this was written
    on, so not one lemma below has been through a prover.  They are theorem
    STATEMENTS, and a statement nobody has checked is a claim, not a proof.
    Anybody reading this should treat every `lemma` as an obligation and not
    as a result.

    What makes it worth writing anyway is that the same model — the same
    state, the same operations, the same invariants — exists as
    `spec/authority_model.py`, which IS validated: it is driven through the
    real `kcnode.c`, `cspace.c` and `syscall_cspace.c` over thousands of
    random traces and agrees with them on the resulting state and on every
    return code.  So this file is not a guess about what the kernel does.  It
    is the same specification in the language a refinement proof needs, and
    the Python one is its executable shadow.

    ── What this is FOR ────────────────────────────────────────────────────

    Phase 4 of the plan is `abstract ⊑ executable ⊑ C`.  This is the first
    of those three, and it is the one that has to exist before the phrase has
    a meaning.  The 37 invariants of `iris-sel4-purity-charter.md` are stated
    here as lemmas over this state, which is the difference between an
    invariant that is reviewed and an invariant that is proved.

    ── What it does NOT cover ──────────────────────────────────────────────

    The CSpace and the derivation tree, which is what phases 1 and 2 have
    validated.  IPC (I1-I7), scheduling (S1-S6) and memory (M1-M5) are stated
    as obligations at the end with no definitions behind them yet: they need
    the executable model to reach those operations first, and it has reached
    the CSpace door and no further.  An invariant stated over a state that
    does not model it would be decoration.
*)

theory IRIS_Authority
  imports Main
begin

section \<open>The state\<close>

text \<open>
  An object identity, a slot address, and the seven rights IRIS has.  seL4 has
  four and does not treat copyability as a right at all; ledger D-3 records
  the divergence, and `spec/rights_model.py` proves this algebra exhaustively
  over all 32896 points of its input space.
\<close>

type_synonym oid  = nat
type_synonym sid  = nat
type_synonym rights = nat        \<comment> \<open>a 7-bit set, as a bitmask\<close>
type_synonym badge  = nat

definition R_READ      :: rights where "R_READ      = 1"
definition R_WRITE     :: rights where "R_WRITE     = 2"
definition R_DUPLICATE :: rights where "R_DUPLICATE = 4"
definition R_TRANSFER  :: rights where "R_TRANSFER  = 8"
definition R_WAIT      :: rights where "R_WAIT      = 16"
definition R_ROUTE     :: rights where "R_ROUTE     = 32"
definition R_MANAGE    :: rights where "R_MANAGE    = 64"

datatype otype = Endpoint | Notification | CNode | Untyped | Reply | SchedCtx

record obj =
  o_type :: otype

text \<open>
  A slot is a capability or it is empty.  The derivation tree is carried IN
  the slots — a parent link and an ordered child list — which is what the
  kernel's MDB is: `mdb_parent`, `mdb_first_child`, the sibling ring.
\<close>

record slot =
  s_obj      :: "oid option"
  s_rights   :: rights
  s_badge    :: badge
  s_parent   :: "sid option"
  s_children :: "sid list"

record cspace =
  slots :: "sid \<Rightarrow> slot"
  objs  :: "oid \<Rightarrow> obj option"
  dom_s :: "sid set"          \<comment> \<open>the slots that exist\<close>

definition occupied :: "cspace \<Rightarrow> sid \<Rightarrow> bool" where
  "occupied C i \<longleftrightarrow> i \<in> dom_s C \<and> s_obj (slots C i) \<noteq> None"

definition named :: "cspace \<Rightarrow> oid \<Rightarrow> bool" where
  "named C x \<longleftrightarrow> (\<exists>i \<in> dom_s C. s_obj (slots C i) = Some x)"


section \<open>The rights algebra\<close>

text \<open>
  Proved by exhaustion in `spec/rights_model.py` over every pair; stated here
  so the derivation lemmas can be about something.  Note \<open>rights_check\<close> at the
  EMPTY requirement: the set-theoretic answer is True and the kernel returns
  False, deliberately, so that a caller which forgot to name a right fails
  closed rather than passing every check.
\<close>

text \<open>
  `rights_reduce` here is the SAME_RIGHTS-FREE branch of the kernel's function,
  and that restriction is load-bearing rather than cosmetic.

  `kcnode`'s `rights_reduce` is two branches, not one: a request carrying
  `RIGHT_SAME_RIGHTS` (bit 31) COPIES the base with that sentinel bit cleared,
  and anything else intersects.  `rights` is `nat` used as a 7-bit set in this
  theory, so the sentinel is outside the type and only the intersecting branch
  is modelled.

  Why it is written down instead of left implicit: the next lemma is
  commutativity, which is TRUE of intersection and **FALSE of the kernel's
  function**.  `spec/prove.py` proves these over the whole 32-bit word, where
  the sentinel lives, and guards its commutativity row with
  `r AND SAME_RIGHTS = 0` for exactly this reason.  Decided by z3 against the
  real two-branch definition, the general claim is refuted with
  `b = 0xed2f9fff`, `r = 0x84506000`: `r` carries the sentinel, so
  `reduce b r = b AND NOT SAME` while `reduce r b = r AND b`.

  An unproved lemma is a bill somebody will pay later.  A lemma that is
  provable HERE and false of the kernel is worse, because paying it produces a
  certificate for a property IRIS does not have — and a certificate is the one
  artifact nobody re-reads.  So the scope is stated at the definition, where a
  reader meets it before the lemma.
\<close>

definition rights_reduce :: "rights \<Rightarrow> rights \<Rightarrow> rights" where
  "rights_reduce b r = (b AND r)"

definition rights_check :: "rights \<Rightarrow> rights \<Rightarrow> bool" where
  "rights_check have need \<longleftrightarrow> (need \<noteq> 0 \<and> (have AND need) = need)"

lemma rights_reduce_monotone: "rights_reduce b r AND b = rights_reduce b r"
  \<comment> \<open>A7: a derivation never amplifies.\<close>
  oops

lemma rights_reduce_idempotent:
  "rights_reduce (rights_reduce b r) r = rights_reduce b r"
  oops

lemma rights_reduce_commutative: "rights_reduce b r = rights_reduce r b"
  \<comment> \<open>TRUE of intersection, and so of this theory's restricted
      `rights_reduce`; FALSE of the kernel's two-branch function, which is why
      `spec/prove.py` guards its row with `r AND SAME_RIGHTS = 0`.  Do not
      restate it without that guard: see the note at the definition for the
      z3 counterexample.  SMT-discharged in the guarded form.\<close>
  oops

lemma rights_check_empty_fails: "\<not> rights_check h 0"
  by (simp add: rights_check_def)


section \<open>The operations\<close>

text \<open>
  Four, and they are the four the executable model drives: derive (mint),
  delete, revoke and retype.  Their ORDER of refusal is part of the
  specification and not an artefact of how the C was written — phase 1 found
  that out by comparing return codes and not only states.
\<close>

datatype err = Ok | InvalidArg | NotFound | AccessDenied | AlreadyExists
             | WrongType

text \<open>
  `can_derive` is the PRIMITIVE's order — `kcnode_slot_derive` — and IRIS has
  two.

  The refusal order below puts `src = dst` before occupancy, which is what the
  primitive does: it rejects a slot named twice before it reads either one.
  `SYS_CSPACE_MINT` is the other way round, because a syscall RESOLVES its
  arguments first, so the same arguments through the syscall answer NotFound
  where the primitive answers InvalidArg.

  Both are reachable from ring 3 — the primitive through the paths that take a
  CNode and an index directly — so neither order is an implementation detail,
  and this theory specifies ONE of them.  `spec/authority_model.py` carries
  both, in `mint` and in `syscall_mint`, and `spec-differential` disagreed with
  the kernel on each of them in turn while they were conflated: `mint 0 0 7 7`
  wants InvalidArg and `smint 13 13 2 7` wants NotFound, on the same empty
  slot.  The scope is written here so that proving the lemmas below is not
  mistaken for having specified both.
\<close>

definition can_derive :: "cspace \<Rightarrow> sid \<Rightarrow> sid \<Rightarrow> rights \<Rightarrow> badge \<Rightarrow> err" where
  "can_derive C src dst req bg =
     (if src = dst then InvalidArg
      else if \<not> occupied C src then NotFound
      else if \<not> rights_check (s_rights (slots C src)) R_DUPLICATE
           then AccessDenied
      else if rights_reduce (s_rights (slots C src)) req = 0 then InvalidArg
      else if bg \<noteq> 0 \<and> s_badge (slots C src) \<noteq> 0
              \<and> s_badge (slots C src) \<noteq> bg then AccessDenied
      else if bg \<noteq> 0 \<and> s_badge (slots C src) = 0
              \<and> o_type (the (objs C (the (s_obj (slots C src)))))
                  \<notin> {Endpoint, Notification} then InvalidArg
      else if occupied C dst then AlreadyExists
      else Ok)"

text \<open>
  The badge rule has THREE parts and the charter's A8 row records one.  A zero
  request inherits; a badged source asked for a different badge is refused;
  and a fresh badge may only be stamped on an IDENTITY type, because a badge
  is sender identity and a CNode has no sender.  Part three is in
  `mdb_badge_derive` and in no document — phase 1 found it on its fourth
  trace.
\<close>

definition badge_of_derive :: "cspace \<Rightarrow> sid \<Rightarrow> badge \<Rightarrow> badge" where
  "badge_of_derive C src bg =
     (if bg = 0 then s_badge (slots C src) else bg)"


text \<open>
  ── The operations, DEFINED ─────────────────────────────────────────────────

  They were not, and five lemmas below invoked them.

  `derive`, `delete`, `revoke` and `retype` appeared only inside the obligation
  statements — `inv_A7 (derive C src dst req bg)` and four more — and nowhere
  as definitions.  Isabelle rejects an undefined constant in a statement, so
  this theory could not be loaded at all, and the `oops` markers did not hide
  that: a parse error happens before a proof is attempted.  The file documented
  the markers as a deliberate choice ("`oops` rather than `sorry`, so a later
  theory cannot build on them as if they held"), which reads as though the file
  had been processed.  It had not.  Found by counting uses against definitions
  while auditing the theory; it is the kind of thing a prover says in one
  second and a reader can miss for a long time.

  The definitions follow `spec/authority_model.py`, because that is the model
  `spec-differential` and `spec-exhaustive` validate against the real C — so
  the chain stays `this text == that model == the kernel` rather than acquiring
  a third reading of its own.
\<close>

abbreviation empty_slot :: slot where
  "empty_slot \<equiv> \<lparr> s_obj = None, s_rights = 0, s_badge = 0,
                   s_parent = None, s_children = [] \<rparr>"

definition derive :: "cspace \<Rightarrow> sid \<Rightarrow> sid \<Rightarrow> rights \<Rightarrow> badge \<Rightarrow> cspace" where
  \<comment> \<open>The destination becomes a CHILD of the source, carrying reduced rights
      and the badge the central rule chose.  The source's child list gains it,
      because `inv_links_symmetric` is what makes A9 and A10 mean anything.\<close>
  "derive C src dst req bg =
     (let s = slots C;
          kid = \<lparr> s_obj = s_obj (s src),
                  s_rights = rights_reduce (s_rights (s src)) req,
                  s_badge = badge_of_derive C src bg,
                  s_parent = Some src,
                  s_children = [] \<rparr>;
          par = (s src)\<lparr> s_children := s_children (s src) @ [dst] \<rparr>
      in C\<lparr> slots := ((s(src := par))(dst := kid)) \<rparr>)"

definition delete :: "cspace \<Rightarrow> sid \<Rightarrow> cspace" where
  \<comment> \<open>Delete REPARENTS: the slot's children become children of its parent, so
      a capability is never orphaned and the forest stays a forest.  A root's
      children become roots.  This is `mdb_detach_reparent`.\<close>
  "delete C i =
     (let s = slots C; p = s_parent (s i); ch = s_children (s i)
      in C\<lparr> slots :=
         (\<lambda>t. if t = i then empty_slot
              else if t \<in> set ch then (s t)\<lparr> s_parent := p \<rparr>
              else if Some t = p
                   then (s t)\<lparr> s_children := removeAll i (s_children (s t)) @ ch \<rparr>
              else s t) \<rparr>)"

definition desc :: "cspace \<Rightarrow> sid \<Rightarrow> sid set" where
  \<comment> \<open>The DESCENDANTS of a slot, as the transitive closure of "is a child of".
      Isabelle can say this and a decidable fragment cannot, which is exactly
      why `revoke_removes_exactly_the_subtree` is owed here and only unrolled
      to a bounded depth by `make spec-prove`.\<close>
  "desc C i = {j. (j, i) \<in> {(a, b). s_parent (slots C a) = Some b}\<^sup>+}"

definition revoke :: "cspace \<Rightarrow> sid \<Rightarrow> cspace" where
  \<comment> \<open>A10: the subtree goes and the ROOT stays — it keeps its capability and
      loses its children.\<close>
  "revoke C i =
     (let s = slots C
      in C\<lparr> slots :=
         (\<lambda>t. if t \<in> desc C i then empty_slot
              else if t = i then (s i)\<lparr> s_children := [] \<rparr>
              else s t) \<rparr>)"

definition can_retype :: "cspace \<Rightarrow> sid \<Rightarrow> sid \<Rightarrow> err" where
  \<comment> \<open>The guard `retype_preserves_O2` was missing, and without it that
      obligation is FALSE rather than unproved.

      `inv_O2` says every edge is a derivation or a CREATION FROM AN UNTYPED,
      so a retype that parents a fresh object under a slot holding an endpoint
      produces a state violating it — and the lemma as written had no premise
      saying the source holds an Untyped at all.  `authority_model.py`'s
      `retype` answers WRONG_TYPE there, and `syscall_untyped.c` is where that
      comes from; the theory simply never said it.

      Stated as its own guard rather than as an extra premise on the lemma,
      because the ORDER of refusals is part of this specification — the
      comment above `can_derive` says so — and an unnamed premise would record
      the condition without recording that the kernel reports it.\<close>
  "can_retype C ut dst =
     (if \<not> occupied C ut then NotFound
      else if o_type (the (objs C (the (s_obj (slots C ut))))) \<noteq> Untyped
           then WrongType
      else if occupied C dst then AlreadyExists
      else Ok)"

definition fresh_oid :: "cspace \<Rightarrow> oid" where
  "fresh_oid C = (SOME x. objs C x = None)"

definition retype :: "cspace \<Rightarrow> sid \<Rightarrow> otype \<Rightarrow> sid \<Rightarrow> cspace" where
  \<comment> \<open>A new object is BORN from the Untyped, and its capability is installed as
      a child of the Untyped's slot — which is what `inv_O2` reads as "the
      storage belongs to the Untyped that produced it", and what lets a reset
      be refused while children live.\<close>
  "retype C ut ty dst =
     (let s = slots C; x = fresh_oid C;
          kid = \<lparr> s_obj = Some x, s_rights = 0, s_badge = 0,
                  s_parent = Some ut, s_children = [] \<rparr>;
          par = (s ut)\<lparr> s_children := s_children (s ut) @ [dst] \<rparr>
      in C\<lparr> slots := ((s(ut := par))(dst := kid)),
            objs := (objs C)(x := Some \<lparr> o_type = ty \<rparr>) \<rparr>)"

section \<open>The invariants of the charter, as obligations\<close>

text \<open>
  Each is stated over the state above.  The ones with definitions behind them
  are A7-A10 and O1-O2; the rest are listed at the end as obligations the
  state does not yet model, which is stated rather than papered over.
\<close>

definition inv_A7 :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>Rights are only kept or reduced.  DERIVATION only: an object retyped
      from an Untyped is BORN with its rights, so the edge from an Untyped to
      its child is outside this invariant.  The first executable checker did
      not make that distinction and flagged the model the moment retype
      existed.\<close>
  "inv_A7 C \<longleftrightarrow>
     (\<forall>i \<in> dom_s C. \<forall>p. s_parent (slots C i) = Some p \<longrightarrow>
        occupied C i \<longrightarrow> occupied C p \<longrightarrow>
        s_obj (slots C i) = s_obj (slots C p) \<longrightarrow>
        (s_rights (slots C i) AND s_rights (slots C p)) = s_rights (slots C i))"

definition inv_A8 :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>A badged capability is never re-badged by a derivation.\<close>
  "inv_A8 C \<longleftrightarrow>
     (\<forall>i \<in> dom_s C. \<forall>p. s_parent (slots C i) = Some p \<longrightarrow>
        occupied C i \<longrightarrow> occupied C p \<longrightarrow>
        s_obj (slots C i) = s_obj (slots C p) \<longrightarrow>
        s_badge (slots C p) \<noteq> 0 \<longrightarrow>
        s_badge (slots C i) = s_badge (slots C p))"

definition inv_A9 :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>Every derived capability is traceable to an ancestor: the parent
      relation is acyclic, so the graph is a forest and every capability
      reaches a root.  seL4's BootInfo capabilities are roots too; the tree
      has roots by design and the invariant is that there are no CYCLES.\<close>
  "inv_A9 C \<longleftrightarrow> wf {(i, p). i \<in> dom_s C \<and> s_parent (slots C i) = Some p}"

definition inv_links_symmetric :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>Not in the charter, and it is what makes A9 and A10 mean anything: the
      tree is doubly linked and the two directions agree.\<close>
  "inv_links_symmetric C \<longleftrightarrow>
     (\<forall>i \<in> dom_s C. (\<forall>c \<in> set (s_children (slots C i)).
                        s_parent (slots C c) = Some i)
                  \<and> (\<forall>p. s_parent (slots C i) = Some p \<longrightarrow>
                          i \<in> set (s_children (slots C p))))"

definition inv_O2 :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>An object's storage belongs to the Untyped that produced it, which is
      represented by the capability being a CHILD of the Untyped's slot —
      the same fact the kernel uses to refuse a reset while children live.\<close>
  "inv_O2 C \<longleftrightarrow>
     (\<forall>i \<in> dom_s C. occupied C i \<longrightarrow>
        (\<forall>p. s_parent (slots C i) = Some p \<longrightarrow> occupied C p \<longrightarrow>
           (s_obj (slots C i) = s_obj (slots C p)
            \<or> o_type (the (objs C (the (s_obj (slots C p))))) = Untyped)))"

definition inv_liveness :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>Ledger D-7's TARGET, stated: an object exists exactly while a capability
      names it.  This is what reference counting only approximates, and the
      ledger records the approximation as the cause of three defects.  Here
      it is decided by the tree, which is what a verified kernel needs it to
      be, and what phase 2 must make true of the C.\<close>
  "inv_liveness C \<longleftrightarrow> (\<forall>x. objs C x \<noteq> None \<longleftrightarrow> named C x)"


section \<open>The theorems phase 4 owes\<close>

text \<open>
  Each of these is the obligation that an operation PRESERVES an invariant.
  None is proved IN ISABELLE.  They are written as `oops` rather than `sorry`
  on purpose: `sorry` would let a later theory build on them as if they held.

  All five now have a machine-checked discharge somewhere else, two of them
  only in a bounded or relative form that is stated exactly below.
  `spec/prove.py` states the same obligations over the same state — slots as
  arrays, rights as 32-bit words, the derivation tree as a parent map — and
  an SMT solver discharges them for EVERY state satisfying the invariant and
  every legal operation.  `make spec-prove` runs it.

  That is a real result and it is not this result, so the `oops` stays:

    * the SMT encoding is a SECOND formalisation, and if it disagrees with
      this one it proves something about a neighbouring system.  Isabelle
      proving THIS text is what removes that gap;
    * it discharges one-step preservation and bounded unrollings, not the
      closure properties: `revoke_removes_exactly_the_subtree` is proved to
      depth 4, and the unbounded claim needs an induction over the tree;
    * and `delete_preserves_A9` is proved only in its relative form, because
      a DEPTH-BOUNDED acyclicity invariant is not inductive under reparenting
      — the solver refuted the naive statement, which is the sharpest single
      argument in this file for proving the unbounded one.

  So: A7, A8 and O2 under derive, delete-with-reparent and retype are MACHINE
  CHECKED over unbounded state; A9 is machine-checked in its relative form
  (no NEW cycle) and A10 to a bounded depth; the inductions that lift those
  two to unbounded statements are what remains, and they are what Isabelle is
  for.  Marked per lemma below.
\<close>

lemma derive_preserves_A7:
  \<comment> \<open>SMT-discharged over symbolic state: `make spec-prove`, row
      "A7 derive preserves it".  All three of `mdb_badge_derive`'s outcomes
      are in the encoding's precondition, not just the inheriting one.\<close>
  "inv_A7 C \<Longrightarrow> can_derive C src dst req bg = Ok \<Longrightarrow> inv_A7 (derive C src dst req bg)"
  oops

lemma derive_preserves_A8:
  \<comment> \<open>SMT-discharged over symbolic state: `make spec-prove`, row
      "A8 derive preserves it".\<close>
  "inv_A8 C \<Longrightarrow> can_derive C src dst req bg = Ok \<Longrightarrow> inv_A8 (derive C src dst req bg)"
  oops

text \<open>
  `delete_preserves_A9` is checked as a DECISION by `make spec-theory`, which
  is worth separating from the bounded SMT row below.  Over a concrete state
  acyclicity is decidable — follow parents and see whether you return — so the
  lane checks the real property after every reparenting delete rather than the
  relative one the solver can state.  487 states per run, and a delete that
  parents a child to itself is caught within 120.

  The relative form stays the SMT row because that is what is provable over
  unbounded state; the induction that lifts it is still owed.
\<close>

lemma delete_preserves_A9:
  \<comment> \<open>Delete REPARENTS: a slot's children become children of its parent, so no
      capability loses its ancestry because an intermediate slot went.  That
      is the step where acyclicity is easiest to break.\<close>
  "inv_A9 C \<Longrightarrow> inv_A9 (delete C i)"
  \<comment> \<open>PARTLY discharged, and the attempt taught something worth writing down.
      `make spec-prove` proves the RELATIVE statement: no cycle of length <= 6
      before implies no cycle of length <= 3 after, i.e. reparenting
      introduces no cycle that was not already there.  The doubled premise is
      not slack — a reparented edge is two original edges, so a cycle of
      length L after lifts to one of length <= 2L before.

      The naive statement (same bound either side) is FALSE and the solver
      refuted it with a 6-cycle that reparenting SHORTENS to 5.  Nothing was
      created; something was pulled into view.  A bounded acyclicity invariant
      is not inductive under an operation that shortens paths, which is what
      reparenting is — so the step from "no new cycle" to "acyclic" is the
      induction, and it is this lemma's remaining content.\<close>
  oops

lemma revoke_removes_exactly_the_subtree:
  \<comment> \<open>A10: revoke removes the descendants and NOT the root.  The strongest of
      these, and the one the executable model checks on every trace.\<close>
  "revoke C i = C' \<Longrightarrow>
     (\<forall>d. descendant C i d \<longrightarrow> \<not> occupied C' d) \<and>
     (occupied C i \<longrightarrow> occupied C' i)"
  \<comment> \<open>Discharged to BOUNDED depth by `make spec-prove`: the root survives,
      every descendant within 4 links is cleared, and nothing that is not a
      descendant is touched.  The unrolling is what stands in for the
      transitive closure, which the decidable fragment cannot express; the
      unbounded claim is the induction and is owed.

      **And checked, unbounded in DEPTH, by `make spec-theory`.**  `desc` is a
      transitive closure, so a transliteration of it can be computed as the
      fixed point it is — and the lane checks this lemma's full content on
      every state a trace builds: every descendant cleared however deep, the
      root's capability kept, its children emptied, and no non-descendant
      touched.  487 states per run.  That is not the induction and does not
      replace it; it is the difference between "every descendant within four
      links" and "every descendant, on these states", and the second is
      strictly more than the first was.  Sabotage-tested: a revoke that spares
      one descendant is caught within 120 states.\<close>
  oops

lemma retype_preserves_O2:
  \<comment> \<open>The `can_retype` premise is NEW and the obligation was false without
      it: nothing said the source holds an Untyped, and `inv_O2` is exactly
      the claim that a non-derivation edge comes from one.\<close>
  "inv_O2 C \<Longrightarrow> can_retype C ut dst = Ok \<Longrightarrow> inv_O2 (retype C ut ty dst)"
  \<comment> \<open>SMT-discharged over symbolic state: `make spec-prove`, row
      "O2 retype preserves it".

      The first annotation here said O2 was untyped region arithmetic and so
      could not be modelled.  That is what O2 is ABOUT; it is not what this
      definition says.  `inv_O2` is structural — every MDB edge is a
      derivation or a creation from an Untyped — and `spec/authority_model.py`
      carries no base, no size and no cursor either.  Structural, therefore
      decidable, and the excuse was a misreading of the text directly above
      it.  Derive and delete-with-reparent preserve it too, which is what
      makes the tree readable as storage ownership at all.\<close>
  oops


section \<open>Obligations with no definitions yet\<close>

text \<open>
  Stated so the gap is visible rather than implied.  These are the charter
  invariants this state does not model, and each needs the executable model
  to reach the operations first — phases 1 and 2 have driven the CSpace and
  the Untyped, and nothing else.

    A1-A6      authority and the single namespace: partly structural (there
               is no second namespace to model), partly about the syscall
               layer.
    A10        stated above; the cross-PROCESS half needs more than one
               CSpace in the state.
    O1, O3-O6  object birth, execution and storage reuse: needs a lifecycle.
    I1-I7      IPC: needs endpoints, queues and message transfer.
    S1-S6      scheduling: needs threads, budgets and time.
    M1-M5      memory: needs address spaces and mappings.
    P1-P3      policy: not a property of a state at all — they are
               statements about WHERE code lives, and belong to review and to
               `check-purity`, not to a refinement proof.

  P1-P3 being unprovable here is not a gap in this file.  It is what the
  charter means when it says the kernel implements mechanism: a proof can
  show that the kernel does what the specification says, and cannot show that
  the specification contains no policy.  That judgement stays with the
  reviewer, and the charter is where it is written down.
\<close>

end
