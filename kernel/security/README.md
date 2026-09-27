# kernel/security — PLACEHOLDER (not implemented)

**State:** Empty. Contains only `.gitkeep` subdirectories.

**Architectural risk:** This directory contradicts the IRIS microkernel model.

In a serious microkernel (seL4, Genode), "security" is not a kernel subsystem:
it is a consequence of the capability model. The kernel has no audit, auth,
identity or session subsystem; those concepts live in user-space servers with
capability-limited access.

**Current security state in IRIS:**
- Capabilities: implemented in `kernel/new_core/` (kobject, kcnode, rights.h)
- Access control: via rights bits on each handle (see `nc/rights.h`)
- Audit: not implemented (neither in kernel nor user space)
- Authentication: not implemented
- User sessions: not implemented

**Decision:** Implement nothing here. If audit is ever needed it is a
user-space server holding the capabilities it audits, not a kernel module —
and this directory should be removed rather than filled.

Empty subdirectories: `audit/`, `auth/`, `capabilities/`, `identity/`, `session/`
