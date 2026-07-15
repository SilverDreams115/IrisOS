# IRIS — IPC Model (Fase S1)

Endpoints síncronos (rendezvous), notifications asíncronas y reply objects
explícitos estilo seL4-MCS.  Complementa `a1-5-ipc-receive-slot.md` e
`ipc-stress-invariants.md` (invariantes I1–I18, que S1 preserva).

## Endpoint

Sin cambios de semántica de rendezvous, staged caps (A1.10 two-phase),
badges (Fase 9) ni bulk (Ph69).  Cambia el ORIGEN del objeto: solo
`SYS_UNTYPED_RETYPE2`.

## Reply objects explícitos (S1)

El kernel YA NO fabrica un KReply por cada EP_CALL.  El modelo:

```
servidor: RETYPE2(…, KOBJ_REPLY, …)   → reply cap en su CSpace
servidor: SYS_EP_RECV(ep, msg, reply_cptr)   ← arg2 nuevo
kernel:   stage (claim exclusivo; BUSY si ya staged/bound)
rendezvous con EP_CALL:
          bind(caller); msg.attached_handle = reply_cptr (eco)
servidor: SYS_REPLY(msg.attached_handle, reply)
kernel:   one-shot por binding; el OBJETO vuelve a free y es reutilizable
```

- Un recv sin reply (arg2 = 0) no puede servir CALLs: el CALL falla
  `NOT_SUPPORTED` sin consumir nada (el receiver bloqueado queda encolado;
  un sender call-mode encolado no es dequeueado).  S22: el camino legacy no
  crea objetos ocultos.
- Un servidor que "aparca" una reply mientras sigue sirviendo usa DOS reply
  objects y alterna (kbd: slots 13/14).
- Muerte del caller → unbind (objeto reutilizable; SYS_REPLY → NOT_FOUND).
- Última cap del reply borrada con caller bound → caller despierta CLOSED.
- El supervisor que mintea la reply a un hijo debe SOLTAR su propia copia:
  una copia retenida suprimiría close-wakes-caller en la muerte del hijo.

ABI: `SYS_EP_RECV`/`SYS_EP_NB_RECV` arg2 pasa el reply CPtr (0 = ninguno).
Cambio justificado y documentado: los callers in-tree pasaban basura no
inicializada en rdx en algunos wrappers de 2 args; todos migrados a
wrappers de 3 args explícitos.  `SYS_REPLY` no cambia (dual-resolve del
valor ecoado).  `SYS_EP_CALL` no cambia.

## Notification

Sin cambios de semántica (signal/wait/timeout, pending bits, IRQ delivery,
shared pager notification).  Origen: solo RETYPE2.  La quota de
notifications fue retirada: crear notifications requiere Untyped + slots.

## Contadores

`iris_ipc_stat_reply_caps` cuenta BINDINGS de reply (uno por rendezvous de
CALL), preservando los balances exactos de I16–I18 y de T109/T110.
