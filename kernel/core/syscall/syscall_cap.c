/* SPDX-License-Identifier: Apache-2.0 */
#include "syscall_priv.h"



/* ── Handle duplication ───────────────────────────────────────────── */

/* ── Handle transfer ──────────────────────────────────────────────── */

/* sys_handle_transfer RETIRED — A1.8.  Zero in-tree callers survived the A1
 * arc: cross-process placement is SYS_PROC_CSPACE_MINT (CSpace-canonical,
 * badge-capable, fail-fast on occupied slots) or an IPC receive-slot.  The
 * dispatcher falls to default (NOT_SUPPORTED); syscall number 23 is
 * permanently reserved.  See docs/architecture/handle-table-freeze.md. */


/* ── Hardware capability creation (C2: policy moved to svcmgr) ──────── */


/* ── Phase S4 (Step 3 prep): CSpace-native device capabilities ────────────
 *
 * SYS_CAP_CREATE_IRQCAP / _IOPORT used to be handle producers: the only way
 * to hold a KIrqCap/KIoPort was a handle, which left the legacy handle tree
 * (SYS_CAP_DERIVE/SYS_CAP_REVOKE) as the ONLY derive+cascade-revoke mechanism
 * for device authority — the blocker that kept Stage 3 shut.
 *
 * They now publish into a CSpace slot, and — this is the point — as an MDB
 * CHILD of the bootstrap-cap SLOT that authorised the creation.  Device
 * authority therefore has a real CSpace ancestor: SYS_CSPACE_REVOKE on the
 * bootstrap cap recursively destroys every device cap issued under it, which
 * is seL4's IRQControl semantics.  The authority argument must consequently
 * be a CPtr — a handle cannot be an MDB parent (charter §3.6/A9).
 *
 * dev_cap_publish installs `obj` at `dest` parented to (auth_cn, auth_idx).
 * The caller keeps its own reference to obj and releases it afterwards.
 *
 * `dest` follows the RETYPE2 convention every other publishing syscall uses —
 * destination CNode in the low 32 bits (0 = the caller's own root), slot index
 * in the high 32.  It was a bare ROOT slot index, refused above 1024, which
 * meant a device capability could only ever land in the caller's root CNode:
 * a service holding its working capabilities in a second-level CNode — which
 * is what a spawner does, and what the root CSpace's 256 slots force — could
 * not receive one at all.  seL4's `seL4_IRQControl_Get` names the destination
 * CNode for the same reason.
 */
static iris_error_t dev_cap_publish(struct task *t, struct KObject *obj,
                                    iris_rights_t rights, uint64_t dest,
                                    struct KCNode *auth_cn, uint32_t auth_idx,
                                    struct KObject *auth_obj) {
    uint64_t dest_cnode = dest & 0xFFFFFFFFu;
    uint32_t dest_slot  = (uint32_t)(dest >> 32);
    if (dest_slot == 0u) return IRIS_ERR_INVALID_ARG;

    struct KCNode *cn = 0;
    iris_error_t err = (dest_cnode == 0u)
        ? cspace_own_root(t->cspace_root, &cn)
        : cspace_resolve_cnode_for_publish(t->cspace_root,
                                           (iris_cptr_t)dest_cnode, &cn);
    if (err != IRIS_OK) return err;

    err = kcnode_slot_install_linked(cn, dest_slot, obj, rights, 0,
                                     auth_cn, auth_idx,
                                     /*parent_expect*/auth_obj,
                                     /*exclusive*/1, /*legacy*/0);
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
    return err;
}

/* Resolve the control-capability CPtr to BOTH its object (to check WHICH
 * authority it is) and its slot (to become the MDB parent).  CSpace only.
 *
 * Stage 5 Step 2: `kind` is matched EXACTLY.  The predecessor accepted any
 * boot capability carrying IRIS_BOOTCAP_HW_ACCESS — one bit that authorised
 * both IRQ and ioport creation, on an object that also carried spawn, debug
 * and framebuffer authority.  A service that needed a serial port therefore
 * held the authority to claim any interrupt line, spawn processes and power
 * the machine off, and the only way to reduce that was to clone a narrowed
 * copy of the whole thing.  Now there are two capabilities and each authorises
 * exactly its own syscall. */
/* `out_ports` (optional) receives the authorising capability's port range,
 * read while the object is still held.  It is what an IOPORT_CONTROL check
 * measures against now that the kernel has no table of its own. */
