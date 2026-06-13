/*
 * test_ipc_cspace.c — Fase 3.2 unit tests for IPC dual-resolve helpers.
 *
 * Covers cspace_or_handle_resolve_endpoint/reply/notification, verifying:
 *   - CSpace path resolves correct typed objects
 *   - Handle-table fallback resolves when no CSpace root is set
 *   - Wrong object type returns IRIS_ERR_WRONG_TYPE
 *   - Missing rights returns IRIS_ERR_ACCESS_DENIED
 *   - ACCESS_DENIED from CSpace does not fall through to handle table
 *   - Refcounts balance across repeated lookups
 *   - Legacy handle ABI continues to work
 */
#include "framework.h"
#include <iris/nc/kobject.h>
#include <iris/nc/kcnode.h>
#include <iris/nc/kendpoint.h>
#include <iris/nc/kreply.h>
#include <iris/nc/knotification.h>
#include <iris/nc/handle_table.h>
#include <iris/nc/kprocess.h>
#include <iris/nc/rights.h>
#include <iris/nc/cspace.h>
#include <iris/kpage.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static struct KProcess *make_proc(void) {
    struct KProcess *p = (struct KProcess *)kpage_alloc((uint32_t)sizeof(struct KProcess));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    handle_table_init(&p->handle_table);
    p->cspace_root_h = HANDLE_INVALID;
    return p;
}

static void free_proc(struct KProcess *p) {
    handle_table_close_all(&p->handle_table);
    kpage_free(p, (uint32_t)sizeof(*p));
}

/* Install a CSpace root with num_slots slots and return the root CNode. */
static struct KCNode *setup_cspace(struct KProcess *p, uint32_t num_slots) {
    struct KCNode *root = kcnode_alloc(num_slots);
    if (!root) return NULL;
    handle_id_t rh = handle_table_insert(&p->handle_table, &root->base,
                                          RIGHT_READ | RIGHT_WRITE);
    kobject_release(&root->base);
    if (rh == HANDLE_INVALID) return NULL;
    p->cspace_root_h = rh;
    return root;
}

/* ── Endpoint helpers ─────────────────────────────────────────────────── */

