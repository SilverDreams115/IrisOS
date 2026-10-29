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

(* The bitwise `AND` used throughout the rights algebra is infix notation that
 * Isabelle keeps in a BUNDLE, not in `Main`'s default syntax — since
 * Isabelle2023 it has to be asked for.  Without this line the theory does not
 * parse at all: `rights_reduce b r = (b AND r)` fails with "Clash of types
 * ⇒ and nat", because `AND` is read as a standalone constant being applied to
 * `b`.  It is the first thing a prover says about this file, and nobody had
 * heard it. *)
unbundle bit_operations_syntax

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
  by (simp add: rights_reduce_def ac_simps)

lemma rights_reduce_idempotent:
  "rights_reduce (rights_reduce b r) r = rights_reduce b r"
  by (simp add: rights_reduce_def and.assoc)

lemma rights_reduce_commutative: "rights_reduce b r = rights_reduce r b"
  \<comment> \<open>TRUE of intersection, and so of this theory's restricted
      `rights_reduce`; FALSE of the kernel's two-branch function, which is why
      `spec/prove.py` guards its row with `r AND SAME_RIGHTS = 0`.  Do not
      restate it without that guard: see the note at the definition for the
      z3 counterexample.  SMT-discharged in the guarded form.\<close>
  by (simp add: rights_reduce_def ac_simps)

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
      to a bounded depth by `make spec-prove`.

      Restricted to `dom_s C`, which is not decoration: `inv_A9` is the
      well-foundedness of exactly that relation, so a `desc` built from a
      WIDER one would not inherit acyclicity and the root-survives half of
      A10 would be unprovable.  `spec/theory_check.py` computes it over
      `dom_s` too.\<close>
  "desc C i =
     {j. (j, i) \<in> {(a, b). a \<in> dom_s C \<and> s_parent (slots C a) = Some b}\<^sup>+}"

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

definition inv_wf :: "cspace \<Rightarrow> bool" where
  \<comment> \<open>WELL-FORMEDNESS, which this theory did not have and every preservation
      lemma needs: an occupied slot's parent link is absent or names a slot
      that is itself occupied.

      Found by Isabelle, not by reading.  The first attempt at
      `derive_preserves_A7` left exactly one goal — the case where `src`'s
      parent link points at `dst`, the slot the derive is about to FILL.  In
      `C` that parent was empty, so `inv_A7` said nothing about the edge;
      afterwards it is occupied with REDUCED rights, and A7 would demand
      `s_rights src AND rights_reduce (s_rights src) req = s_rights src`,
      which is false whenever the request reduces anything.  So the obligation
      is false without this premise, and the `oops` was hiding a missing
      invariant rather than a missing proof.

      `spec/invariants.py` has carried it since phase 4's first link, as `wf`,
      and `spec/prove.py` supplies it as a premise to every theorem.  The
      abstract side simply never stated it — which is the drift those two
      files exist to prevent, found in the one direction they could not check
      while this text would not load.\<close>
  "inv_wf C \<longleftrightarrow>
     (\<forall>i \<in> dom_s C. \<forall>p. s_parent (slots C i) = Some p \<longrightarrow>
        occupied C i \<longrightarrow> (p \<in> dom_s C \<and> occupied C p))"

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


text \<open>
  ── What `can_derive = Ok` tells us, and what `derive` leaves alone ─────────

  Extracted as named facts rather than unfolded inside each obligation.  The
  first attempt unfolded `can_derive_def` into the goal and ran `clarsimp` with
  `split: if_splits`: eight nested conditions times a record update at two
  slots, and the build had not finished after ten minutes.  A refusal chain is
  a bad thing to put in front of a simplifier and a fine thing to read facts
  off once.
\<close>

lemma can_derive_src_dst: "can_derive C src dst req bg = Ok \<Longrightarrow> src \<noteq> dst"
  by (simp add: can_derive_def split: if_splits)

lemma can_derive_src_occupied:
  "can_derive C src dst req bg = Ok \<Longrightarrow> occupied C src"
  by (simp add: can_derive_def split: if_splits)

lemma can_derive_dst_empty:
  "can_derive C src dst req bg = Ok \<Longrightarrow> \<not> occupied C dst"
  by (simp add: can_derive_def split: if_splits)

lemma can_derive_badge:
  \<comment> \<open>The badge rule's second part, as a fact: a badged source asked for a
      DIFFERENT badge is refused, so `Ok` means the request either inherits or
      matches.  That is what makes A8 survive a derive.\<close>
  "can_derive C src dst req bg = Ok \<Longrightarrow>
   bg = 0 \<or> s_badge (slots C src) = 0 \<or> s_badge (slots C src) = bg"
  by (simp add: can_derive_def split: if_splits)

