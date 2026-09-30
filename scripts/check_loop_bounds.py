#!/usr/bin/env python3
"""
check_loop_bounds.py — every loop in the verification boundary states its bound.

Phase 2 of Stage V asks for "every loop with its bound written", and the word
that makes it checkable is WRITTEN.  Proving 81 loops terminate is a research
problem; requiring that each one says why it does is a gate, and it is the gate
that catches the loop somebody adds next year over a list a ring-3 principal
can extend.

── What counts as written ──────────────────────────────────────────────────

A COUNTED `for` — a scalar induction variable, a relational guard, and a step
of `++`/`--` — states its bound in its own header.  `for (i = 0; i < n; i++)`
needs no prose: the bound is `n`, it is right there, and a reader checking
whether `n` is bounded is reading the same line.  Those pass automatically.

Everything else has to carry a `BOUND:` note within six lines above it, or on
it.  That is: `while` loops, `for (;;)`, and pointer walks
(`for (p = head; p; p = p->next)`), which are exactly the shapes where
termination is an argument about a data structure rather than about a number.
A walk over a list a principal can lengthen is the whole reason this file
exists.

── The boundary ────────────────────────────────────────────────────────────

The files `spec_driver.c` links — the authority model's C, which is what
`spec-differential`, `spec-exhaustive` and the proofs are actually about.
Taken from the Makefile rather than listed here, so the two cannot drift: a
file added to the boundary is a file this checks, with no second edit.
"""
import re
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Stage V phase 2: "core <= 12k lines".  Code lines in the
# verification boundary — see the check at the bottom for why that
# is the definition and why comments do not count.
CORE_CEILING = 12000

LOOP    = re.compile(r"\b(for|while)\s*\(")
COUNTED = re.compile(r"for\s*\([^;]*;[^;]*[<>]=?[^;]*;\s*[^;]*(\+\+|--)")
NOTE    = re.compile(r"BOUND:")
# A comment line, a continuation of one, or blank — the region a note attached
# to the loop can occupy.  Searching a FIXED number of lines above instead was
# the first version, and a ten-line explanation pushed its own "BOUND:" out of
# the window: the checker then demanded a note that was already there.  A bound
# worth writing is usually worth more than six lines, so the window is the
# comment block, whatever length the explanation needed.
ATTACHED = re.compile(r"^\s*(/\*|\*|//|$)")


def boundary_files():
    mk = (ROOT / "Makefile").read_text()
    m = re.search(r"SPEC_DRIVER_SRCS\s*:?=\s*((?:.*\\\n)*.*)", mk)
    if not m:
        print("[loopbound] FAIL: SPEC_DRIVER_SRCS not found in the Makefile — "
              "the boundary is defined there and this cannot guess it")
        sys.exit(1)
    block = m.group(1).replace("\\\n", " ")
    return [t for t in block.split()
            if t.endswith(".c") and t.startswith("kernel/")]


def main():
    files = boundary_files()
    counted = 0
    noted = 0
    missing = []

    for rel in files:
        p = ROOT / rel
        if not p.exists():
            continue
        raw = p.read_text().splitlines()
        # Comments are kept — the note lives in one — but a loop keyword inside
        # a comment must not be mistaken for a loop.
        stripped = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                          p.read_text(), flags=re.S).splitlines()

        for i, line in enumerate(stripped):
            if not LOOP.search(line):
                continue
            if line.strip().startswith("*"):
                continue
            if COUNTED.search(line.strip()):
                counted += 1
                continue
            j = i - 1
            while j >= 0 and ATTACHED.match(raw[j]):
                j -= 1
            window = "\n".join(raw[j + 1:i + 1])
            if NOTE.search(window):
                noted += 1
            else:
                missing.append((rel, i + 1, raw[i].strip()[:96]))

    if missing:
        print("[loopbound] FAIL: %d loop(s) in the verification boundary do "
              "not state a bound:" % len(missing))
        for rel, ln, txt in missing:
            print("             %s:%d  %s" % (rel, ln, txt))
        print("           A counted `for` states its bound in its header and "
              "needs nothing.")
        print("           Anything else — while, for(;;), a pointer walk — "
              "needs a `BOUND:` note in")
        print("           the comment block directly above it, saying what "
              "stops it and whether")
        print("           ring 3 can make that larger.")
        return 1

    #
    # ── The other half of phase 2's measurable target: core <= 12k lines ────
    #
    # The roadmap has asked for that since the stage opened and never said
    # what "core" MEANS, which makes it a target nobody can pass or fail.  It
    # means this: the files a refinement proof would have to cover, which are
    # the files `spec_driver.c` links — the same boundary the loops above are
    # counted in, and the same one `spec-exhaustive` drives.  Defining it once,
    # here, is what turns the row into a number.
    #
    # Code lines, comments excluded, because the ceiling is about how much
    # there is to PROVE.  IRIS comments heavily on purpose and a proof
    # obligation is not created by a paragraph.
    #
    code = 0
    for rel in files:
        p = ROOT / rel
        if not p.exists():
            continue
        t = re.sub(r"/\*.*?\*/", "", p.read_text(), flags=re.S)
        t = re.sub(r"//[^\n]*", "", t)
        code += sum(1 for l in t.splitlines() if l.strip())

    if code > CORE_CEILING:
        print("[loopbound] FAIL: the verification boundary is %d code lines, "
              "over the %d ceiling" % (code, CORE_CEILING))
        print("           Stage V phase 2 asks for a core small enough to "
              "prove.  Raising the")
        print("           ceiling is a roadmap change and needs a reason in "
              "the same commit.")
        return 1

    print("[loopbound] RESULT: OK (%d counted loops, %d with a written bound, "
          "%d code lines of %d, across %d files)"
          % (counted, noted, code, CORE_CEILING, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
