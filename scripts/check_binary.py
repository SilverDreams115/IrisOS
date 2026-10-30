#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
check_binary.py — the kernel's indirect control transfers, inventoried.

Phase 5 asks for binary verification.  This is the modest, real part of it:
`check-purity` proves a property of the SOURCE — "destructor dispatch: by type,
no ops pointer in any kernel file" — and nobody had checked what the compiler
made of it.

It made six indirect jumps.  A dense `switch` becomes a JUMP TABLE, so the very
statements that replaced the 18 `KObjectOps` tables compile to
`notrack jmp *%rax` through a table of addresses.  That is not a defect and it
is not nothing: the distinction a verification story has to state is WHERE the
targets live.  A jump table sits in `.rodata`, derived from the switch and
unwritable; `KObjectOps` sat in `.data`, and anything that could write it could
choose what the kernel called next.  So this lane checks both halves — the
inventory, and that every table it jumps through is read-only.

Two more are register-indirect `call`s and neither is dispatch:
`sched_resume` calls `task->kentry` to start a kernel thread, and `tramp_lm`
calls the per-CPU entry from the AP trampoline.

── Why this file carries so many controls ──────────────────────────────────

Because the first version of this check reported "0 indirect calls", which
matched the source-level claim exactly and was completely false: the ELF did
not exist, and `2>/dev/null` had swallowed `objdump: No such file`.  A zero
with no denominator is how a non-check disguises itself as evidence, and this
one disguised itself as agreement with a property I wanted to confirm.  So:
the binary must exist, the disassembly must be large, the DIRECT calls must be
plentiful, and only then does the indirect count mean anything.
"""
import re
import subprocess
import sys

ELF = "build/kernel.elf"

# Every indirect transfer the kernel is allowed to contain, by the function it
# is in and why it is there.  A new one fails this lane.
ALLOWED = {
    "kobject_dispatch_close":        "switch on obj->type -> jump table",
    "kobject_dispatch_destroy":      "switch on obj->type -> jump table",
    "sys_untyped_retype2":           "switch on the requested type -> jump table",
    "syscall_invoke":                "switch on the invocation label -> jump table",
    "root_bootinfo_set_control_cap": "switch on the control slot -> jump table",
    "sched_resume":                  "task->kentry, a kernel thread's entry",
    "tramp_lm":                      "the AP trampoline's per-CPU entry",
}

MIN_INSNS = 10000      # the disassembly must be real
MIN_DIRECT = 1000      # and so must the control


def main():
    try:
        dis = subprocess.run(["objdump", "-d", ELF], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError) as e:
        print("[binary] FAIL: cannot disassemble %s — %s" % (ELF, e))
        print("         Run `make` first.  This lane reports nothing rather "
              "than reporting zero, because a zero here reads like a result.")
        return 1

    insns = len(re.findall(r"^\s+[0-9a-f]+:", dis, re.M))
    direct = len(re.findall(r"\scall\s+[0-9a-f]", dis))
    if insns < MIN_INSNS or direct < MIN_DIRECT:
        print("[binary] FAIL: the disassembly is not credible — %d "
              "instructions, %d direct calls (want >=%d, >=%d).  Any count of "
              "indirect transfers below would be meaningless."
              % (insns, direct, MIN_INSNS, MIN_DIRECT))
        return 1

    #
    # The SEGMENTS, which is what the hardware enforces.
    #
    # The jump-table check used to ask whether a table was inside `.rodata`,
    # by name.  What actually matters is whether the page it lives on can be
    # WRITTEN: a section's name is a convention and the segment's flags are a
    # permission.  So the segments are read, W^X is required of all of them,
    # and a table has to land in one that is not writable.
    segs = []
    phdr = subprocess.run(["readelf", "-lW", ELF], capture_output=True,
                          text=True).stdout
    for ln in phdr.splitlines():
        f = ln.split()
        if len(f) >= 8 and f[0] == "LOAD":
            try:
                vaddr = int(f[2], 16); memsz = int(f[5], 16)
            except ValueError:
                continue
            flags = "".join(f[6:-1]) if len(f) > 8 else f[6]
            segs.append((vaddr, vaddr + memsz, flags))
    if len(segs) < 2:
        print("[binary] FAIL: parsed %d LOAD segments — any permission claim "
              "below would be meaningless" % len(segs))
        return 1

    wx = [s_ for s_ in segs if "W" in s_[2] and "E" in s_[2]]
    if wx:
        print("[binary] FAIL: %d segment(s) are WRITABLE and EXECUTABLE:" % len(wx))
        for a, b, fl in wx:
            print("         [0x%x,0x%x) flags=%s" % (a, b, fl))
        print("         A page that can be written and executed is where a "
              "jump table stops being read-only data.")
        return 1

    def writable_at(a):
        for lo, hi, fl in segs:
            if lo <= a < hi:
                return "W" in fl
        return True        # outside every segment: treat as unsafe

    fn, found, bad = "?", [], 0
    prev = []
    for ln in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:", ln)
        if m:
            fn, prev = m.group(1), []
            continue
        if re.search(r"\s(call|jmp)q?\s+\*", ln):
            addr = ln.split(":")[0].strip()
            found.append((addr, fn))
            if fn not in ALLOWED:
                print("[binary] FAIL: a NEW indirect transfer in %s at %s"
                      % (fn, addr))
                print("         %s" % ln.strip())
                print("         If this is dispatch, check-purity's source "
                      "claim no longer describes the binary.")
                bad += 1
            # A jump table loads its target from an absolute address; require
            # that address to be read-only.
            for p in prev[-3:]:
                t = re.search(r"mov\s+0x([0-9a-f]+)\(,%r[a-z0-9]+,8\)", p)
                if t:
                    a = int(t.group(1), 16)
                    if writable_at(a):
                        print("[binary] FAIL: %s jumps through a table at "
                              "0x%x, on a page that can be WRITTEN"
                              % (fn, a))
                        bad += 1
        prev.append(ln)

    unused = [k for k in ALLOWED if k not in {f for _, f in found}]
    if unused:
        # Not a failure: the compiler may stop needing a table.  But an
        # allowlist entry nothing matches is an entry nobody is maintaining.
        print("[binary] note: allowlist entries with no indirect transfer "
              "any more — %s" % ", ".join(sorted(unused)))

    if bad:
        print("[binary] RESULT: %d problem(s)" % bad)
        return 1
    print("[binary] RESULT: OK (%d instructions, %d direct calls, %d indirect "
          "transfers and every one accounted for; %d LOAD segments, none "
          "writable+executable, every jump table unwritable)"
          % (insns, direct, len(found), len(segs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