lemma derive_obj_dst:
  "src \<noteq> dst \<Longrightarrow>
   s_obj (slots (derive C src dst req bg) dst) = s_obj (slots C src)"
  by (simp add: derive_def Let_def)

lemma derive_rights_dst:
  "src \<noteq> dst \<Longrightarrow>
   s_rights (slots (derive C src dst req bg) dst)
     = rights_reduce (s_rights (slots C src)) req"
  by (simp add: derive_def Let_def)

lemma derive_parent_dst:
  "src \<noteq> dst \<Longrightarrow>
   s_parent (slots (derive C src dst req bg) dst) = Some src"
  by (simp add: derive_def Let_def)

lemma derive_badge_dst:
  "src \<noteq> dst \<Longrightarrow>
   s_badge (slots (derive C src dst req bg) dst) = badge_of_derive C src bg"
  by (simp add: derive_def Let_def)

lemma derive_other:
  "t \<noteq> src \<Longrightarrow> t \<noteq> dst \<Longrightarrow>
   slots (derive C src dst req bg) t = slots C t"
  by (simp add: derive_def Let_def)

lemma derive_src_fields:
  "src \<noteq> dst \<Longrightarrow>
   s_obj (slots (derive C src dst req bg) src) = s_obj (slots C src)
   \<and> s_rights (slots (derive C src dst req bg) src) = s_rights (slots C src)
   \<and> s_badge (slots (derive C src dst req bg) src) = s_badge (slots C src)
   \<and> s_parent (slots (derive C src dst req bg) src) = s_parent (slots C src)"
  by (simp add: derive_def Let_def)

lemma derive_keeps:
  \<comment> \<open>Every slot but the destination keeps the three fields A7 and A8 read.
      Stated once so the obligations below do not have to case-split on
      `t = src` each time.\<close>
  "src \<noteq> dst \<Longrightarrow> t \<noteq> dst \<Longrightarrow>
   s_obj (slots (derive C src dst req bg) t) = s_obj (slots C t)
   \<and> s_rights (slots (derive C src dst req bg) t) = s_rights (slots C t)
   \<and> s_badge (slots (derive C src dst req bg) t) = s_badge (slots C t)
   \<and> s_parent (slots (derive C src dst req bg) t) = s_parent (slots C t)"
  by (cases "t = src") (simp_all add: derive_def Let_def)

lemma derive_dom: "dom_s (derive C src dst req bg) = dom_s C"
  by (simp add: derive_def Let_def)

text \<open>
  ── What `retype` touches, and the two assumptions it needs stated ──────────

  `fresh_oid` is `SOME x. objs C x = None`, which says nothing unless such an
  `x` EXISTS — so the freshness lemma carries that as a hypothesis rather than
  pretending the object space cannot be exhausted.  And `inv_liveness` is
  needed for a reason worth naming: without it a slot could name an object the
  `objs` map does not have, and then `o_type (the (objs C ...))` is `the None`
  — a term Isabelle is happy to write and nothing can say anything about.
\<close>

lemma can_retype_ut_occupied:
  "can_retype C ut dst = Ok \<Longrightarrow> occupied C ut"
  by (simp add: can_retype_def split: if_splits)

lemma can_retype_ut_untyped:
  "can_retype C ut dst = Ok \<Longrightarrow>
   o_type (the (objs C (the (s_obj (slots C ut))))) = Untyped"
  by (simp add: can_retype_def split: if_splits)

lemma can_retype_dst_empty:
  "can_retype C ut dst = Ok \<Longrightarrow> \<not> occupied C dst"
  by (simp add: can_retype_def split: if_splits)

lemma fresh_oid_fresh:
  "\<exists>x. objs C x = None \<Longrightarrow> objs C (fresh_oid C) = None"
  unfolding fresh_oid_def by (rule someI_ex)

lemma slot_oid_exists:
  \<comment> \<open>What `inv_liveness` gives at a slot: the object it names is IN the map.
      Needed twice below, and without it `o_type (the (objs C ...))` is
      `the None` — a term Isabelle writes happily and nothing can constrain.\<close>
  "inv_liveness C \<Longrightarrow> occupied C t \<Longrightarrow>
   objs C (the (s_obj (slots C t))) \<noteq> None"
  by (auto simp: inv_liveness_def occupied_def named_def)