static iris_error_t dev_cap_auth_ranged(struct task *t, uint64_t auth_cptr,
                                        uint32_t kind,
                                        struct KCNode **out_cn,
                                        uint32_t *out_idx,
                                        uint16_t out_ports[2],
                                        struct KObject **out_auth) {
    if (!cspace_only_cptr(auth_cptr)) return IRIS_ERR_INVALID_ARG;

    struct KCNode *cn; uint32_t idx;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)auth_cptr,
                                           &cn, &idx);
    if (err != IRIS_OK) return err;

    struct KObject *auth; iris_rights_t ar;
    err = kcnode_fetch(cn, idx, &auth, &ar);
    if (err != IRIS_OK) {
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return err;
    }
    int ok = (auth->type == KOBJ_BOOTSTRAP_CAP) &&
             kbootcap_is((struct KBootstrapCap *)auth, kind);
    if (ok && out_ports) {
        out_ports[0] = ((struct KBootstrapCap *)auth)->port_first;
        out_ports[1] = ((struct KBootstrapCap *)auth)->port_last;
    }
    if (!ok) {
        kobject_active_release(auth);
        kobject_release(auth);
        kobject_active_release(&cn->base);
        kobject_release(&cn->base);
        return IRIS_ERR_ACCESS_DENIED;
    }
    /*
     * A-40: the authority object stays HELD until the publish is done with it.
     * It is the expected MDB parent, and an expectation compared against a
     * pointer whose object may have been freed and its address reused is not
     * an expectation.  The caller drops it through dev_cap_auth_release.
     */
    *out_cn = cn; *out_idx = idx; *out_auth = auth;
    return IRIS_OK;
}

static iris_error_t dev_cap_auth(struct task *t, uint64_t auth_cptr,
                                 uint32_t kind,
                                 struct KCNode **out_cn, uint32_t *out_idx,
                                 struct KObject **out_auth) {
    return dev_cap_auth_ranged(t, auth_cptr, kind, out_cn, out_idx, 0, out_auth);
}

static void dev_cap_auth_release(struct KCNode *cn, struct KObject *auth) {
    if (auth) { kobject_active_release(auth); kobject_release(auth); }
    kobject_active_release(&cn->base);
    kobject_release(&cn->base);
}

/*
 * Stage 7 Step 14 — the budget is NAMED, never assumed.
 *
 * These two allocated their object from `t->process->mem_pool`: the Untyped
 * the kernel remembered as "this process's", picked because the caller had not
 * said which of its budgets should pay.  That is the kernel choosing whose
 * memory funds an allocation, which Stage 6 Step 5 removed everywhere a
 * syscall had an argument to spare — these were the sites that did not.
 *
 * IRQCAP had one: arg2 was unused.  IOPORT did not, so its two 16-bit device
 * facts share arg1 (base | count << 16), which is what they always were —
 * one range, described in one word — and arg2 says who pays.
 *
 * Required, not defaulted.  A zero budget is INVALID_ARG rather than a fall
 * back to the process's own, because a default is the kernel making the choice
 * again with extra steps.
 */
static iris_error_t dev_cap_budget(struct task *t, uint64_t budget_cptr,
                                   struct KUntyped **out) {
    *out = 0;
    if (budget_cptr == 0u) return IRIS_ERR_INVALID_ARG;
    iris_rights_t br;
    /* WRONG_TYPE is reported as WRONG_TYPE.  This used to flatten it to
     * INVALID_ARG, which said "something about your argument is wrong" about a
     * capability the resolver had just identified exactly — the same defect the
     * resolvers themselves carried until they started checking type before
     * rights.  A caller that named an endpoint where a budget goes is told so. */
    return cspace_resolve_only_untyped(t->cspace_root,
                                       (iris_cptr_t)budget_cptr, RIGHT_WRITE,
                                       out, &br);
}

static void dev_cap_budget_release(struct KUntyped *u) {
    if (!u) return;
    kobject_active_release(&u->base);
    kobject_release(&u->base);
}

