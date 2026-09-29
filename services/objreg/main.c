/* SPDX-License-Identifier: Apache-2.0 */
/*
 * objreg/main.c — the shared-object registry.
 *
 * `iris/objreg_ep_proto.h` has the protocol and the reasoning about what is
 * shared and what is not.  This file is the service.
 *
 * ── What it actually does, in one paragraph ────────────────────────────────
 *
 * It reads an ELF through the VFS once, retypes ONE frame for its executable
 * segment and ONE for its data segment, copies the file's bytes into them, and
 * then never reads the file again.  Every consumer gets a derivation CHILD of
 * those two capabilities: read-only, so the text they map is the same physical
 * memory and the data they get is a master they must copy before writing.
 *
 * The saving is the point, and it is a real number: N processes running a
 * library cost one copy of its text instead of N.  `OBJREG_OP_INFO` reports it
 * as "bytes of text held", which is the memory this service exists to not
 * duplicate.
 *
 * ── Why the segments are separate frames ───────────────────────────────────
 *
 * Because W^X is enforced on the MAP and the two segments want different
 * flags, and because the whole design turns on text being shareable while data
 * is not.  One frame holding both would be a frame that is either writable —
 * and then its text is writable — or not, and then its data cannot be copied
 * out per process without also handing over the text.  Two objects, two
 * lifetimes, two rights.
 */
#include <stdint.h>
#include "../common/iris_msg.h"
#include "../common/iris_map.h"
#include "../common/iris_ipc_buffer.h"
#include "../common/elf64.h"
#include "../common/console_client.h"
#include <iris/syscall.h>
#include <iris/invoke.h>
#include <iris/nc/cptr.h>
#include <iris/nc/rights.h>
#include <iris/nc/error.h>
#include <iris/ipc_msg.h>
#include <iris/endpoint_proto.h>
#include <iris/vfs_ep_proto.h>
#include <iris/objreg_ep_proto.h>

/* ── this service's own slots ────────────────────────────────────────────── */
#define OBJ_SLOT_PT          32u
#define OBJ_SLOT_IPCBUF      33u
#define OBJ_SLOT_IPCBUF_PT   34u
#define OBJ_SLOT_FILE        35u   /* the raw image, while it is being read  */
#define OBJ_SLOT_FILEPOOL    36u   /* reset between loads; the file is scratch */
/* The masters.  Two slots per object, and they are the objects: everything a
 * consumer ever gets is derived from one of these. */
#define OBJ_SLOT_TEXT(i)     (40u + 2u * (i))
#define OBJ_SLOT_DATA(i)     (41u + 2u * (i))

/* ── where it maps, in its own address space ─────────────────────────────── */
#define OBJ_VA_FILE   0x80F0000000ULL
#define OBJ_VA_SEG    0x80F4000000ULL   /* one segment at a time, while filling */

#define OBJ_FILE_MAX  (1u << 20)
#define OBJ_POOL_BYTES (2u << 20)

struct obj_entry {
    char     name[VFS_EP_PATH_MAX];
    uint64_t text_bytes, data_bytes;
    /* Where the IMAGE says its segments and its entry point are, unbiased.  A
     * consumer maps at a base of its own and needs these to know where within
     * it each piece belongs; the registry has no opinion about that base. */
    uint64_t text_vaddr, data_vaddr, entry;
    uint32_t used;
};

static struct obj_entry g_obj[OBJREG_MAX_OBJECTS];
static uint32_t g_nobj;
static uint8_t *g_buf;

/* What it says out loud: one line per object it takes in, because the memory a
 * registry holds is memory nothing else can account for, and a boot that
 * silently loaded nothing looks exactly like one that silently loaded four. */
static void obj_log(const char *s) {
    if (g_buf) (void)console_ep_write((iris_cptr_t)OBJREG_SLOT_CONSOLE_EP, g_buf, s);
}