lemma slot_oid_not_fresh:
  "inv_liveness C \<Longrightarrow> occupied C t \<Longrightarrow> objs C (fresh_oid C) = None
   \<Longrightarrow> the (s_obj (slots C t)) \<noteq> fresh_oid C"
  using slot_oid_exists by fastforce

lemma retype_dom: "dom_s (retype C ut ty dst) = dom_s C"
  by (simp add: retype_def Let_def)

lemma retype_obj_dst:
  "ut \<noteq> dst \<Longrightarrow>
   s_obj (slots (retype C ut ty dst) dst) = Some (fresh_oid C)"
  by (simp add: retype_def Let_def)

lemma retype_parent_dst:
  "ut \<noteq> dst \<Longrightarrow> s_parent (slots (retype C ut ty dst) dst) = Some ut"
  by (simp add: retype_def Let_def)

lemma retype_keeps:
  "ut \<noteq> dst \<Longrightarrow> t \<noteq> dst \<Longrightarrow>
   s_obj (slots (retype C ut ty dst) t) = s_obj (slots C t)
   \<and> s_parent (slots (retype C ut ty dst) t) = s_parent (slots C t)"
  by (cases "t = ut") (simp_all add: retype_def Let_def)

lemma retype_objs_old:
  "y \<noteq> fresh_oid C \<Longrightarrow> objs (retype C ut ty dst) y = objs C y"
  by (simp add: retype_def Let_def)

lemma delete_dom: "dom_s (delete C i) = dom_s C"
  by (simp add: delete_def Let_def)

lemma delete_parent_i: "s_parent (slots (delete C i) i) = None"
  by (simp add: delete_def Let_def)

lemma delete_parent_child:
  "t \<noteq> i \<Longrightarrow> t \<in> set (s_children (slots C i)) \<Longrightarrow>
   s_parent (slots (delete C i) t) = s_parent (slots C i)"
  by (simp add: delete_def Let_def)

lemma delete_parent_other:
  "t \<noteq> i \<Longrightarrow> t \<notin> set (s_children (slots C i)) \<Longrightarrow>
   s_parent (slots (delete C i) t) = s_parent (slots C t)"
  by (simp add: delete_def Let_def)

lemma delete_edges_in_trancl:
  \<comment> \<open>Every edge the delete leaves is one `C` already had, or a REPARENTED
      one — and a reparented edge is exactly two original edges, `(a, i)` then
      `(i, b)`.  So the new relation sits inside the transitive closure of the
      old, which is all well-foundedness needs.\<close>
  assumes ls: "inv_links_symmetric C" and idom: "i \<in> dom_s C"
  shows "{(a, b). a \<in> dom_s (delete C i)
                  \<and> s_parent (slots (delete C i) a) = Some b}
         \<subseteq> {(a, b). a \<in> dom_s C \<and> s_parent (slots C a) = Some b}\<^sup>+"
proof (rule subsetI)
  fix x
  assume xin: "x \<in> {(a, b). a \<in> dom_s (delete C i)
                             \<and> s_parent (slots (delete C i) a) = Some b}"
  obtain a b where xab: "x = (a, b)" by (cases x) auto
  from xin xab have ad: "a \<in> dom_s C"
    and e: "s_parent (slots (delete C i) a) = Some b"
    by (auto simp: delete_dom)
  let ?r = "{(a, b). a \<in> dom_s C \<and> s_parent (slots C a) = Some b}"
  have "(a, b) \<in> ?r\<^sup>+"
  proof (cases "a = i")
    case True
    with e show ?thesis by (simp add: delete_parent_i)
  next
    case ai_ne: False
    show ?thesis
    proof (cases "a \<in> set (s_children (slots C i))")
      case True
      have e1: "(a, i) \<in> ?r"
      proof -
        have "s_parent (slots C a) = Some i"
          using ls idom True unfolding inv_links_symmetric_def by blast
        thus ?thesis using ad by simp
      qed
      have e2: "(i, b) \<in> ?r"
      proof -
        have "s_parent (slots C i) = Some b"
          using e ai_ne True by (simp add: delete_parent_child)
        thus ?thesis using idom by simp
      qed
      from e1 e2 show ?thesis by (rule r_r_into_trancl)
    next
      case False
      have "s_parent (slots C a) = Some b"
        using e ai_ne False by (simp add: delete_parent_other)
      hence "(a, b) \<in> ?r" using ad by simp
      thus ?thesis by (rule r_into_trancl)
    qed
  qed
  thus "x \<in> ?r\<^sup>+" using xab by simp
qed

section \<open>Refinement: what the MDB is for, and where it is not needed\<close>

