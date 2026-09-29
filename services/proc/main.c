/* SPDX-License-Identifier: Apache-2.0 */
/*
 * proc/main.c — the service that spawns a PROGRAM.
 *
 * `init` spawns SERVICES: an image in the kernel initrd, found by index, with
 * a manifest of capabilities written in `init` itself.  Nothing there is a
 * file, and nothing there can be launched by anything but `init`.
 *
 * This service spawns the other kind of thing.  It takes a PATH, reads the ELF
 * behind it through the VFS, and builds a child that holds what
 * `docs/contracts/program.md` says a program holds — a budget, its own address
 * space and thread, and a System V initial stack with `argv`, `envp` and an
 * auxiliary vector on it.  That last part is the whole reason this exists: a
 * C runtime starts by reading that vector, and nothing in this tree had ever
 * written one.
 *
 * ── One loader, not two ────────────────────────────────────────────────────
 *
 * It does NOT reimplement loading.  `svc_loader` had exactly one line that
 * cared where the image came from; that line is a parameter now, so a program
 * goes through the same retype-configure-resume a service does.  The only
 * thing this file adds is what happens on either side of it: reading a file
 * into a frame before, and writing a stack after.
 *
 * ── What it deliberately does not hold ─────────────────────────────────────
 *
 * No initrd capability.  A program comes from a filesystem; a spawner that
 * could also reach the kernel's boot images would be two mechanisms wearing
 * one name, and the one that answers "where did this binary come from" would
 * stop being the filesystem.
 *
 * ── What is not here yet, and is not pretended ─────────────────────────────
 *
 * An ELF with a `PT_INTERP` is REFUSED, with `PROC_STEP_INTERP`.  Loading an
 * interpreter, resolving `DT_NEEDED` against an object table and filling
 * slots 64.. is step 4 of Stage 10-run.  The auxiliary vector already carries
 * `AT_BASE`, `AT_IRIS_OBJC` and `AT_IRIS_OBJV` — at 0, 0 and 64 — so the
 * binaries built against this contract do not change when step 4 fills them.
 * Refusing loudly is the alternative to loading a dynamic binary that would
 * then jump to an entry point nothing had relocated.
 */
#include <stdint.h>
#include "../common/iris_msg.h"
#include "../common/iris_map.h"
#include "../common/iris_ipc_buffer.h"
#include "../common/svc_loader.h"
#include "../common/prog_stack.h"
#include "../common/elf64.h"
#include "../common/console_client.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/user_ctx.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include <iris/program_abi.h>

/* ── this service's own slots ────────────────────────────────────────────── */
/* 5, 6, 7, 8 and 12 are the manifest (program_abi.h); 32.. is a service's own
 * range, which is where everything a spawn needs a capability for goes. */
#define PROC_SLOT_WS          40u  /* the loader's second-level CNode         */
#define PROC_SLOT_PT          41u  /* page-table scratch for our own maps     */
#define PROC_SLOT_IMGPOOL     42u  /* the Untyped the image frame is cut from */
#define PROC_SLOT_CHILD_STACK 43u  /* the child's stack, while we write it    */
#define PROC_SLOT_CHILD_TCB   44u  /* the child's thread, handed to the caller */
#define PROC_SLOT_IPCBUF      45u
#define PROC_SLOT_IPCBUF_PT   46u

/* ── where we map, in our own address space ──────────────────────────────── */
#define PROC_VA_IMAGE  0x80E0000000ULL
#define PROC_VA_STACK  0x80E8000000ULL

/* The image pool, and the largest program this can read.
 *
 * The pool is RESET before every spawn, so a spawn costs nothing that the next
 * one does not reclaim — the same arrangement the loader's own ELF scratch
 * uses, for the same reason: without it a service that spawns in a loop spends
 * its whole budget one binary at a time and reports nothing. */
#define PROC_IMGPOOL_BYTES (2u << 20)
#define PROC_IMAGE_MAX     (1u << 20)
/* What a child gets when the request does not say.  The spawner knows what it
 * is launching, so the request may say; this is only the answer for one that
 * does not care. */