static void obj_msg_zero(struct iris_msg *m) {
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < (uint32_t)sizeof(*m); i++) b[i] = 0;
}
static void obj_slot_delete(uint32_t slot) {
    (void)iris_invoke1(0, INV_CNODE_DELETE, (long)slot);
}
static uint32_t obj_strlen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int obj_streq(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* The same re-staging discipline `proc` uses, for the same reason: the request
 * payload and the reply payload are one registered buffer. */
static long obj_vfs(uint64_t label, const char *path, uint64_t w0, uint64_t w1,
                    uint32_t wc, struct iris_msg *out) {
    uint32_t plen = obj_strlen(path);
    if (plen + 1u > VFS_EP_PATH_MAX) return (long)IRIS_ERR_INVALID_ARG;
    if (!g_buf) return (long)IRIS_ERR_NOT_SUPPORTED;

    obj_msg_zero(out);
    out->label      = label;
    out->words[0]   = w0;
    out->words[1]   = w1;
    out->word_count = wc;
    for (uint32_t i = 0; i < plen; i++) g_buf[i] = (uint8_t)path[i];
    g_buf[plen] = 0u;
    out->buf_len = plen + 1u;

    long r = iris_msg_call((long)OBJREG_SLOT_VFS_EP, out);
    if (r != 0) return r;
    if (out->label != IRIS_EP_REPLY_OK) return (long)IRIS_ERR_NOT_FOUND;
    return 0;
}

/* Read the whole file into a scratch frame mapped at OBJ_VA_FILE.  Returns its
 * size, or a negative error.  The frame is the caller's to unmap and delete. */
static long obj_read_file(const char *path) {
    struct iris_msg m;
    uint64_t size, off, pages;

    if (obj_vfs(VFS_EP_OP_STAT, path, 0, 0, 0u, &m) != 0) return (long)IRIS_ERR_NOT_FOUND;
    size = m.words[1];
    if (size < sizeof(Elf64_Ehdr) || size > OBJ_FILE_MAX) return (long)IRIS_ERR_INVALID_ARG;

    {
        long pr = iris_invoke0((long)OBJ_SLOT_FILEPOOL, INV_UNTYPED_RESET);
        if (pr == (long)IRIS_ERR_BUSY) return (long)IRIS_ERR_BUSY;
        if (pr != 0 &&
            iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                        (long)((uint64_t)IRIS_KOBJ_UNTYPED | (1ULL << 32)),
                        (long)((uint64_t)OBJ_SLOT_FILEPOOL << 32),
                        (long)OBJ_POOL_BYTES) != 0)
            return (long)IRIS_ERR_NO_MEMORY;
    }

    pages = (size + 0xFFFULL) & ~0xFFFULL;
    obj_slot_delete(OBJ_SLOT_FILE);
    if (iris_invoke((long)OBJ_SLOT_FILEPOOL, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)OBJ_SLOT_FILE << 32), (long)pages) != 0)
        return (long)IRIS_ERR_NO_MEMORY;
    if (iris_map_frame(OBJ_SLOT_FILE, IRIS_CPTR_OWN_VSPACE, IRIS_CPTR_OWN_UNTYPED,
                       OBJ_SLOT_PT, OBJ_VA_FILE, pages, 1ull) != 0)
        return (long)IRIS_ERR_NO_MEMORY;

    for (off = 0; off < size; ) {
        uint8_t *dst = (uint8_t *)(uintptr_t)(OBJ_VA_FILE + off);
        uint64_t n;
        if (obj_vfs(VFS_EP_OP_READ_AT, path, off, VFS_EP_DATA_MAX, 2u, &m) != 0)
            return (long)IRIS_ERR_NOT_FOUND;
        n = m.words[1];
        if (n == 0u || n > size - off || m.buf_len != (uint32_t)n)
            return (long)IRIS_ERR_INVALID_ARG;
        for (uint64_t i = 0; i < n; i++) dst[i] = g_buf[i];
        off += n;
    }
    return (long)size;
}

/*
 * Carve one segment into a frame of its own and fill it.
 *
 * Filled through a WRITABLE mapping in this service's address space and then
 * unmapped, which is why the text a consumer maps read+execute is never
 * writable anywhere once this returns: the only writable mapping that ever
 * existed was here, and it is gone.
 */