text \<open>
  Phase 4 asks for `abstract \<sqsubseteq> executable \<sqsubseteq> C`.  `spec-theory` now checks the
  first link by running both sides; `spec-exhaustive` checks the second to a
  bounded depth.  What neither says is what the derivation TREE is for.

  So: the AUTHORITY VIEW of a state is what each slot can do — the object it
  names, its rights, its badge — with the parent links and child lists thrown
  away.  An operation REFINES a view-level operation when its effect on the
  view is computable from the view alone; and then the tree is bookkeeping for
  that operation, not semantics.

  The result is a split, and the split is the point:

    * `derive` and `delete` refine single-slot view updates.  Everything the
      MDB carries is invisible to them — a derive's rights and badge come from
      the SOURCE's, and a delete's reparenting moves links and touches no
      slot's authority.
    * `revoke` does NOT.  Which slots it empties depends on the tree and on
      nothing in the view, which is exactly why the tree exists.  That is
      stated below as a counterexample rather than left as a remark, because
      "the MDB is necessary" is the kind of claim this project keeps and does
      not check.
\<close>

definition aobj :: "cspace \<Rightarrow> sid \<Rightarrow> oid option" where
  "aobj C = (\<lambda>i. s_obj (slots C i))"

definition arights :: "cspace \<Rightarrow> sid \<Rightarrow> rights" where
  "arights C = (\<lambda>i. s_rights (slots C i))"

definition abadge :: "cspace \<Rightarrow> sid \<Rightarrow> badge" where
  "abadge C = (\<lambda>i. s_badge (slots C i))"

lemma derive_refines:
  \<comment> \<open>A derive's whole effect on authority is one slot's worth, and every
      input to it is in the view: the source's object, rights and badge.\<close>
  assumes ok: "can_derive C src dst req bg = Ok"
  shows "aobj (derive C src dst req bg) = (aobj C)(dst := aobj C src)
         \<and> arights (derive C src dst req bg)
             = (arights C)(dst := rights_reduce (arights C src) req)
         \<and> abadge (derive C src dst req bg)
             = (abadge C)(dst := (if bg = 0 then abadge C src else bg))"
proof -
  have ne: "src \<noteq> dst" using ok by (rule can_derive_src_dst)
  show ?thesis
    using ne
    by (auto simp: aobj_def arights_def abadge_def derive_def badge_of_derive_def
                   Let_def fun_eq_iff)
qed

lemma delete_refines:
  \<comment> \<open>A delete clears one slot's authority and moves links.  No other slot's
      object, rights or badge changes — the reparenting is invisible here,
      which is what makes it safe for A7 and A8 and what
      `delete_preserves_A9` is separately about.\<close>
  "aobj (delete C i) = (aobj C)(i := None)
   \<and> arights (delete C i) = (arights C)(i := 0)
   \<and> abadge (delete C i) = (abadge C)(i := 0)"
  by (auto simp: aobj_def arights_def abadge_def delete_def Let_def fun_eq_iff)

text \<open>
  And the negative half, which is where the derivation tree earns its keep.

  Two states with the SAME authority view — every slot naming the same object
  with the same rights and the same badge — and the only difference is a parent
  link.  Revoking the same slot empties a capability in one and not in the
  other, so the effect of `revoke` on authority is not a function of authority.
  That is the whole reason IRIS carries an MDB, and it is stated here as a
  construction rather than as a remark.
\<close>

definition ex_objs :: "oid \<Rightarrow> obj option" where
  "ex_objs = (\<lambda>x. if x = 7 then Some \<lparr> o_type = Endpoint \<rparr> else None)"

definition ex_linked :: cspace where
  \<comment> \<open>Slot 1 is a CHILD of slot 0.\<close>
  "ex_linked =
     \<lparr> slots = (\<lambda>t. if t = 0
                     then \<lparr> s_obj = Some 7, s_rights = 1, s_badge = 0,
                             s_parent = None, s_children = [1] \<rparr>
                     else if t = 1
                     then \<lparr> s_obj = Some 7, s_rights = 1, s_badge = 0,
                             s_parent = Some 0, s_children = [] \<rparr>
                     else empty_slot),
       objs = ex_objs, dom_s = {0, 1} \<rparr>"

definition ex_loose :: cspace where
  \<comment> \<open>The same authority, and slot 1 is a ROOT.\<close>
  "ex_loose =
     \<lparr> slots = (\<lambda>t. if t = 0
                     then \<lparr> s_obj = Some 7, s_rights = 1, s_badge = 0,
                             s_parent = None, s_children = [] \<rparr>
                     else if t = 1
                     then \<lparr> s_obj = Some 7, s_rights = 1, s_badge = 0,
                             s_parent = None, s_children = [] \<rparr>
                     else empty_slot),
       objs = ex_objs, dom_s = {0, 1} \<rparr>"