#define PROC_BUDGET_DEFAULT (4u << 20)

/*
 * Why a PATH step failed, reported beside it.
 *
 * `PROC_STEP_PATH` says the file could not be read, and that covers a name the
 * filesystem does not have, a file too large to load, and a service that never
 * got an IPC buffer to read one THROUGH — three different things to fix.  The
 * step is the contract (`program_abi.h`); this is the detail beside it, and it
 * is here rather than in the header because it is this implementation's
 * account of itself, not something a caller binds to.
 */
#define PROC_WHY_NONE   0u
#define PROC_WHY_NOBUF  1u   /* no registered IPC buffer: nothing can be read */
#define PROC_WHY_STAT   2u   /* the filesystem does not have that name       */
#define PROC_WHY_SIZE   3u   /* too small to be an ELF, or too large to load */
#define PROC_WHY_POOL   4u   /* the image pool would not reset or carve      */
#define PROC_WHY_FRAME  5u   /* no frame for it                              */
#define PROC_WHY_MAP    6u   /* the frame would not map here                 */
#define PROC_WHY_READ   7u   /* the filesystem stopped answering mid-file    */
#define PROC_WHY_WS     8u   /* the loader's workspace CNode would not carve  */

static uint8_t *g_buf;               /* this thread's registered IPC buffer */
static uint64_t g_spawns, g_started;
static uint32_t g_last_step;
/*
 * The last child's BUDGET, and what it was given.
 *
 * Kept because reclamation has to be nameable.  `Untyped_Reset` on this region
 * is the whole of "the child is gone and its memory is back", and it refuses
 * with BUSY while anything is still charged to it — which is the model working,
 * not a failure: a capability to anything inside the region is exactly what
 * should keep it from being rewound.
 */
static uint32_t g_child_budget_c;
static uint64_t g_child_budget_bytes;

/*
 * What this service says out loud, which is almost nothing.
 *
 * Every way a spawn can fail reaches the caller AS the answer — a step, a
 * reason and the kernel's own error — so logging them here would say the same
 * thing twice to a different audience.  What is left is the case the answer
 * cannot cover: an answer that never arrives.
 */
static void proc_log(const char *s) {
    if (!g_buf) return;
    (void)console_ep_write((iris_cptr_t)PROC_SLOT_CONSOLE_EP, g_buf, s);
}

static void proc_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}

static void proc_slot_delete(uint32_t slot) {
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
}

static uint32_t proc_strlen(const char *s) {
    uint32_t n = 0; while (s[n]) n++; return n;
}

/*
 * A VFS call, with the path re-staged every time.
 *
 * The request payload (the path) and the reply payload (the data) are the same
 * registered buffer — there is no pointer on the message path — so a second
 * call with the path still in the buffer would be a second call whose path has
 * been overwritten by the first call's answer.  `vfs_ep_proto.h` says so; this
 * is what saying so costs.
 */
static long proc_vfs(uint64_t label, const char *path,
                     uint64_t w0, uint64_t w1, uint32_t wc,
                     struct iris_msg *out) {
    uint32_t plen = proc_strlen(path);
    if (plen + 1u > VFS_EP_PATH_MAX) return (long)IRIS_ERR_INVALID_ARG;
    if (!g_buf) return (long)IRIS_ERR_NOT_SUPPORTED;

    proc_msg_zero(out);
    out->label      = label;
    out->words[0]   = w0;
    out->words[1]   = w1;
    out->word_count = wc;
    for (uint32_t i = 0; i < plen; i++) g_buf[i] = (uint8_t)path[i];
    g_buf[plen] = 0u;
    out->buf_len = plen + 1u;

    long r = iris_msg_call((long)PROC_SLOT_VFS_EP, out);
    if (r != 0) return r;
    if (out->label != IRIS_EP_REPLY_OK) return (long)IRIS_ERR_NOT_FOUND;
    return 0;
}