static int obj_load_segment(const Elf64_Phdr *ph, uint32_t dest_slot,
                            uint64_t *out_bytes, uint64_t *out_vaddr) {
    uint64_t base  = ph->p_vaddr & ~0xFFFULL;
    uint64_t pgoff = ph->p_vaddr - base;
    uint64_t bytes = (pgoff + ph->p_memsz + 0xFFFULL) & ~0xFFFULL;

    if (bytes == 0u || bytes > OBJ_FILE_MAX) return 0;
    obj_slot_delete(dest_slot);
    if (iris_invoke((long)IRIS_CPTR_OWN_UNTYPED, INV_UNTYPED_RETYPE,
                    (long)((uint64_t)IRIS_KOBJ_FRAME | (1ULL << 32)),
                    (long)((uint64_t)dest_slot << 32), (long)bytes) != 0)
        return 0;
    if (iris_map_frame(dest_slot, IRIS_CPTR_OWN_VSPACE, IRIS_CPTR_OWN_UNTYPED,
                       OBJ_SLOT_PT, OBJ_VA_SEG, bytes, 1ull) != 0) {
        obj_slot_delete(dest_slot);
        return 0;
    }
    {
        uint8_t *dst = (uint8_t *)(uintptr_t)OBJ_VA_SEG;
        const uint8_t *src = (const uint8_t *)(uintptr_t)(OBJ_VA_FILE + ph->p_offset);
        uint64_t i;
        for (i = 0; i < pgoff; i++) dst[i] = 0u;
        for (i = 0; i < ph->p_filesz; i++) dst[pgoff + i] = src[i];
        /* `p_memsz` past `p_filesz` is .bss and is ZERO — a frame comes back
         * zeroed only if something zeroed it, and "only if" is not a thing to
         * rely on for the segment a library's uninitialised data lives in. */
        for (i = pgoff + ph->p_filesz; i < bytes; i++) dst[i] = 0u;
    }
    (void)iris_invoke2((long)dest_slot, INV_FRAME_UNMAP,
                       (long)IRIS_CPTR_OWN_VSPACE, (long)OBJ_VA_SEG);
    *out_bytes = bytes;
    /* The PAGE the segment starts in, not `p_vaddr`: the frame covers whole
     * pages and a consumer maps the frame, so the address it maps at is the
     * page base or the mapping is off by `p_vaddr & 0xFFF`. */
    *out_vaddr = base;
    return 1;
}

/* Load an object by path, or answer the id it already has.  Returns the id, or
 * a negative error. */