lemma ex_same_view:
  "aobj ex_linked = aobj ex_loose \<and> arights ex_linked = arights ex_loose
   \<and> abadge ex_linked = abadge ex_loose"
  by (auto simp: aobj_def arights_def abadge_def ex_linked_def ex_loose_def
                 fun_eq_iff)

lemma ex_linked_desc: "(1::sid) \<in> desc ex_linked 0"
  by (auto simp: desc_def ex_linked_def intro: r_into_trancl)

lemma ex_loose_desc: "(1::sid) \<notin> desc ex_loose 0"
proof -
  have e: "{(a, b). a \<in> dom_s ex_loose
                    \<and> s_parent (slots ex_loose a) = Some b} = {}"
    by (auto simp: ex_loose_def)
  show ?thesis by (simp add: desc_def e trancl_empty)
qed

lemma revoke_does_not_refine:
  \<comment> \<open>So no view-level operation can stand in for `revoke`: the tree is
      semantics for it and not bookkeeping.\<close>
  "aobj (revoke ex_linked 0) \<noteq> aobj (revoke ex_loose 0)"
proof -
  have "aobj (revoke ex_linked 0) 1 = None"
    using ex_linked_desc by (simp add: aobj_def revoke_def Let_def)
  moreover have "aobj (revoke ex_loose 0) 1 = Some 7"
    using ex_loose_desc by (simp add: aobj_def revoke_def Let_def ex_loose_def)
  ultimately show ?thesis by force
qed

section \<open>The theorems phase 4 owed — and they are proved\<close>

text \<open>
  Each of these is the obligation that an operation PRESERVES an invariant, and
  **all five are now proved in Isabelle**, together with the three of the
  rights algebra.  `isabelle build -D spec/isabelle` discharges the session
  with no `oops` and no `sorry` left, and this session rejects `sorry`
  outright — a diagnostic one failed the build, which is worth knowing: the
  text cannot cheat even temporarily.

  That paragraph replaces one which said "None is proved IN ISABELLE.  They are
  written as `oops` rather than `sorry` on purpose: `sorry` would let a later
  theory build on them as if they held."  The intent was right and the state it
  described had never been checked by a prover at all — the theory did not
  load, for six separate reasons, and every one of them was found by running it
  rather than by reading it:

    * `AND` is infix notation Isabelle keeps in a BUNDLE, so without
      `unbundle bit_operations_syntax` the rights algebra does not parse.  That
      is the FIRST thing the prover says about this file, and nobody had heard
      it.
    * `derive`, `delete`, `revoke` and `retype` were used in five statements
      and defined nowhere.
    * `retype_preserves_O2` was false for want of `can_retype`'s guard.
    * `descendant` was used in A10's statement and defined nowhere — and
      because it sits in a BOOLEAN position Isabelle read it as a free
      variable, so the lemma type-checked and claimed something about EVERY
      ternary predicate.  A free variable is how a formal text goes quietly
      wrong, where an undefined operation goes loudly wrong.
    * A10's statement was two claims where the model checks three: it never
      said that non-descendants are untouched, and "removes the descendants"
      without that is satisfied by removing everything.
    * And four premises were missing, each of which makes its obligation FALSE
      rather than merely unproved: `inv_wf` for both derive lemmas,
      `inv_liveness` for retype, `inv_links_symmetric` and `i \<in> dom_s C`
      for delete, and `inv_A9` for revoke's root-survives half.

  `inv_wf` deserves its own line.  `spec/invariants.py` has carried it since
  phase 4's first link and `spec/prove.py` supplies it to every theorem; the
  abstract side never stated it.  That is exactly the drift those two files
  exist to prevent, in the one direction they could not check while this text
  would not load.

  What the SMT side still contributes is not redundant: it proves the same
  obligations over 32-bit words rather than `nat` used as a 7-bit set, and it
  carries the rows about SLICES and bounded unrollings that have no counterpart
  here.  And `delete_preserves_A9` turned out to need no induction at all —
  the right statement is about the RELATION, and well-foundedness passes to a
  subset of a transitive closure.  The bounded form the solver could state
  needed its doubled premise for the same reason it could not finish: a closure
  is not in its fragment.
\<close>