/*
 * Read the whole file into a frame this service owns, and answer its size.
 *
 * The frame lands in the LOADER's image slot rather than one of ours, because
 * the loader takes ownership of it either way and a copy that lived here too
 * would be a second reference keeping the image alive past the spawn.
 */
static long proc_read_image(const char *path, uint64_t ws, uint32_t *why) {
    struct iris_msg m;
    uint64_t size, off;

    *why = PROC_WHY_STAT;
    if (!g_buf) { *why = PROC_WHY_NOBUF; return -1; }
    if (proc_vfs(VFS_EP_OP_STAT, path, 0, 0, 0u, &m) != 0) return -1;
    size = m.words[1];
    *why = PROC_WHY_SIZE;
    if (size < sizeof(Elf64_Ehdr) || size > PROC_IMAGE_MAX) return -1;
    /* The workspace holds the image, so it has to exist before the image
     * does — the loader would otherwise create it several steps too late. */
    *why = PROC_WHY_WS;
    if (!svc_ws_ensure(ws)) return -1;
    *why = PROC_WHY_POOL;

    /* The pool: reset if it is there, carved if it is not.  A reset that
     * reports BUSY means the previous image is somehow still referenced, and
     * carving a second pool on top of it would strand the first for ever — so
     * that is a failure, not something to work around. */
    {
        long pr = iris_invoke0((long)PROC_SLOT_IMGPOOL, INV_UNTYPED_RESET);
        if (pr == (long)IRIS_ERR_BUSY) return -1;
        if (pr != 0 &&
            iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                        (long)((uint64_t)IRIS_KOBJ_UNTYPED | (1ULL << 32)),
                        (long)((uint64_t)PROC_SLOT_IMGPOOL << 32),
                        (long)PROC_IMGPOOL_BYTES) != 0)
            return -1;
    }

    /* A frame is whole pages: the retype refuses anything else, and a file
     * is whatever length it is.  The loader is told the FILE's size, which is
     * what bounds every offset it reads out of the image; the frame is the
     * page-rounded thing that holds it. */
    {
        uint64_t pages = (size + 0xFFFULL) & ~0xFFFULL;
        *why = PROC_WHY_FRAME;
        if (iris_invoke((long)PROC_SLOT_IMGPOOL, INV_UNTYPED_RETYPE,
                        (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                        (long)svc_image_dest(ws), (long)pages) != 0)
            return -1;

        *why = PROC_WHY_MAP;
        if (iris_map_frame(svc_image_slot(ws), IRIS_CPTR_OWN_VSPACE,
                           IRIS_CPTR_OWN_UNTYPED, PROC_SLOT_PT,
                           PROC_VA_IMAGE, pages, 1ull) != 0)
            return -1;
    }

    *why = PROC_WHY_READ;

    /* One reply at a time; the server clamps to its own buffer, so the loop is
     * over what it actually returned rather than over what we asked for. */
    for (off = 0; off < size; ) {
        uint8_t *dst = (uint8_t *)(uintptr_t)(PROC_VA_IMAGE + off);
        uint64_t n;
        if (proc_vfs(VFS_EP_OP_READ_AT, path, off, VFS_EP_DATA_MAX, 2u, &m) != 0)
            return -1;
        n = m.words[1];
        if (n == 0u || n > size - off || m.buf_len != (uint32_t)n) return -1;
        for (uint64_t i = 0; i < n; i++) dst[i] = g_buf[i];
        off += n;
    }
    *why = PROC_WHY_NONE;
    return (long)size;
}

/* What the header said, kept across the unmap so the auxiliary vector can be
 * built from it once the bias is known. */
struct proc_image {
    uint64_t entry;       /* e_entry, unbiased                               */
    uint64_t phdr_vaddr;  /* the program headers' own vaddr, unbiased        */
    uint32_t phentsize;
    uint32_t phnum;
    int      has_phdr;    /* whether phdr_vaddr was FOUND, not merely nonzero */
    int      has_interp;
};