static long obj_open(const char *path) {
    long size;
    uint32_t id;
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *phs;
    const Elf64_Phdr *text = 0, *data = 0;

    for (id = 0; id < g_nobj; id++)
        if (g_obj[id].used && obj_streq(g_obj[id].name, path))
            return (long)id;              /* loaded once, and only once */
    if (g_nobj >= OBJREG_MAX_OBJECTS) return (long)IRIS_ERR_TABLE_FULL;
    id = g_nobj;

    size = obj_read_file(path);
    if (size < 0) goto fail;

    eh = (const Elf64_Ehdr *)(uintptr_t)OBJ_VA_FILE;
    size = (long)IRIS_ERR_INVALID_ARG;
    {
        const Elf64_Ehdr *e = eh;
        uint64_t n = (uint64_t)e->e_phnum * (uint64_t)e->e_phentsize;
        if (e->e_ident[0] != 0x7f || e->e_ident[1] != 'E' ||
            e->e_ident[2] != 'L'  || e->e_ident[3] != 'F') goto fail;
        if (e->e_type != (Elf64_Half)ET_DYN)                goto fail;
        if (e->e_phnum == 0 ||
            e->e_phentsize < (Elf64_Half)sizeof(Elf64_Phdr)) goto fail;
        if (e->e_phoff > OBJ_FILE_MAX || n > OBJ_FILE_MAX)   goto fail;
        phs = (const Elf64_Phdr *)(uintptr_t)(OBJ_VA_FILE + e->e_phoff);
        for (uint32_t i = 0; i < e->e_phnum; i++) {
            const Elf64_Phdr *p = (const Elf64_Phdr *)(uintptr_t)
                                     ((uintptr_t)phs + i * e->e_phentsize);
            if (p->p_type != PT_LOAD || p->p_memsz == 0u) continue;
            if ((p->p_flags & PF_X) && !text) text = p;
            else if ((p->p_flags & PF_W) && !data) data = p;
        }
    }
    /* Both, or neither.  An object with no text is not a library, and one with
     * no data would make every consumer's copy step conditional — which is a
     * branch nobody would exercise until the day it mattered. */
    if (!text || !data) { size = (long)IRIS_ERR_INVALID_ARG; goto fail; }

    if (!obj_load_segment(text, OBJ_SLOT_TEXT(id), &g_obj[id].text_bytes,
                          &g_obj[id].text_vaddr)) {
        size = (long)IRIS_ERR_NO_MEMORY; goto fail;
    }
    if (!obj_load_segment(data, OBJ_SLOT_DATA(id), &g_obj[id].data_bytes,
                          &g_obj[id].data_vaddr)) {
        obj_slot_delete(OBJ_SLOT_TEXT(id));
        size = (long)IRIS_ERR_NO_MEMORY; goto fail;
    }
    g_obj[id].entry = eh->e_entry;

    {
        uint32_t i = 0;
        for (; i + 1u < VFS_EP_PATH_MAX && path[i]; i++) g_obj[id].name[i] = path[i];
        for (; i < VFS_EP_PATH_MAX; i++) g_obj[id].name[i] = '\0';
    }
    g_obj[id].used = 1u;
    g_nobj = id + 1u;
    size = (long)id;
    obj_log("[OBJREG] object loaded\n");

fail:
    /* The file is scratch and goes either way: what the registry keeps is the
     * SEGMENTS, and a raw image left mapped would be a second copy of exactly
     * the bytes this service exists to hold once. */
    (void)iris_invoke2((long)OBJ_SLOT_FILE, INV_FRAME_UNMAP,
                       (long)IRIS_CPTR_OWN_VSPACE, (long)OBJ_VA_FILE);
    obj_slot_delete(OBJ_SLOT_FILE);
    return size;
}

/* ── the service loop ────────────────────────────────────────────────────── */

