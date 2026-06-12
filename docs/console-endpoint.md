# Console Endpoint Protocol (Fase 7.3)

`iris/console_ep_proto.h` defines the KEndpoint protocol for the serial
console service. It replaces the legacy `CONSOLE_MSG_WRITE` KChannel path for
endpoint clients (init, sh, vfs, iris_test); svcmgr keeps the legacy KChannel
write path until its legacy loop is retired (Fase 8).

## Why migrate the console

The legacy console is **asynchronous**: writers enqueue `CONSOLE_MSG_WRITE`
into the service KChannel and continue; the console task drains the backlog
to the UART later. That asynchrony caused a real interleaving bug (Fase 7.1:
the S10 marker was split mid-line by iris_test's raw COM1 output) which had
to be patched with the `CONSOLE_MSG_SYNC` barrier. The EP path removes the
problem at the root: `EP_CALL` is synchronous rendezvous, so **every EP write
is its own flush barrier** — when the call returns, the bytes are on the
UART.

## Endpoint ownership and discovery

The console is spawned by **init** (not from the svcmgr catalog), so the
`own_service_ep` machinery does not apply. Init plays the svcmgr role:

1. init creates the endpoint (`SYS_ENDPOINT_CREATE`) and keeps the master.
2. The **recv** side goes to console at bootstrap (kind
   `SVCMGR_BOOTSTRAP_KIND_SERVICE_EP` = 0x21, `RIGHT_READ`). Console's
   bootstrap loop *requires* it: a missing endpoint fails the smoke gates.
3. The **send** side goes to svcmgr at bootstrap (kind
   `SVCMGR_BOOTSTRAP_KIND_CONSOLE_EP` = 0x22), which publishes it as
   `"console.ep"` (`RIGHT_WRITE`) through both lookup paths.

Because the name is bootstrap-delivered and never runtime-registered, the
`".ep"` anti-spoof rule (`SVCMGR_MSG_REGISTER` rejects `".ep"` names) holds
for the console exactly as for catalog services.

## Operations

Wire format: `struct IrisMsg`, `msg.label` = opcode. Exactly one reply per
request; unknown opcodes → `IRIS_ERR_NOT_SUPPORTED`.

| Opcode | Value | Request | Reply |
|--------|-------|---------|-------|
| `CONSOLE_EP_OP_WRITE` | 0x0301 | bulk payload = raw bytes (`buf_len` ≤ 256; 0 = no-op) | `REPLY_OK` only after every byte hit the UART |
| `CONSOLE_EP_OP_SYNC` | 0x0302 | no payload (payload → `INVALID_ARG`) | `REPLY_OK` after the legacy KChannel queue is drained (non-blocking drain) |
| `IRIS_EP_OP_PING` | 0xFF01 | — | `REPLY_OK` |

`CONSOLE_EP_OP_SYNC` is the **cross-path** barrier: an EP client can order
itself against legacy writers (today: svcmgr). EP writes themselves need no
barrier — they are synchronous by construction.

## Server loop (services/console/main.c)

Endpoint-first coexistence, same shape as vfs/svcmgr:

```
loop:
    while EP_NB_RECV(ep_h) == OK:      /* drain EP requests            */
        serve (WRITE / SYNC / PING); SYS_REPLY exactly once
    SYS_CHAN_RECV_TIMEOUT(service_h, 5ms)   /* legacy writer: svcmgr   */
        on CONSOLE_MSG_WRITE: emit bytes
        on CONSOLE_MSG_SYNC:  ack via attached reply channel (proto v2)
```

`CONSOLE_EP_OP_SYNC` drains the legacy queue with `SYS_CHAN_RECV_NB` before
replying, so the FIFO guarantee spans both paths.

## Clients

- `services/common/console_client.h` provides `console_ep_write()` (chunks by
  `IRIS_IPC_BUF_SIZE`, needs a caller staging buffer — EP bulk payloads are
  read from user memory) and `console_ep_sync()`.
- **init**: creates the endpoint, logs through it when available
  (`init_log` prefers the EP path), prints the gated `[USER] console ep OK`.
- **sh**: looks up `"console.ep"` at boot; prints `[SH] console ep OK`.
- **vfs**: looks up `"console.ep"` via its discovery EP (kind 0x20);
  prints `[VFS] console ep OK`.
- **iris_test**: T036 (lookup + PING), T037 (EP WRITE — the gated marker
  `[IRIS][TEST] console ep write OK` itself travels over the endpoint),
  T038 (SYNC + malformed-request semantics).

All four markers are smoke-gated in `scripts/run_qemu_headless.sh`; a broken
console EP path cannot pass CI.

## Limits

| Limit | Value |
|-------|-------|
| Max bytes per EP WRITE | `IRIS_IPC_BUF_SIZE` (256) — clients chunk |
| Legacy writers remaining | svcmgr (KChannel `CONSOLE_MSG_WRITE`, retired with its legacy loop in Fase 8) |
| Replies per request | exactly 1 (KReply one-shot) |
