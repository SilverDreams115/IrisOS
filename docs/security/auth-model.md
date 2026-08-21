# IRIS authentication model

> **Not implemented — aspirational.**  IRIS has no user accounts, no login, no
> password store and no privilege elevation.  It boots to a shell with no
> authentication step, and authority in the running system is capability-based
> rather than identity-based: what a service may do is what its CSpace holds,
> and a message's sender is identified by the kernel-stamped **badge** on the
> capability it invoked (`docs/badges-sender-identity.md`), not by a user.
>
> This file records the intended shape of a future login model.  Nothing below
> corresponds to code in the tree, and a POSIX-style user model is Stage 10
> platform work at the earliest.

## Principles
- One user, one master password
- Login, terminal and privilege elevation use the same credential
- Passwords stored as a secure salted hash
- Authentication and authorization kept separate