uint64_t sys_cap_create_irqcap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    uint8_t      irq_num   = (uint8_t)(arg1 & 0xFFu);
    uint64_t     dest      = arg3;
    struct task *t         = task_current();

    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (irq_num > 15u)     return syscall_err(IRIS_ERR_INVALID_ARG);
    if ((uint32_t)(dest >> 32) == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    struct KCNode *auth_cn; uint32_t auth_idx; struct KObject *auth_obj;
    iris_error_t err = dev_cap_auth(t, arg0, IRIS_BOOTCAP_IRQ_CONTROL,
                                    &auth_cn, &auth_idx, &auth_obj);
    if (err != IRIS_OK) return syscall_err(err);

    struct KUntyped *pool;
    err = dev_cap_budget(t, arg2, &pool);
    if (err != IRIS_OK) { dev_cap_auth_release(auth_cn, auth_obj); return syscall_err(err); }

    /* Stage 6 Step 6: the object comes out of a budget.  Stage 7 Step 14: out
     * of the one the caller named. */
    struct KIrqCap *irqcap = kirqcap_alloc_from(pool, irq_num);
    dev_cap_budget_release(pool);
    if (!irqcap) {
        dev_cap_auth_release(auth_cn, auth_obj);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    err = dev_cap_publish(t, &irqcap->base,
                          RIGHT_ROUTE | RIGHT_DUPLICATE | RIGHT_TRANSFER,
                          dest, auth_cn, auth_idx, auth_obj);
    dev_cap_auth_release(auth_cn, auth_obj);
    if (err != IRIS_OK) {
        kirqcap_free(irqcap);
        return syscall_err(err);
    }
    kobject_release(&irqcap->base);   /* the slot holds its own refs */
    return syscall_ok_u64(0);
}


uint64_t sys_cap_create_ioport(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    /* Stage 7 Step 14: one range, one word — base in the low half, count in
     * the high half — which frees arg2 to say which budget pays. */
    uint16_t     base      = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t     count     = (uint16_t)((arg1 >> 16) & 0xFFFFu);
    uint64_t     dest      = arg3;
    struct task *t         = task_current();

    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);
    if (count == 0u || (uint32_t)base + count > 0x10000u)
        return syscall_err(IRIS_ERR_INVALID_ARG);
    if ((uint32_t)(dest >> 32) == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);
    /*
     * The range check is against the AUTHORITY, not against a table.
     *
     * It used to be `kioport_in_whitelist(base, count)` — four hardcoded
     * ranges in syscall_priv.h — checked BEFORE the authority, so a caller
     * with no authority at all got "denied because of the port" and one with
     * full authority got "denied because of the port" too.  The kernel had no
     * basis for that list: which ports exist is a fact about a machine, and
     * who may claim them is a fact about who is trusted, and neither is the
     * kernel's to know.  It also applied to everybody equally, so it could not
     * express the one thing worth expressing — that init may claim a serial
     * port and svcmgr may not.
     *
     * Now the authority carries a range and this asks whether the request is
     * inside it.  Order matters and is deliberate: authority FIRST, so a
     * caller learns it has no authority rather than learning something about
     * the port map.
     */
    struct KCNode *auth_cn; uint32_t auth_idx; struct KObject *auth_obj;
    uint16_t auth_ports[2] = { 0u, 0u };
    iris_error_t err = dev_cap_auth_ranged(t, arg0, IRIS_BOOTCAP_IOPORT_CONTROL,
                                           &auth_cn, &auth_idx, auth_ports,
                                           &auth_obj);
    if (err != IRIS_OK) return syscall_err(err);
    {
        uint32_t req_last = (uint32_t)base + (uint32_t)count - 1u;
        if ((uint32_t)base < (uint32_t)auth_ports[0] ||
            req_last > (uint32_t)auth_ports[1]) {
            dev_cap_auth_release(auth_cn, auth_obj);
            return syscall_err(IRIS_ERR_ACCESS_DENIED);
        }
    }

    struct KUntyped *pool;
    err = dev_cap_budget(t, arg2, &pool);
    if (err != IRIS_OK) { dev_cap_auth_release(auth_cn, auth_obj); return syscall_err(err); }

    /* Stage 6 Step 6: the object comes out of a budget.  Stage 7 Step 14: out
     * of the one the caller named. */
    struct KIoPort *ioport = kioport_alloc_from(pool, base, count);
    dev_cap_budget_release(pool);
    if (!ioport) {
        dev_cap_auth_release(auth_cn, auth_obj);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    err = dev_cap_publish(t, &ioport->base,
                          RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE | RIGHT_TRANSFER,
                          dest, auth_cn, auth_idx, auth_obj);
    dev_cap_auth_release(auth_cn, auth_obj);
    if (err != IRIS_OK) {
        kioport_free(ioport);
        return syscall_err(err);
    }
    kobject_release(&ioport->base);
    return syscall_ok_u64(0);
}


/*
 * sys_handle_insert(proc_h, obj_h, rights, _) → new_handle_id or iris_error_t
 *
 * Copies obj_h into the target process's handle table with the specified rights
 * (a subset of obj_h's current rights). Requires RIGHT_MANAGE on proc_h and
 * RIGHT_TRANSFER on obj_h. The source handle is NOT consumed.
 * Returns the new handle_id assigned in the target process.
 */
/* ── I/O port sub-delegation (A4) ───────────────────────────────────── */

/*
 * SYS_IOPORT_CONTROL_NARROW — derive a narrower I/O-port control capability.
 *
 * The replacement for the kernel's port whitelist.  See the ABI note in
 * syscall.h for why the table had to go; what this adds is the thing the table
 * could not do — say WHO the restriction applies to.
 *
 * RIGHT_DUPLICATE on the source, because this creates a second capability
 * carrying (part of) the same authority, which is exactly what that right
 * governs everywhere else in IRIS.  A delegate handed a control capability
 * without it can use its range and cannot subdivide it further.
 */
uint64_t sys_ioport_control_narrow(uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3) {
    struct task *t = task_current();
    if (!t || !t->cspace_root) return syscall_err(IRIS_ERR_INVALID_ARG);

    uint16_t first     = (uint16_t)(arg1 & 0xFFFFu);
    uint16_t last      = (uint16_t)((arg1 >> 16) & 0xFFFFu);
    uint64_t dest      = arg3;

    if (first > last) return syscall_err(IRIS_ERR_INVALID_ARG);
    if ((uint32_t)(dest >> 32) == 0u) return syscall_err(IRIS_ERR_INVALID_ARG);

    /* Resolve the authority and read both its kind and its range while it is
     * held; the slot becomes the MDB parent of what comes out. */
    if (!cspace_only_cptr(arg0)) return syscall_err(IRIS_ERR_INVALID_ARG);
    struct KCNode *auth_cn; uint32_t auth_idx;
    iris_error_t err = cspace_resolve_slot(t->cspace_root, (iris_cptr_t)arg0,
                                           &auth_cn, &auth_idx);
    if (err != IRIS_OK) return syscall_err(err);

    struct KObject *auth; iris_rights_t ar;
    err = kcnode_fetch(auth_cn, auth_idx, &auth, &ar);
    if (err != IRIS_OK) {
        dev_cap_auth_release(auth_cn, 0);
        return syscall_err(err);
    }

    uint16_t src_first = 0u, src_last = 0u;
    int ok = (auth->type == KOBJ_BOOTSTRAP_CAP) &&
             kbootcap_is((struct KBootstrapCap *)auth,
                         IRIS_BOOTCAP_IOPORT_CONTROL) &&
             rights_check(ar, RIGHT_DUPLICATE);
    if (ok) {
        src_first = ((struct KBootstrapCap *)auth)->port_first;
        src_last  = ((struct KBootstrapCap *)auth)->port_last;
    }
    /* A-40: `auth` stays held all the way to the publish below — it is the
     * expected MDB parent, and a pointer whose object may already be freed
     * and its address reused is not an expectation. */

    /* A narrowing can only narrow.  Checked here rather than in the allocator
     * because it is a fact about the pair, not about the new object. */
    if (!ok || first < src_first || last > src_last) {
        dev_cap_auth_release(auth_cn, auth);
        return syscall_err(IRIS_ERR_ACCESS_DENIED);
    }

    /* The object comes out of a budget the caller NAMED, like every other
     * device capability since Stage 7 Step 14.  A narrowed control capability
     * is memory, and a syscall that let ring 3 spend the kernel's would be a
     * hole in charter M3 opened by the very change that closed a policy one. */
    struct KUntyped *pool;
    err = dev_cap_budget(t, arg2, &pool);
    if (err != IRIS_OK) {
        dev_cap_auth_release(auth_cn, auth);
        return syscall_err(err);
    }
    struct KBootstrapCap *narrow =
        kbootcap_alloc_from(pool, IRIS_BOOTCAP_IOPORT_CONTROL, first, last);
    dev_cap_budget_release(pool);
    if (!narrow) {
        dev_cap_auth_release(auth_cn, auth);
        return syscall_err(IRIS_ERR_NO_MEMORY);
    }

    err = dev_cap_publish(t, &narrow->base,
                          RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER,
                          dest, auth_cn, auth_idx, auth);
    dev_cap_auth_release(auth_cn, auth);
    if (err != IRIS_OK) {
        kbootcap_free(narrow);
        return syscall_err(err);
    }
    kobject_release(&narrow->base);
    return syscall_ok_u64(0);
}
