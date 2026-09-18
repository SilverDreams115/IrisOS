#ifndef IRIS_FBCON_H
#define IRIS_FBCON_H

#include <stdint.h>
#include <iris/fb_info.h>

/*
 * fbcon — the kernel's console of last resort, on the screen.
 *
 * ── Why the kernel paints at all ───────────────────────────────────────────
 *
 * Output is a ring-3 job in this system: `console` holds the KIoPort for
 * 0x3F8 and writes every `[USER]` line you see.  The kernel keeps a serial
 * path anyway, for panic and for the stretch of boot before ring 3 exists,
 * because a diagnostic that needs a working userspace cannot report a
 * userspace that never started.  This file is that same exception on a
 * second device, and it exists for one reason: MOST MACHINES HAVE NO SERIAL
 * PORT.  On such a machine every existing diagnostic in this kernel writes to
 * a port that is not there, and a boot that dies early is a black screen.
 *
 * It is deliberately NOT a driver in the sense the charter uses.  It claims
 * nothing, allocates nothing, and takes no capability: it writes pixels to an
 * address the firmware already decided on and the kernel already maps.  When
 * ring 3 is alive the framebuffer belongs to a ring-3 service, and this code
 * is expected to be silent.
 *
 * ── What it will not do ────────────────────────────────────────────────────
 *
 * There is no colour beyond white on black, because the pixel FORMAT is not
 * recorded anywhere — UEFI reports RGBA on some machines and BGRA on others,
 * and this boot protocol carries only geometry.  White and black are the two
 * values that mean the same thing in both, so they are the two it uses.
 *
 * Anything other than 32 bits per pixel is refused rather than guessed at.
 */

/* Safe to call with a framebuffer that does not exist; everything after it is
 * then a no-op.  Returns 1 if there is a screen to write on. */
int  fbcon_init(const struct iris_fb_params *p);

int  fbcon_active(void);
void fbcon_putc(char c);
void fbcon_write(const char *s);

/*
 * The screen has one owner at a time.
 *
 * `fbcon_yield` is called when ring 3 claims the framebuffer, and everything
 * above goes quiet: two writers painting one screen produce a screen that
 * describes neither.  From that point the kernel's log lives in the ring the
 * `console` service drains, which is where it belonged all along.
 *
 * `fbcon_reclaim` takes it back, and exactly one caller is entitled to —
 * panic.  A kernel that is about to stop has no userspace left to be polite
 * to, and the alternative is dying silently behind somebody else's pixels.
 */
void fbcon_yield(void);
void fbcon_reclaim(void);

/*
 * A boot marker: one character, on a reserved line at the top of the screen,
 * that does not scroll and does not depend on fbcon having been initialised
 * with anything more than a base address.
 *
 * It mirrors the raw `_early_putc` bytes the kernel already emits at each
 * point the boot can die.  On a machine with a serial port those bytes are
 * the record; on a machine without one they were nothing at all, which is
 * precisely the case this is for.
 */
void fbcon_mark(char c);

#endif /* IRIS_FBCON_H */