/*
 * Parse the mapped image.  Returns 0, or the PROC_STEP_* that says why not.
 *
 * `AT_PHDR` has to be the program headers AT THEIR MAPPED ADDRESS, and the
 * only honest way to get there is the file's own account of where they are:
 * `PT_PHDR` when it has one, otherwise the `PT_LOAD` whose file range contains
 * `e_phoff`.  Assuming the first segment starts at file offset 0 is true of
 * everything this tree links today and is not a property of ELF.
 */
static uint32_t proc_parse(uint64_t size, struct proc_image *out) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(uintptr_t)PROC_VA_IMAGE;
    const Elf64_Phdr *ph;
    uint64_t phsz;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') return PROC_STEP_ELF;
    if (eh->e_type != (Elf64_Half)ET_DYN)                return PROC_STEP_ELF;
    if (eh->e_machine != (Elf64_Half)EM_X86_64)          return PROC_STEP_ELF;
    if (eh->e_phnum == 0 ||
        eh->e_phentsize < (Elf64_Half)sizeof(Elf64_Phdr)) return PROC_STEP_ELF;

    phsz = (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
    if (eh->e_phoff > size || phsz > size - eh->e_phoff)  return PROC_STEP_ELF;
    ph = (const Elf64_Phdr *)(uintptr_t)(PROC_VA_IMAGE + eh->e_phoff);

    out->entry      = eh->e_entry;
    out->phentsize  = eh->e_phentsize;
    out->phnum      = eh->e_phnum;
    out->phdr_vaddr = 0;
    out->has_phdr   = 0;
    out->has_interp = 0;

    for (uint32_t i = 0; i < out->phnum; i++) {
        const Elf64_Phdr *p = (const Elf64_Phdr *)(uintptr_t)
                                 ((uintptr_t)ph + i * out->phentsize);
        if (p->p_type == PT_INTERP) out->has_interp = 1;
        if (p->p_type == PT_PHDR) { out->phdr_vaddr = p->p_vaddr; out->has_phdr = 1; }
    }
    if (!out->has_phdr) {
        for (uint32_t i = 0; i < out->phnum; i++) {
            const Elf64_Phdr *p = (const Elf64_Phdr *)(uintptr_t)
                                     ((uintptr_t)ph + i * out->phentsize);
            if (p->p_type != PT_LOAD) continue;
            if (eh->e_phoff < p->p_offset) continue;
            if (eh->e_phoff - p->p_offset >= p->p_filesz) continue;
            out->phdr_vaddr = p->p_vaddr + (eh->e_phoff - p->p_offset);
            out->has_phdr   = 1;
            break;
        }
    }
    /* A program that cannot say where its own headers are cannot be handed an
     * `AT_PHDR`, and a runtime given a wrong one walks arbitrary memory.  It is
     * refused here rather than started with a plausible guess. */
    if (!out->has_phdr)        return PROC_STEP_ELF;
    if (out->has_interp)       return PROC_STEP_INTERP;
    return 0;
}

/*
 * The spawn.
 *
 * Returns PROC_STEP_RUNNING, or the step it failed at.  A step number rather
 * than an error code because a spawn has ten places to fail and an
 * `IRIS_ERR_NO_MEMORY` from any of them tells the caller nothing about which.
 */
