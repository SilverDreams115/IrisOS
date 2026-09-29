/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_LIBC_LOCK_H
#define IRIS_LIBC_LOCK_H

/*
 * libc_lock.h — what this library shares between threads, and what it does not.
 *
 * ── A program here CAN have two threads ────────────────────────────────────
 *
 * That is why this file exists.  A program holds `IRIS_CPTR_OWN_UNTYPED`, its
 * own CSpace and its own VSpace, so it can retype a TCB and configure it
 * against the same two — which is what a "second thread" IS in this system,
 * there being no process object.  Nothing has to be asked for it and nothing
 * can refuse it.  A C library that assumed one thread would therefore be
 * assuming something the program is free to make false at any moment, without
 * doing anything unusual.
 *
 * So the shared data structures are locked, and the things that are still
 * per-thread-in-name-only are LISTED, below, rather than discovered.
 *
 * ── Why a spin and not a wait ──────────────────────────────────────────────
 *
 * The blocking primitive here is a Notification, and one per lock is an object
 * per lock: retyped from the program's budget, occupying a CSpace slot, and
 * needing to exist before the first `malloc` — which runs before the program's
 * first instruction.  The contended path is instead a yield, which needs no
 * capability at all and hands the processor to whoever holds the lock.
 *
 * That is the right trade only while these locks are held for a few hundred
 * instructions, which is true of all three (a free-list walk, a line buffer, a
 * slot scan).  It stops being true the moment one of them covers an IPC, and
 * the day that happens this comment is the record of what has to change.
 *
 * ── What is still NOT safe with two threads, stated plainly ────────────────
 *
 *   - `errno`.  One word, shared.  `__errno_location()` exists so that the
 *     storage can move into the thread's own block without anything that
 *     reads `errno` changing, and that move has not happened.
 *   - The IPC buffer.  `__libc_console_init` registers ONE frame against the
 *     calling thread's TCB (`TCB_SetIPCBuffer`), so a second thread has no
 *     buffer registered and the kernel refuses its bulk sends.  Printing and
 *     `open`/`read` therefore work on the thread that started the program and
 *     no other.  A per-thread buffer is the fix, and it is a real piece of
 *     work: a frame and a slot per thread, retyped when the thread is made.
 *   - The thread pointer.  `__libc_tls_init` sets `%fs` for one thread; a
 *     second one starts with none, so the stack protector in any function it
 *     enters reads from an unmapped address.
 *
 * A program that makes a thread today gets a correct heap and a correct
 * `printf` buffer from it, and nothing else.  That is a bigger promise than
 * the library made before this file, and a much smaller one than "threads
 * work".
 */

/* Unlocked is 0.  `__atomic_test_and_set` works on any scalar and lowers to a
 * single instruction on x86-64, so this needs no library behind it. */
struct libc_lock { volatile int held; };

#define LIBC_LOCK_INIT { 0 }

void __libc_lock(struct libc_lock *l);
void __libc_unlock(struct libc_lock *l);

#endif /* IRIS_LIBC_LOCK_H */