lemma derive_preserves_A7:
  \<comment> \<open>SMT-discharged over symbolic state: `make spec-prove`, row
      "A7 derive preserves it".  All three of `mdb_badge_derive`'s outcomes
      are in the encoding's precondition, not just the inheriting one.\<close>
  "inv_wf C \<Longrightarrow> inv_A7 C \<Longrightarrow> can_derive C src dst req bg = Ok
   \<Longrightarrow> inv_A7 (derive C src dst req bg)"
proof -
  assume wf: "inv_wf C" and a7: "inv_A7 C"
     and ok: "can_derive C src dst req bg = Ok"
  let ?D = "derive C src dst req bg"
  have ne: "src \<noteq> dst" using ok by (rule can_derive_src_dst)
  have edst: "\<not> occupied C dst" using ok by (rule can_derive_dst_empty)
  show ?thesis
    unfolding inv_A7_def
  proof (intro ballI allI impI)
    fix i p
    assume par: "s_parent (slots ?D i) = Some p"
       and oi: "occupied ?D i"
       and op: "occupied ?D p"
       and same: "s_obj (slots ?D i) = s_obj (slots ?D p)"
    show "s_rights (slots ?D i) AND s_rights (slots ?D p)
            = s_rights (slots ?D i)"
    proof (cases "i = dst")
      case True
      \<comment> \<open>The new capability: its parent IS the source and its rights are the
          source's, reduced — which is `rights_reduce_monotone` and nothing
          else.\<close>
      with par ne have "p = src" by (simp add: derive_parent_dst)
      with True ne show ?thesis
        by (simp add: derive_rights_dst derive_keeps rights_reduce_monotone)
    next
      case False
      \<comment> \<open>Any other slot kept its fields, so the edge is one `C` already had
          — provided its parent is not the slot just filled.  That is where
          `inv_wf` is needed: without it `p` could be `dst`, which was EMPTY,
          so A7 said nothing about the edge in `C` and would now demand that
          the source's full rights be a subset of its own reduction.\<close>
      have pd: "p \<noteq> dst"
      proof
        assume "p = dst"
        moreover have "occupied C i"
          using oi False ne by (simp add: occupied_def derive_keeps derive_dom)
        moreover have "s_parent (slots C i) = Some p"
          using par False ne by (simp add: derive_keeps)
        ultimately show False
          using wf edst by (fastforce simp: inv_wf_def occupied_def)
      qed
      have "s_rights (slots C i) AND s_rights (slots C p) = s_rights (slots C i)"
      proof -
        have "occupied C i"
          using oi False ne by (simp add: occupied_def derive_keeps derive_dom)
        moreover have "occupied C p"
          using op pd ne by (simp add: occupied_def derive_keeps derive_dom)
        moreover have "s_parent (slots C i) = Some p"
          using par False ne by (simp add: derive_keeps)
        moreover have "s_obj (slots C i) = s_obj (slots C p)"
          using same False pd ne by (simp add: derive_keeps)
        ultimately show ?thesis
          using a7 by (simp add: inv_A7_def occupied_def)
      qed
      thus ?thesis using False pd ne by (simp add: derive_keeps)
    qed
  qed
qed

lemma derive_preserves_A8:
  \<comment> \<open>SMT-discharged over symbolic state: `make spec-prove`, row
      "A8 derive preserves it".\<close>
  "inv_wf C \<Longrightarrow> inv_A8 C \<Longrightarrow> can_derive C src dst req bg = Ok
   \<Longrightarrow> inv_A8 (derive C src dst req bg)"