static uint32_t proc_spawn(const char *path, uint64_t budget, uint32_t *why,
                           long *detail) {
    uint64_t ws = SVC_LOADER_WS(IRIS_CPTR_OWN_UNTYPED, PROC_SLOT_WS);
    struct proc_image img;
    struct iris_user_ctx ctx;
    iris_cptr_t child_h = IRIS_CPTR_NULL, boot_h = IRIS_CPTR_NULL;
    long size, r;
    *detail = 0;
    uint64_t bias, rsp;

    /* Whatever the last spawn left. */
    proc_slot_delete(PROC_SLOT_CHILD_STACK);
    proc_slot_delete(PROC_SLOT_CHILD_TCB);

    size = proc_read_image(path, ws, why);
    if (size < 0) return PROC_STEP_PATH;

    {
        uint32_t step = proc_parse((uint64_t)size, &img);
        (void)iris_invoke2((long)svc_image_slot(ws), INV_FRAME_UNMAP,
                           (long)IRIS_CPTR_OWN_VSPACE, (long)PROC_VA_IMAGE);
        if (step) { proc_slot_delete(svc_image_slot(ws)); return step; }
    }

    /*
     * The manifest.  Slots 2 and 3 are the two endpoints a program may need
     * before it has anything else; 12, 18 and 19 the loader mints itself once
     * a budget slot is named.  Slots 5 and 13 — an endpoint and a reply object
     * of its own — are left EMPTY on purpose: they would have to be retyped
     * per spawn out of memory nothing reclaims when the program dies, and the
     * contract already says a slot a program was not given is empty.  A
     * program that means to serve asks for one from its own budget.
     */
    {
        struct svc_mint pm[2] = { 0 };
        uint32_t n = 0;
        pm[n].slot = IRIS_CPTR_VFS_EP;     pm[n].src_cptr = PROC_SLOT_VFS_EP;
        pm[n].rights = RIGHT_WRITE;        n++;
        pm[n].slot = IRIS_CPTR_CONSOLE_EP; pm[n].src_cptr = PROC_SLOT_CONSOLE_EP;
        pm[n].rights = RIGHT_WRITE;        n++;

        r = svc_load_image_ws((uint64_t)size,
                              /*keep_stack_dest=*/((uint64_t)PROC_SLOT_CHILD_STACK << 32),
                              &child_h, &boot_h, pm, n, ws,
                              budget ? budget : (uint64_t)PROC_BUDGET_DEFAULT,
                              /*own_budget_slot=*/(uint32_t)IRIS_CPTR_OWN_UNTYPED,
                              /*keep_cnode_dest=*/0u,
                              ((uint64_t)PROC_SLOT_CHILD_TCB << 32),
                              /*keep_vspace_dest=*/0u);
    }
    if (r < 0) { *detail = r; return PROC_STEP_LOAD; }
    /* Arithmetic on the capability the spawn already returned — see
     * `svc_child_budget_slot`.  Nothing extra is kept for it. */
    g_child_budget_c     = svc_child_budget_slot(ws, (uint64_t)child_h);
    g_child_budget_bytes = budget ? budget : (uint64_t)PROC_BUDGET_DEFAULT;
    /*
     * And the loader's OWN copy of the child's thread goes, now.
     *
     * The loader returns it in a workspace leaf and does not close it, which is
     * right for a caller that has nowhere else to put it — but this service
     * mints its own at `PROC_SLOT_CHILD_TCB` and hands THAT on, so the leaf is
     * a third reference nobody needs.  It is not harmless: while it exists the
     * TCB exists, the TCB is charged to the child's budget, and `Untyped_Reset`
     * on that budget answers BUSY for ever.  A spawner whose children can
     * never be reclaimed is the whole failure this step exists to rule out.
     */
    (void)iris_invoke1((long)((uint32_t)child_h & 0xFFu), INV_CNODE_DELETE,
                       (long)((uint32_t)child_h >> 8));

    /*
     * Where the loader put it.  Asked rather than assumed: the image is loaded
     * at a bias drawn from RDTSC, and the one thing that knows it is the
     * thread's own entry register.  `bias = rip - e_entry` is exact, and it
     * costs one syscall instead of a second return value out of the loader.
     */
    if (iris_invoke1((long)PROC_SLOT_CHILD_TCB, INV_TCB_READ_REGS,
                     (long)(uintptr_t)&ctx) != 0)
        return PROC_STEP_START;
    if (ctx.rip < img.entry) return PROC_STEP_START;
    bias = ctx.rip - img.entry;

    /* The stack, mapped here so it can be written, at the same size the loader
     * mapped it there. */
    if (iris_map_frame(PROC_SLOT_CHILD_STACK, IRIS_CPTR_OWN_VSPACE,
                       IRIS_CPTR_OWN_UNTYPED, PROC_SLOT_PT,
                       PROC_VA_STACK, SVC_STACK_MAPPED, 1ull) != 0)
        return PROC_STEP_STACK;

    {
        /*
         * Sixteen bytes a runtime seeds its stack guard from.
         *
         * They go at the BOTTOM of the child's stack — the deepest address it
         * owns — because `prog_stack_build` fills from the top down and this
         * is the one place in the region it provably never reaches.  A program
         * that consumed its entire 32 KiB to get back here has already hit the
         * guard page one page lower, and by then every runtime that wanted
         * these bytes has copied them.
         */
        uint64_t rnd_child = SVC_STACK_MAP_BASE + 16u;
        uint8_t *rnd = (uint8_t *)(uintptr_t)(PROC_VA_STACK + 16u);
        uint64_t a, d;
        __asm__ volatile ("rdtsc" : "=a"(a), "=d"(d));
        for (uint32_t i = 0; i < 16u; i++) {
            a = a * 6364136223846793005ULL + 1442695040888963407ULL;
            rnd[i] = (uint8_t)((a >> 33) ^ d);
        }

        const struct prog_auxv aux[] = {
            { AT_PHDR,          bias + img.phdr_vaddr },
            { AT_PHENT,         img.phentsize },
            { AT_PHNUM,         img.phnum },
            { AT_PAGESZ,        4096u },
            { AT_BASE,          0u },          /* no interpreter yet: step 4 */
            { AT_ENTRY,         bias + img.entry },
            { AT_RANDOM,        rnd_child },
            { AT_IRIS_OBJC,     0u },          /* an empty object table...   */
            { AT_IRIS_OBJV,     IRIS_PROG_SLOT_OBJ_BASE },  /* ...that is still where
                                                             * the contract says */
            { AT_IRIS_UNTYPED,  IRIS_CPTR_OWN_UNTYPED },
        };
        const char *argv[1] = { path };
        rsp = prog_stack_build((void *)(uintptr_t)PROC_VA_STACK,
                               SVC_STACK_MAP_BASE, SVC_STACK_MAPPED,
                               argv, 1u, 0, 0u,
                               aux, (uint32_t)(sizeof(aux) / sizeof(aux[0])));
    }

    (void)iris_invoke2((long)PROC_SLOT_CHILD_STACK, INV_FRAME_UNMAP,
                       (long)IRIS_CPTR_OWN_VSPACE, (long)PROC_VA_STACK);
    proc_slot_delete(PROC_SLOT_CHILD_STACK);
    if (rsp == 0u) return PROC_STEP_STACK;

    /* Same entry, the stack we just built, and go.  `%rdi` is 0: a program's
     * first argument is its stack pointer and its own entry stub reads it from
     * `%rsp` — see services/hello/entry.S. */
    if (iris_invoke((long)PROC_SLOT_CHILD_TCB, INV_TCB_WRITE_REGS,
                    (long)ctx.rip, (long)rsp, 0) != 0)
        return PROC_STEP_START;
    if (iris_invoke0((long)PROC_SLOT_CHILD_TCB, INV_TCB_RESUME) != 0)
        return PROC_STEP_START;

    g_started++;
    return PROC_STEP_RUNNING;
}