void test_ipc_cspace(void) {
    TEST_SUITE("ipc_cspace");

    /* ── [EP] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 3, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, 3u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_ENDPOINT);
        ASSERT_EQ(rout, RIGHT_READ | RIGHT_WRITE);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [EP] Handle fallback: no CSpace root → legacy handle ABI works ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        /* cspace_root_h stays HANDLE_INVALID */

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base,
                                             RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER);
        kobject_release(&ep->base);
        ASSERT_NE(h, (handle_id_t)HANDLE_INVALID);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, (iris_cptr_t)h,
                                                     RIGHT_READ, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_ENDPOINT);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [EP] Wrong object type in CSpace → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        /* Mint a KNotification where an endpoint is expected. */
        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        ASSERT_EQ(kcnode_mint(root, 2, &n->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&n->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, 2u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }

    /* ── [EP] Wrong object type in handle table → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        handle_id_t h = handle_table_insert(&p->handle_table, &n->base, RIGHT_READ);
        kobject_release(&n->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, (iris_cptr_t)h,
                                                     RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }

    /* ── [EP] Missing rights in CSpace → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 4, &ep->base, RIGHT_READ), IRIS_OK);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, 4u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);
        /* Correct right succeeds. */
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, 4u, RIGHT_READ, &out, &rout),
                  IRIS_OK);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [EP] Missing rights in handle fallback → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base, RIGHT_READ);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, (iris_cptr_t)h,
                                                     RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        free_proc(p);
    }

    /* ── [EP] ACCESS_DENIED from CSpace does NOT fall back to handle table ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        /* Mint endpoint with READ-only into slot 1. */
        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 1, &ep->base, RIGHT_READ), IRIS_OK);
        kobject_release(&ep->base);

        /* Also insert the SAME endpoint into handle table with WRITE rights.
         * If fallback occurred on ACCESS_DENIED, this handle would succeed. */
        kobject_retain(&ep->base);   /* lend a ref for the second insert */
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&ep->base);

        struct KEndpoint *out; iris_rights_t rout;
        /* cptr=1 resolves via CSpace → ACCESS_DENIED (READ-only, requesting WRITE).
         * Must NOT fall back to handle table (which has WRITE). */
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, 1u, RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        (void)h;
        free_proc(p);
    }

    /* ── [EP] Repeated resolve: refcount balance ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&ep->base);

        for (int i = 0; i < 5; i++) {
            struct KEndpoint *out; iris_rights_t rout;
            ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, (iris_cptr_t)h,
                                                         RIGHT_NONE, &out, &rout), IRIS_OK);
            kobject_release(&out->base);
        }
        /* Object still alive: handle_table_get_object must still find it. */
        struct KObject *obj; iris_rights_t r;
        ASSERT_EQ(handle_table_get_object(&p->handle_table, h, &obj, &r), IRIS_OK);
        ASSERT_EQ(obj->type, KOBJ_ENDPOINT);
        kobject_release(obj);

        free_proc(p);
    }

    /* ── [Reply] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KReply *rp = kreply_alloc(NULL);
        ASSERT_NOT_NULL(rp);
        ASSERT_EQ(kcnode_mint(root, 5, &rp->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&rp->base);

        struct KReply *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_reply(p, 5u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_REPLY);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [Reply] Handle fallback: legacy ABI works ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KReply *rp = kreply_alloc(NULL);
        ASSERT_NOT_NULL(rp);
        handle_id_t h = handle_table_insert(&p->handle_table, &rp->base,
                                             RIGHT_READ | RIGHT_WRITE);
        kobject_release(&rp->base);

        struct KReply *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_reply(p, (iris_cptr_t)h,
                                                  RIGHT_WRITE, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_REPLY);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [Reply] Wrong type → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        ASSERT_EQ(kcnode_mint(root, 2, &ep->base, RIGHT_READ | RIGHT_WRITE), IRIS_OK);
        kobject_release(&ep->base);

        struct KReply *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_reply(p, 2u, RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }

    /* ── [Reply] Missing rights → ACCESS_DENIED ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KReply *rp = kreply_alloc(NULL);
        ASSERT_NOT_NULL(rp);
        handle_id_t h = handle_table_insert(&p->handle_table, &rp->base, RIGHT_READ);
        kobject_release(&rp->base);

        struct KReply *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_reply(p, (iris_cptr_t)h,
                                                  RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        free_proc(p);
    }

    /* ── [Notification] CSpace path: typed resolve OK ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        ASSERT_EQ(kcnode_mint(root, 6, &n->base,
                               RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT), IRIS_OK);
        kobject_release(&n->base);

        struct KNotification *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_notification(p, 6u, RIGHT_NONE, &out, &rout),
                  IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_NOTIFICATION);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [Notification] Handle fallback: legacy ABI works ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        handle_id_t h = handle_table_insert(&p->handle_table, &n->base,
                                             RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT);
        kobject_release(&n->base);

        struct KNotification *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_notification(p, (iris_cptr_t)h,
                                                         RIGHT_WAIT, &out, &rout), IRIS_OK);
        ASSERT_EQ(out->base.type, KOBJ_NOTIFICATION);
        kobject_release(&out->base);

        free_proc(p);
    }

    /* ── [Notification] Wrong type → WRONG_TYPE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base, RIGHT_READ);
        kobject_release(&ep->base);

        struct KNotification *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_notification(p, (iris_cptr_t)h,
                                                         RIGHT_NONE, &out, &rout),
                  IRIS_ERR_WRONG_TYPE);

        free_proc(p);
    }

    /* ── [Notification] Signal requires RIGHT_WRITE ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        handle_id_t h = handle_table_insert(&p->handle_table, &n->base, RIGHT_READ);
        kobject_release(&n->base);

        struct KNotification *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_notification(p, (iris_cptr_t)h,
                                                         RIGHT_WRITE, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        free_proc(p);
    }

    /* ── [Notification] Wait requires RIGHT_WAIT ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        handle_id_t h = handle_table_insert(&p->handle_table, &n->base, RIGHT_WRITE);
        kobject_release(&n->base);

        struct KNotification *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_notification(p, (iris_cptr_t)h,
                                                         RIGHT_WAIT, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        free_proc(p);
    }

    /* ── [Notification] ACCESS_DENIED from CSpace does NOT fallback ── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        /* CSpace slot: READ-only. Handle table: full rights. */
        ASSERT_EQ(kcnode_mint(root, 3, &n->base, RIGHT_READ), IRIS_OK);
        kobject_retain(&n->base);
        handle_id_t h = handle_table_insert(&p->handle_table, &n->base,
                                             RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT);
        kobject_release(&n->base);
        kobject_release(&n->base); /* drop extra retain */

        struct KNotification *out; iris_rights_t rout;
        /* cptr=3 → CSpace → ACCESS_DENIED (need WAIT, have READ-only). No fallback. */
        ASSERT_EQ(cspace_or_handle_resolve_notification(p, 3u, RIGHT_WAIT, &out, &rout),
                  IRIS_ERR_ACCESS_DENIED);

        (void)h;
        free_proc(p);
    }

    /* ── [Fase 8] namespace split: handle values never alias CSpace slots ──
     * Regression for the Fase-8 boot bug: cspace_resolve_cap's radix walk
     * masks the index (cptr & slot_count-1), so a HANDLE like 0x403 (1027)
     * used to land on root slot 3 once low slots were populated.  The dual
     * resolvers must treat >= 1024 as handle-table-only. */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        /* Occupy slot 3 with a WRONG-TYPE object (notification). */
        struct KNotification *n = knotification_alloc();
        ASSERT_NOT_NULL(n);
        ASSERT_EQ(kcnode_mint(root, 3, &n->base, RIGHT_WAIT), IRIS_OK);
        kobject_release(&n->base);

        /* Insert an endpoint whose HANDLE id has low bits == 3.  Handle ids
         * are slot | gen<<10; force the table slot to 3 by filling 0..2. */
        struct KEndpoint *eps[4];
        handle_id_t hs[4];
        for (uint32_t i = 0; i < 4u; i++) {
            eps[i] = kendpoint_alloc();
            ASSERT_NOT_NULL(eps[i]);
            hs[i] = handle_table_insert(&p->handle_table, &eps[i]->base,
                                        RIGHT_READ | RIGHT_WRITE);
            kobject_release(&eps[i]->base);
        }
        /* hs[2] sits in table slot 3 (slot 0 = cspace root handle). */
        handle_id_t h_alias = hs[2];
        ASSERT_EQ((uint32_t)(h_alias & 0x3FFu), 3u);

        /* OLD bug: resolve walked CSpace, hit slot 3 (notification) and
         * returned WRONG_TYPE.  NEW: >= 1024 goes straight to the handle
         * table and resolves the REAL endpoint. */
        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, (iris_cptr_t)h_alias,
                                                    RIGHT_WRITE, &out, &rout),
                  IRIS_OK);
        ASSERT_TRUE(out == eps[2]);
        kobject_release(&out->base);

        /* And a true CPtr (< 1024) into an EMPTY slot must fail cleanly
         * WITHOUT falling back to the handle table. */
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, 5u, RIGHT_NONE,
                                                    &out, &rout),
                  IRIS_ERR_NOT_FOUND);

        free_proc(p);
    }

    /* ── [Fase 9] badges: per-cap identity ───────────────────────────── */
    {
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);
        struct KCNode *root = setup_cspace(p, 8);
        ASSERT_NOT_NULL(root);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);

        /* Two caps to the SAME endpoint with DIFFERENT badges. */
        ASSERT_EQ(kcnode_mint_excl_badged(root, 1, &ep->base, RIGHT_WRITE,
                                          0xAAu), IRIS_OK);
        ASSERT_EQ(kcnode_mint_excl_badged(root, 2, &ep->base, RIGHT_WRITE,
                                          0xBBu), IRIS_OK);
        /* Unbadged third cap. */
        ASSERT_EQ(kcnode_mint_excl(root, 3, &ep->base, RIGHT_WRITE), IRIS_OK);

        struct KEndpoint *out; iris_rights_t rout; uint64_t badge;
        badge = 99u;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint_badged(p, 1u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0xAAu);
        kobject_release(&out->base);

        badge = 99u;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint_badged(p, 2u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0xBBu);
        kobject_release(&out->base);

        badge = 99u;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint_badged(p, 3u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0u);                 /* unbadged cap delivers 0 */
        kobject_release(&out->base);

        /* swap preserves badges; delete clears only its own slot. */
        ASSERT_EQ(kcnode_swap(root, 1, 2), IRIS_OK);
        uint64_t b1 = 0, b2 = 0; struct KObject *o; iris_rights_t rr;
        ASSERT_EQ(kcnode_fetch_badged(root, 1, &o, &rr, &b1), IRIS_OK);
        kobject_active_release(o); kobject_release(o);
        ASSERT_EQ(kcnode_fetch_badged(root, 2, &o, &rr, &b2), IRIS_OK);
        kobject_active_release(o); kobject_release(o);
        ASSERT_EQ(b1, 0xBBu);
        ASSERT_EQ(b2, 0xAAu);
        ASSERT_EQ(kcnode_delete(root, 1), IRIS_OK);
        ASSERT_EQ(kcnode_fetch_badged(root, 2, &o, &rr, &b2), IRIS_OK);
        kobject_active_release(o); kobject_release(o);
        ASSERT_EQ(b2, 0xAAu);                 /* unaffected by delete */

        /* Handle namespace: badged handle resolves with its badge; a badged
         * cap in a slot does NOT leak its badge to handle lookups. */
        kobject_retain(&ep->base);
        handle_id_t hb = handle_table_insert_badged(&p->handle_table,
                                                    &ep->base,
                                                    RIGHT_WRITE, 0xCCu);
        kobject_release(&ep->base);
        ASSERT_TRUE(hb != HANDLE_INVALID);
        ASSERT_EQ(handle_table_get_badge(&p->handle_table, hb), 0xCCu);
        badge = 99u;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint_badged(p, (iris_cptr_t)hb,
                                                           RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_OK);
        ASSERT_EQ(badge, 0xCCu);
        kobject_release(&out->base);

        /* ACCESS_DENIED on a badged slot stays a hard stop, no badge leak. */
        ASSERT_EQ(kcnode_mint_excl_badged(root, 4, &ep->base, RIGHT_READ,
                                          0xDDu), IRIS_OK);
        badge = 99u;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint_badged(p, 4u, RIGHT_WRITE,
                                                           &out, &rout, &badge),
                  IRIS_ERR_ACCESS_DENIED);
        ASSERT_EQ(badge, 99u);                /* untouched on failure */

        kobject_release(&ep->base);           /* drop alloc ref */
        free_proc(p);
    }

    /* ── [EP] IPC lifecycle: active_refs NOT held after resolve ── */
    {
        /* Verifies that the IPC helpers release active_refs before returning.
         * After resolve + release: active_refs must be exactly what the handle
         * table had (1 from handle_entry_init, 0 after table close). */
        struct KProcess *p = make_proc();
        ASSERT_NOT_NULL(p);

        struct KEndpoint *ep = kendpoint_alloc();
        ASSERT_NOT_NULL(ep);
        /* After alloc: refcount=1, active_refs=0 */
        handle_id_t h = handle_table_insert(&p->handle_table, &ep->base,
                                             RIGHT_READ | RIGHT_WRITE);
        /* After insert: refcount=2, active_refs=1 (handle_entry_init does both) */
        kobject_release(&ep->base); /* drop alloc ref: refcount=1, active_refs=1 */

        struct KEndpoint *out; iris_rights_t rout;
        ASSERT_EQ(cspace_or_handle_resolve_endpoint(p, (iris_cptr_t)h,
                                                     RIGHT_NONE, &out, &rout), IRIS_OK);
        /* During resolve via handle path: refcount=2 (lifecycle retained).
         * active_refs must remain 1 — helper must NOT add active_retain.
         * Release lifecycle ref from resolve. */
        kobject_release(&out->base); /* refcount back to 1 */

        /* Close handle: active_refs drops to 0 → close fires (wakes would-be waiters).
         * refcount drops to 0 → destroy. This must not crash. */
        handle_table_close(&p->handle_table, h);

        kpage_free(p, (uint32_t)sizeof(*p));
    }
}
