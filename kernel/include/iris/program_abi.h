/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_PROGRAM_ABI_H
#define IRIS_PROGRAM_ABI_H

/*
 * What `proc` gives a program.  The machine-readable half of
 * `docs/contracts/program.md`, which carries the reasoning; this file carries
 * the numbers so the two cannot drift apart silently.
 *
 * A PROGRAM is not a service.  A service is an image in the kernel initrd,
 * spawned by `init` with a manifest written in `init`.  A program is an ELF
 * file on a filesystem, spawned by path, carrying a C runtime — so it needs
 * things no service has needed: a heap, a stack with arguments on it, and a
 * thread pointer.
 *
 * Slots 1..19 keep the meanings `iris/endpoint_proto.h` gives them.  A program
 * is given a SUBSET, and a slot it was not given is empty: resolving it is
 * NOT_FOUND, which is the right answer for authority nobody granted.
 */

#include <stdint.h>

/*
 * ── The object table ──────────────────────────────────────
 *
 * The part of the contract that makes dynamic linking honest.
 *
 * A stock dynamic linker resolves `DT_NEEDED` against `DT_RPATH`,
 * `LD_LIBRARY_PATH` and `/lib` — "open any path it can name" arriving through
 * the loader rather than through the program, which is what charter §6 refused
 * (ledger A-49).  Here the SPAWNER resolves the transitive set BEFORE the child
 * exists and mints one capability per object into this run of slots.  The
 * interpreter reads a table it was given and can reach nothing else.
 *
 * Every entry is a derivation child of the spawner's, so revoking one removes
 * the object from every process that has it.
 */
#define IRIS_PROG_SLOT_OBJ_BASE   64u
#define IRIS_PROG_SLOT_OBJ_MAX    64u    /* slots 64..127 */

/* Reserved and left EMPTY: `iris_test` puts its fixtures here, and a program
 * that is one day run under the harness must not find its own capabilities
 * where the harness expects its own. */
#define IRIS_PROG_SLOT_RESERVED_LO 20u
#define IRIS_PROG_SLOT_RESERVED_HI 31u

/* The program's own, free from the first instruction — 32..63, and 128 up. */
#define IRIS_PROG_SLOT_FREE_LO    32u
#define IRIS_PROG_SLOT_FREE_HI    63u
#define IRIS_PROG_SLOT_FREE2_LO   128u

/*
 * ── The address space ─────────────────────────────────────
 *
 * Offsets from USER_PRIVATE_BASE.  The image region is what the existing
 * loader already randomises within; the rest is this contract.
 *
 * The interpreter gets a region of its OWN, deliberately: two ET_DYN objects
 * biased out of one range can overlap, and a loader that had to check for that
 * is a loader with a failure mode.  Disjoint ranges cannot collide at all.
 */
#define IRIS_PROG_IMAGE_OFF       0x00200000ULL   /* = USER_TEXT_BASE  */
#define IRIS_PROG_IMAGE_END_OFF   0x50000000ULL   /* = USER_VMO_BASE   */
#define IRIS_PROG_INTERP_OFF      0x50000000ULL
#define IRIS_PROG_INTERP_END_OFF  0x60000000ULL
#define IRIS_PROG_HEAP_OFF        0x60000000ULL
#define IRIS_PROG_HEAP_END_OFF    0x70000000ULL
#define IRIS_PROG_MMAP_OFF        0x70000000ULL   /* .. USER_STACK_BASE */

/*
 * ── The auxiliary vector ──────────────────────────────────
 *
 * The standard entries are the standard numbers; a runtime reads them where
 * every other runtime does.  These three are this system's own, because a
 * program here must be told things a Linux program reads out of `/proc` or
 * finds by searching a path.
 *
 * The values are far above anything the standard set will ever reach (Linux is
 * below 64), and they spell "IR" so that a stray one in a dump is recognisable
 * rather than a number.
 *
 * `OBJV` and `UNTYPED` carry SLOT NUMBERS that this header also defines, which
 * looks redundant and is not: a program that reads them keeps working if the
 * table or the budget moves, and one that hardcodes the constant does not.
 * The indirection is the whole point of spending two auxv entries on it.
 */
/*
 * The standard entries, by their System V numbers.  They are written here
 * rather than taken from a libc header because there is no libc here yet and
 * the spawner has to emit them before there is one — and because a contract
 * that names `AT_PHDR` in prose and nowhere in code is a contract the compiler
 * cannot check.
 */
#define AT_NULL                   0ULL
#define AT_PHDR                   3ULL
#define AT_PHENT                  4ULL
#define AT_PHNUM                  5ULL
#define AT_PAGESZ                 6ULL
#define AT_BASE                   7ULL
#define AT_ENTRY                  9ULL
#define AT_RANDOM                25ULL

#define AT_IRIS_OBJC              0x49520001ULL   /* objects the table holds  */
#define AT_IRIS_OBJV              0x49520002ULL   /* the first object's slot  */
#define AT_IRIS_UNTYPED           0x49520003ULL   /* the budget's slot        */

/*
 * ── What `proc` answers ───────────────────────────────────
 */
#define PROC_SLOT_CTRL_EP         5u    /* the endpoint it serves on          */
#define PROC_SLOT_REPLY           6u    /* the reply object its receive stages */
#define PROC_SLOT_VFS_EP          7u    /* to read an ELF by path             */
#define PROC_SLOT_CONSOLE_EP      8u    /* handed on to programs it spawns    */
/* Its own budget arrives at IRIS_CPTR_OWN_UNTYPED (12), like every service's. */

/* Spawn a program.  words[0] = length of the path, which follows in the
 * message's own page.  Replies with the child's first thread as a capability,
 * so the caller can watch it, and its exit status is read with SYS_TCB_*. */
#define PROC_OP_SPAWN             0x7401u
/* What the service found and refused, in numbers, for a machine with no
 * serial port: a spawn that failed names the step it failed at. */
#define PROC_OP_INFO              0x7402u

#define PROC_REP_OK               0x7480u
#define PROC_REP_ERR              0x7481u

/*
 * Where a spawn stopped.  The same shape the network service uses, and for the
 * same reason: "it did not run" names no cause, and on a machine whose only
 * output is a report on a disk, the step IS the diagnosis.
 */
#define PROC_STEP_NONE            0u   /* nothing attempted                  */
#define PROC_STEP_PATH            1u   /* the path was not readable          */
#define PROC_STEP_ELF             2u   /* not an ELF this system can load    */
#define PROC_STEP_INTERP          3u   /* its interpreter could not be found */
#define PROC_STEP_OBJECTS         4u   /* an object it needs, or too many    */
#define PROC_STEP_BUDGET          5u   /* the child's memory could not be cut */
#define PROC_STEP_CSPACE          6u   /* its initial CSpace could not be built */
#define PROC_STEP_LOAD            7u   /* a segment could not be mapped      */
#define PROC_STEP_STACK           8u   /* argv/envp/auxv would not fit       */
#define PROC_STEP_START           9u   /* it would not resume                */
#define PROC_STEP_RUNNING        12u   /* up; the same "12 is up" the net
                                        * backends use, so one number means
                                        * one thing across the tree */

#endif /* IRIS_PROGRAM_ABI_H */