/* ── the service loop ────────────────────────────────────────────────────── */

void proc_main(iris_cptr_t bootstrap_ch_h);
void proc_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    g_buf = (uint8_t *)iris_ipc_buffer_init(PROC_SLOT_IPCBUF,
                                            PROC_SLOT_IPCBUF_PT,
                                            IRIS_IPC_BUFFER_VA);

    for (;;) {
        struct iris_msg m, rep;
        proc_msg_zero(&m);
        m.reply = (long)PROC_SLOT_REPLY;
        if (iris_msg_recv((long)PROC_SLOT_CTRL_EP, &m) != 0) continue;

        proc_msg_zero(&rep);
        rep.label = PROC_REP_ERR;

        if (m.label == PROC_OP_INFO) {
            rep.label      = PROC_REP_OK;
            rep.words[0]   = g_spawns;
            rep.words[1]   = g_started;
            rep.words[2]   = g_last_step;
            rep.word_count = 3u;
        } else if (m.label == PROC_OP_REAP) {
            if (g_child_budget_c == 0u) {
                rep.words[0]   = (uint64_t)(int64_t)IRIS_ERR_NOT_FOUND;
                rep.word_count = 1u;
            } else {
                /*
                 * Our OWN copy of the child's thread goes first.
                 *
                 * It was handed on in the spawn reply, so this service has no
                 * further use for it — and while it exists the TCB exists, the
                 * TCB is charged to the region, and the reset is right to
                 * refuse.  A spawner that forgot this would report BUSY for
                 * ever and blame the caller.
                 */
                proc_slot_delete(PROC_SLOT_CHILD_TCB);
                long rr = iris_invoke0((long)g_child_budget_c, INV_UNTYPED_RESET);
                if (rr != 0) {
                    rep.words[0]   = (uint64_t)rr;
                    rep.word_count = 1u;
                } else {
                    uint64_t phys = 0, avail = 0;
                    (void)iris_invoke2((long)g_child_budget_c, INV_UNTYPED_INFO,
                                       (long)(uintptr_t)&phys,
                                       (long)(uintptr_t)&avail);
                    rep.label      = PROC_REP_OK;
                    rep.words[0]   = g_child_budget_bytes;
                    rep.words[1]   = avail;
                    rep.word_count = 2u;
                }
            }
        } else if (m.label == PROC_OP_SPAWN) {
            char path[VFS_EP_PATH_MAX];
            uint32_t n = m.buf_len, i;
            if (!g_buf || n == 0u || n > VFS_EP_PATH_MAX) {
                rep.words[0]   = PROC_STEP_PATH;
                rep.words[1]   = g_buf ? PROC_WHY_SIZE : PROC_WHY_NOBUF;
                rep.word_count = 2u;
            } else {
                for (i = 0; i < n; i++) path[i] = (char)g_buf[i];
                path[n - 1u] = '\0';
                uint32_t why = PROC_WHY_NONE;
                long detail = 0;
                g_spawns++;
                g_last_step = proc_spawn(path, m.words[0], &why, &detail);
                rep.words[0]   = g_last_step;
                rep.words[1]   = why;
                /* Whatever the kernel said, verbatim.  A step says WHERE and a
                 * why says WHICH; only the error itself says what the object
                 * layer refused, and folding it away is how a spawn failure
                 * becomes a guess. */
                rep.words[2]   = (uint64_t)detail;
                rep.word_count = 3u;
                if (g_last_step == PROC_STEP_RUNNING) {
                    /*
                     * The child's THREAD, which is the whole of what a
                     * supervisor needs: `TCB_Watch` for the death,
                     * `TCB_ExitCode` for what it exited with.  There is no
                     * process object to hand over and has not been since
                     * Stage 7.
                     */
                    rep.label      = PROC_REP_OK;
                    rep.cap        = (long)PROC_SLOT_CHILD_TCB;
                    rep.cap_rights = RIGHT_READ | RIGHT_WRITE;
                }
            }
        }

        if (iris_msg_reply((long)PROC_SLOT_REPLY, &rep) != 0) {
            /*
             * The one thing worth a line every time.
             *
             * A reply that does not land leaves the caller blocked for ever on
             * an answer that was computed and thrown away — and on the send
             * side it is silent, because the loop simply goes back to
             * receiving.  Everything else this service can get wrong reaches
             * the caller AS the answer; this is the case that cannot.
             */
            proc_log("[PROC] reply FAILED\n");
        }
    }
}