proof -
  assume wf: "inv_wf C" and a8: "inv_A8 C"
     and ok: "can_derive C src dst req bg = Ok"
  let ?D = "derive C src dst req bg"
  have ne: "src \<noteq> dst" using ok by (rule can_derive_src_dst)
  have edst: "\<not> occupied C dst" using ok by (rule can_derive_dst_empty)
  have bok: "bg = 0 \<or> s_badge (slots C src) = 0 \<or> s_badge (slots C src) = bg"
    using ok by (rule can_derive_badge)
  show ?thesis
    unfolding inv_A8_def
  proof (intro ballI allI impI)
    fix i p
    assume par: "s_parent (slots ?D i) = Some p"
       and oi: "occupied ?D i"
       and op: "occupied ?D p"
       and same: "s_obj (slots ?D i) = s_obj (slots ?D p)"
       and bp: "s_badge (slots ?D p) \<noteq> 0"
    show "s_badge (slots ?D i) = s_badge (slots ?D p)"
    proof (cases "i = dst")
      case True
      \<comment> \<open>The new capability inherits when the request is 0 and otherwise
          matches, because `can_derive` refused everything else.\<close>
      with par ne have ps: "p = src" by (simp add: derive_parent_dst)
      from bp ps ne have "s_badge (slots C src) \<noteq> 0"
        by (simp add: derive_keeps)
      with bok have "badge_of_derive C src bg = s_badge (slots C src)"
        by (auto simp: badge_of_derive_def)
      thus ?thesis using True ps ne
        by (simp add: derive_badge_dst derive_keeps)
    next
      case False
      have pd: "p \<noteq> dst"
      proof
        assume "p = dst"
        moreover have "occupied C i"
          using oi False ne by (simp add: occupied_def derive_keeps derive_dom)
        moreover have "s_parent (slots C i) = Some p"
          using par False ne by (simp add: derive_keeps)
        ultimately show False
          using wf edst by (fastforce simp: inv_wf_def occupied_def)
      qed
      have "s_badge (slots C i) = s_badge (slots C p)"
      proof -
        have "occupied C i"
          using oi False ne by (simp add: occupied_def derive_keeps derive_dom)
        moreover have "occupied C p"
          using op pd ne by (simp add: occupied_def derive_keeps derive_dom)
        moreover have "s_parent (slots C i) = Some p"
          using par False ne by (simp add: derive_keeps)
        moreover have "s_obj (slots C i) = s_obj (slots C p)"
          using same False pd ne by (simp add: derive_keeps)
        moreover have "s_badge (slots C p) \<noteq> 0"
          using bp pd ne by (simp add: derive_keeps)
        ultimately show ?thesis
          using a8 by (simp add: inv_A8_def occupied_def)
      qed
      thus ?thesis using False pd ne by (simp add: derive_keeps)
    qed
  qed
qed

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
  "inv_A9 C \<Longrightarrow> inv_links_symmetric C \<Longrightarrow> i \<in> dom_s C
   \<Longrightarrow> inv_A9 (delete C i)"
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

      \<comment> \<open>PROVED now, and not by an induction on cycle length: the right
          statement is about the RELATION.  Every edge the delete leaves is
          either one `C` already had, or a reparented one — and a reparented
          edge is exactly two original edges, `(a, i)` and `(i, b)`.  So the
          new relation is contained in the TRANSITIVE CLOSURE of the old one,
          and well-foundedness passes to any subset of a well-founded
          closure (`wf_trancl`, `wf_subset`).  The bounded statement the
          solver could make needed the doubled premise for the same reason,
          and could not take the last step because a closure is not in its
          fragment.

          Two premises the obligation did not have: `inv_links_symmetric`,
          without which a child's parent link need not point back and the
          reparented edge is not two original edges; and `i \<in> dom_s C`,
          because `delete` as written reparents the children of a slot that
          does not exist.\<close>
proof -
  assume a9: "inv_A9 C" and ls: "inv_links_symmetric C" and idom: "i \<in> dom_s C"
  have t: "wf ({(a, b). a \<in> dom_s C \<and> s_parent (slots C a) = Some b}\<^sup>+)"
    using a9 unfolding inv_A9_def by (rule wf_trancl)
  show ?thesis
    unfolding inv_A9_def
    by (rule wf_subset[OF t delete_edges_in_trancl[OF ls idom]])
qed

lemma revoke_removes_exactly_the_subtree:
  \<comment> \<open>A10: revoke removes the descendants and NOT the root.  The strongest of
      these, and the one the executable model checks on every trace.

      ── The statement itself was wrong, and silently ───────────────────────

      It said `\<forall>d. descendant C i d \<longrightarrow> \<not> occupied C' d`, and `descendant` is
      defined NOWHERE.  Unlike the four operations — caught as type errors,
      because they stood where a `cspace` was required — this one sits in a
      BOOLEAN position, so Isabelle read it as a free variable of predicate
      type and the lemma type-checked.  What it then claimed was "for EVERY
      ternary predicate", which is false for `\<lambda>_ _ _. True` and unprovable.
      The `oops` was hiding a meaningless statement, not an unfinished proof,
      and a free variable is how a formal text goes quietly wrong.

      ── And it is three claims, not two ────────────────────────────────────

      The missing third is the one the model has always checked and the text
      never stated: nothing that is NOT a descendant is touched.  Without it
      "removes the descendants" is satisfied by an operation that removes
      everything.\<close>
  assumes a9: "inv_A9 C"
  shows "(\<forall>d \<in> desc C i. \<not> occupied (revoke C i) d)
         \<and> (occupied C i \<longrightarrow> occupied (revoke C i) i)
         \<and> (\<forall>t. t \<notin> desc C i \<longrightarrow> t \<noteq> i
                 \<longrightarrow> slots (revoke C i) t = slots C t)"