void objreg_main(iris_cptr_t bootstrap_ch_h);
void objreg_main(iris_cptr_t bootstrap_ch_h) {
    (void)bootstrap_ch_h;

    g_buf = (uint8_t *)iris_ipc_buffer_init(OBJ_SLOT_IPCBUF, OBJ_SLOT_IPCBUF_PT,
                                            IRIS_IPC_BUFFER_VA);
    obj_log("[OBJREG] ready\n");

    for (;;) {
        struct iris_msg m, rep;
        obj_msg_zero(&m);
        m.reply = (long)OBJREG_SLOT_REPLY;
        if (iris_msg_recv((long)OBJREG_SLOT_CTRL_EP, &m) != 0) continue;

        obj_msg_zero(&rep);
        rep.label = OBJREG_REP_ERR;

        if (m.label == OBJREG_OP_OPEN) {
            char path[VFS_EP_PATH_MAX];
            uint32_t n = m.buf_len;
            if (!g_buf || n == 0u || n > VFS_EP_PATH_MAX) {
                rep.words[0] = (uint64_t)(int64_t)IRIS_ERR_INVALID_ARG;
                rep.word_count = 1u;
            } else {
                long id;
                for (uint32_t i = 0; i < n; i++) path[i] = (char)g_buf[i];
                path[n - 1u] = '\0';
                id = obj_open(path);
                if (id < 0) {
                    rep.words[0]   = (uint64_t)id;
                    rep.word_count = 1u;
                } else {
                    rep.label      = OBJREG_REP_OK;
                    rep.words[0]   = (uint64_t)id;
                    rep.words[1]   = g_obj[id].text_bytes;
                    rep.words[2]   = g_obj[id].data_bytes;
                    rep.word_count = 3u;
                }
            }
        } else if (m.label == OBJREG_OP_TEXT || m.label == OBJREG_OP_DATA) {
            uint64_t id = m.words[0];
            if (id >= g_nobj || !g_obj[id].used) {
                rep.words[0]   = (uint64_t)(int64_t)IRIS_ERR_NOT_FOUND;
                rep.word_count = 1u;
            } else {
                int is_text = (m.label == OBJREG_OP_TEXT);
                rep.label      = OBJREG_REP_OK;
                rep.words[0]   = is_text ? g_obj[id].text_bytes
                                         : g_obj[id].data_bytes;
                rep.word_count = 1u;
                rep.cap        = (long)(is_text ? OBJ_SLOT_TEXT(id)
                                                : OBJ_SLOT_DATA(id));
                /*
                 * READ, and the two rights a DELEGATE needs — never WRITE.
                 *
                 * No WRITE is the one that matters and it is absolute: the
                 * holder cannot alter a library every other process is
                 * running, and cannot obtain the right by any means, because a
                 * mint reduces and never widens.
                 *
                 * DUPLICATE and TRANSFER are here because of who actually
                 * calls this.  It is not usually the program that will map the
                 * library — it is `proc`, resolving a child's object set
                 * BEFORE the child exists, and a capability it cannot mint is
                 * a capability it cannot give to the child it is building.
                 * `kcnode_slot_derive` requires DUPLICATE on the source, so
                 * withholding it does not make the registry safer; it makes
                 * the registry useless.
                 *
                 * What keeps this honest is not withholding rights from the
                 * delegate, it is the derivation tree: every copy anybody makes
                 * is a descendant of THIS slot, so `OBJREG_OP_REVOKE` reaches
                 * all of them however far they were passed.  The spawner
                 * reduces to plain READ when it mints into a program, so a
                 * program is a leaf and cannot pass the library on at all.
                 */
                rep.cap_rights = RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER;
            }
        } else if (m.label == OBJREG_OP_LAYOUT) {
            uint64_t id = m.words[0];
            if (id >= g_nobj || !g_obj[id].used) {
                rep.words[0]   = (uint64_t)(int64_t)IRIS_ERR_NOT_FOUND;
                rep.word_count = 1u;
            } else {
                rep.label      = OBJREG_REP_OK;
                rep.words[0]   = g_obj[id].text_vaddr;
                rep.words[1]   = g_obj[id].data_vaddr;
                rep.words[2]   = g_obj[id].entry;
                rep.word_count = 3u;
            }
        } else if (m.label == OBJREG_OP_REVOKE) {
            uint64_t id = m.words[0];
            if (id >= g_nobj || !g_obj[id].used) {
                rep.words[0]   = (uint64_t)(int64_t)IRIS_ERR_NOT_FOUND;
                rep.word_count = 1u;
            } else {
                /* Both masters.  Revoking only the text would leave every
                 * consumer holding a data master for an object that no longer
                 * exists, which is a half-withdrawn library and worse than
                 * either whole state. */
                /* The COUNT comes back, not a status: `CSpace_Revoke` answers
                 * how many capabilities it destroyed, and zero is a perfectly
                 * good answer for an object nobody had taken yet.  Only a
                 * negative is a failure. */
                long a = iris_invoke0((long)OBJ_SLOT_TEXT(id), INV_CSPACE_REVOKE);
                long b = iris_invoke0((long)OBJ_SLOT_DATA(id), INV_CSPACE_REVOKE);
                if (a >= 0 && b >= 0) {
                    rep.label      = OBJREG_REP_OK;
                    rep.words[0]   = (uint64_t)a;
                    rep.words[1]   = (uint64_t)b;
                    rep.word_count = 2u;
                } else {
                    rep.words[0]   = (uint64_t)(a < 0 ? a : b);
                    rep.word_count = 1u;
                }
            }
        } else if (m.label == OBJREG_OP_INFO) {
            uint64_t held = 0;
            for (uint32_t i = 0; i < g_nobj; i++)
                if (g_obj[i].used) held += g_obj[i].text_bytes;
            rep.label      = OBJREG_REP_OK;
            rep.words[0]   = g_nobj;
            rep.words[1]   = held;
            rep.word_count = 2u;
        }

        (void)iris_msg_reply((long)OBJREG_SLOT_REPLY, &rep);
    }
}
