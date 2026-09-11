# Console Endpoint Protocol (Phase 7.3)

`iris/console_ep_proto.h` defines the KEndpoint protocol for the serial
console service. Since Phase 13 it is the ONLY console path: the legacy
`CONSOLE_MSG_WRITE` KChannel route is retired and every writer — init, sh,
vfs, iris_test and svcmgr's klog drain — goes through `console.ep`.

## Why the console moved to an endpoint

The legacy console was **asynchronous**: writers enqueued `CONSOLE_MSG_WRITE`
into the service KChannel and continued; the console task drained the backlog
to the UART later. That asynchrony caused a real interleaving bug (Phase 7.1:
the S10 marker was split mid-line by iris_test's raw COM1 output) which had
to be patched with a barrier message. The endpoint removes the problem at the
root: `EP_Call` is synchronous rendezvous, so **every write is its own flush
barrier** — when the call returns, the bytes are on the UART.

## Endpoint ownership and discovery

The console is spawned by **init** (not from the svcmgr catalog), so the
`own_service_ep` machinery does not apply. Init plays the svcmgr role:

1. init retypes the endpoint from its untyped pool and keeps the master.
2. The **recv** side is pre-start-minted into the console's root CNode at
   `IRIS_CPTR_OWN_EP` (slot 5, `RIGHT_READ`); the console serves the slot
   directly.
3. The **send** side is pre-start-minted into svcmgr's root CNode at
   `IRIS_CPTR_CONSOLE_EP` (slot 3, `RIGHT_WRITE|DUPLICATE|TRANSFER`); svcmgr
   publishes it as `"console.ep"` and re-mints it (WRITE) into every catalog
   child.

Because the cap is spawner-minted and never runtime-registered, the `".ep"`
anti-spoof rule (REGISTER rejects `".ep"` names) holds for the console exactly
as for catalog services.

## Operations

Wire format: a MessageInfo word plus message registers (ledger A-33), with the
opcode in the message LABEL. Exactly one reply per request; unknown opcodes →
`IRIS_ERR_NOT_SUPPORTED`.

| Opcode | Value | Request | Reply |
|--------|-------|---------|-------|
| `CONSOLE_EP_OP_WRITE` | 0x0301 | bulk payload = raw bytes in the caller's IPC buffer (`msg.buf_len`; 0 = no-op) | `REPLY_OK` only after every byte hit the UART |
| `CONSOLE_EP_OP_SYNC` | 0x0302 | no payload (payload → `INVALID_ARG`) | `REPLY_OK` |
| `IRIS_EP_OP_PING` | 0xFF01 | — | `REPLY_OK`, `words[1]` = the badge the kernel delivered |

`CONSOLE_EP_OP_SYNC` was the **cross-path** barrier: it let an endpoint client
order itself against the legacy writers. With those gone it is a trivial
acknowledge, kept because clients call it and because a barrier that costs one
rendezvous is not worth an ABI break.

## Server loop (services/console/main.c)

Endpoint-only, and a blocking receive rather than a drain — there is no second
transport left to fall through to:

```
loop:
    req.reply = IRIS_CPTR_OWN_REPLY        /* the explicit reply object (S1) */
    iris_msg_recv(IRIS_CPTR_OWN_EP, &req)  /* blocking                       */
    serve (WRITE / SYNC / PING)
    iris_msg_reply(req.got_cap, &reply)    /* exactly once                   */
```

The reply object arrives in `req.got_cap`, which since A-33 is its own field —
a Call hands the server two capabilities (the caller's gift and the reply it
is owed) and they no longer share one.

## Clients

- `services/common/console_client.h` provides `console_ep_write()` (chunks to
  the caller's IPC-buffer capacity) and `console_ep_sync()`.
- **init**: retypes the endpoint, logs through the master
  (`init_log` prefers the EP path), prints the gated `[USER] console ep OK`.
- **sh**: writes through slot `IRIS_CPTR_CONSOLE_EP` from its first
  instruction; prints the gated `[SH] console cptr OK`.
- **vfs**: writes through slot 3, verified by PING; prints the gated
  `[VFS] console cptr OK`.
- **iris_test**: T036 (lookup + PING — name lookup stays alive), T037/T038
  (WRITE / SYNC + malformed semantics via the looked-up cap), T043
  (WRITE via slot 3 — the gated marker
  `[IRIS][TEST] console cptr write OK` travels over the slot).

All four markers are smoke-gated in `scripts/run_qemu_headless.sh`; a broken
console EP path cannot pass CI.

## Limits

| Limit | Value |
|-------|-------|
| Max bytes per WRITE | the writer's registered IPC buffer (a 4 KiB frame) — clients chunk |
| Writers on any other path | none (Phase 13) |
| Replies per request | exactly 1 (KReply one-shot) |