proof -
  let ?r = "{(a, b). a \<in> dom_s C \<and> s_parent (slots C a) = Some b}"
  \<comment> \<open>The root is not its own descendant, and THAT is what `inv_A9` is for:
      with a cycle through `i` the first branch of `revoke` would empty the
      very slot A10 promises to spare.\<close>
  have inot: "i \<notin> desc C i"
    using wf_irrefl[OF wf_trancl[OF a9[unfolded inv_A9_def]]]
    by (simp add: desc_def)
  show ?thesis
  proof (intro conjI ballI allI impI)
    fix d assume "d \<in> desc C i"
    thus "\<not> occupied (revoke C i) d"
      by (simp add: revoke_def Let_def occupied_def)
  next
    assume "occupied C i"
    thus "occupied (revoke C i) i"
      using inot by (simp add: revoke_def Let_def occupied_def)
  next
    fix t assume "t \<notin> desc C i" and "t \<noteq> i"
    thus "slots (revoke C i) t = slots C t"
      by (simp add: revoke_def Let_def)
  qed
qed

lemma retype_preserves_O2:
  \<comment> \<open>The `can_retype` premise is NEW and the obligation was false without
      it: nothing said the source holds an Untyped, and `inv_O2` is exactly
      the claim that a non-derivation edge comes from one.\<close>
  "inv_wf C \<Longrightarrow> inv_liveness C \<Longrightarrow> inv_O2 C
   \<Longrightarrow> can_retype C ut dst = Ok \<Longrightarrow> \<exists>x. objs C x = None
   \<Longrightarrow> inv_O2 (retype C ut ty dst)"
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
proof -
  assume wf: "inv_wf C" and lv: "inv_liveness C" and o2: "inv_O2 C"
     and ok: "can_retype C ut dst = Ok" and ex: "\<exists>x. objs C x = None"
  let ?R = "retype C ut ty dst"
  have out: "occupied C ut" using ok by (rule can_retype_ut_occupied)
  have edst: "\<not> occupied C dst" using ok by (rule can_retype_dst_empty)
  have ne: "ut \<noteq> dst" using out edst by auto
  have fr: "objs C (fresh_oid C) = None" using ex by (rule fresh_oid_fresh)
  have uty: "o_type (the (objs C (the (s_obj (slots C ut))))) = Untyped"
    using ok by (rule can_retype_ut_untyped)
  show ?thesis
    unfolding inv_O2_def
  proof (intro ballI impI allI)
    fix i p
    assume oi: "occupied ?R i"
       and par: "s_parent (slots ?R i) = Some p"
       and op: "occupied ?R p"
    show "s_obj (slots ?R i) = s_obj (slots ?R p)
          \<or> o_type (the (objs ?R (the (s_obj (slots ?R p))))) = Untyped"
    proof (cases "i = dst")
      case True
      \<comment> \<open>The new capability's parent is the Untyped it was carved from —
          which is the whole content of O2, and the premise the obligation was
          missing until `can_retype` existed.\<close>
      with par ne have pu: "p = ut" by (simp add: retype_parent_dst)
      have "the (s_obj (slots C ut)) \<noteq> fresh_oid C"
        using lv out fr by (rule slot_oid_not_fresh)
      hence "objs ?R (the (s_obj (slots ?R p))) = objs C (the (s_obj (slots C ut)))"
        using pu ne by (simp add: retype_keeps retype_objs_old)
      thus ?thesis using uty by simp
    next
      case False
      have pd: "p \<noteq> dst"
      proof
        assume "p = dst"
        moreover have "occupied C i"
          using oi False ne by (simp add: occupied_def retype_keeps retype_dom)
        moreover have "s_parent (slots C i) = Some p"
          using par False ne by (simp add: retype_keeps)
        ultimately show False
          using wf edst by (fastforce simp: inv_wf_def occupied_def)
      qed
      have oc: "occupied C i" and ocp: "occupied C p"
        using oi False ne op pd
        by (simp_all add: occupied_def retype_keeps retype_dom)
      have "s_obj (slots C i) = s_obj (slots C p)
            \<or> o_type (the (objs C (the (s_obj (slots C p))))) = Untyped"
        using o2 oc ocp par False ne pd
        by (simp add: inv_O2_def occupied_def retype_keeps)
      moreover have "the (s_obj (slots C p)) \<noteq> fresh_oid C"
        using lv ocp fr by (rule slot_oid_not_fresh)
      ultimately show ?thesis
        using False pd ne by (simp add: retype_keeps retype_objs_old)
    qed
  qed
qed


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
