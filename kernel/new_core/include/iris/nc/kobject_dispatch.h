/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IRIS_NC_KOBJECT_DISPATCH_H
#define IRIS_NC_KOBJECT_DISPATCH_H

/*
 * Every object's close and destroy, named, so `kobject_release` can reach
 * them with a `switch (obj->type)` instead of a per-object function pointer.
 *
 * seL4 dispatches on the capability type and stores no pointer in the object;
 * IRIS stored one, in 18 `KObjectOps` tables across 17 files, and the cost was
 * not only the indirection:
 *
 *   - `scripts/check_lock_order.py` §9.1 admits it cannot follow
 *     `ops->destroy`, so the lock hierarchy was UNCHECKED exactly where a
 *     destructor runs — the place it matters most;
 *   - the type -> destructor mapping was invisible to a reader, and to any
 *     tool, because it existed only as initialisers scattered across the
 *     files that happened to define the objects;
 *   - and a forward-edge indirect call is the thing a control-flow attack
 *     wants.
 *
 * Declared here rather than in each type's own header because this is the
 * dispatch table — written down in one place, as a table should be, instead of
 * assembled at link time out of eighteen pieces.
 */

struct KObject;

void kasidpool_obj_close(struct KObject *obj);
void kasidpool_obj_destroy(struct KObject *obj);
void kbootcap_close(struct KObject *obj);
void kbootcap_destroy(struct KObject *obj);
void kbootcap_destroy_ut(struct KObject *obj);
void kcnode_obj_close(struct KObject *obj);
void kcnode_obj_destroy(struct KObject *obj);
void kendpoint_obj_close(struct KObject *obj);
void kendpoint_obj_destroy_ut(struct KObject *obj);
void kframe_obj_close(struct KObject *obj);
void kframe_obj_destroy(struct KObject *obj);
void kiopagetable_obj_close(struct KObject *obj);
void kiopagetable_obj_destroy(struct KObject *obj);
void kioport_close(struct KObject *obj);
void kioport_destroy(struct KObject *obj);
void kiospace_obj_close(struct KObject *obj);
void kiospace_obj_destroy(struct KObject *obj);
void kirqcap_close(struct KObject *obj);
void kirqcap_destroy(struct KObject *obj);
void knotification_close(struct KObject *obj);
void knotification_destroy_ut(struct KObject *obj);
void kpagetable_obj_close(struct KObject *obj);
void kpagetable_obj_destroy(struct KObject *obj);
void kreply_obj_close(struct KObject *obj);
void kreply_obj_destroy_ut(struct KObject *obj);
void kschedctx_obj_close(struct KObject *obj);
void kschedctx_obj_destroy_ut(struct KObject *obj);
void ktcb_obj_destroy(struct KObject *obj);
void kuntyped_obj_close(struct KObject *obj);
void kuntyped_obj_destroy(struct KObject *obj);
void kvspace_obj_close(struct KObject *obj);
void kvspace_obj_destroy(struct KObject *obj);

#endif /* IRIS_NC_KOBJECT_DISPATCH_H */
