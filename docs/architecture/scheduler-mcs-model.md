# IRIS — MCS Scheduling Model (Fase S2)

SchedulingContext (SC) canónico estilo seL4-MCS: presupuesto/período como
objeto kernel retipado desde Untyped, ligado uno-a-uno a un TCB.

## SchedulingContext

- Storage: **exclusivamente Untyped** (`SYS_UNTYPED_RETYPE2`,
  `KOBJ_SCHED_CONTEXT`).  `SYS_SC_CREATE` (83) y el `kschedctx_alloc` kslab
  están RETIRADOS (ledger).
- Estado inicial (B2): un SC recién retipado nace **sin configurar y sin
  ligar** — `budget=period=remaining=0`, `configured=0`, `bound_task=NULL`.
  No puede dirigir un hilo hasta configurarse y ligarse.
- Campos: budget/period/remaining, `bound_task` (binding inverso uno-a-uno),
  `configured`, header KObject, lock.  Sin sidecar kslab.

## SC_CONFIGURE (84)

`SYS_SC_CONFIGURE(sc_cptr, budget, period)`: valida `budget>0`, `period>0`,
`budget<period`; resetea `remaining=budget`; marca `configured=1`.  Atómico
bajo el lock del SC (S2.8).  Requiere RIGHT_WRITE.

## SC_BIND (113)

`SYS_SC_BIND(sc_cptr, tcb_cptr)`: enlaza explícitamente un SC a un TCB, ambos
por CPtr, ambos vivos, uno-a-uno (S2.9).  Contrato:

- el SC debe estar configurado;
- el TCB no puede tener ya otro SC (`IRIS_ERR_BUSY`);
- el SC no puede estar ligado a otra task (`IRIS_ERR_BUSY`);
- `tcb_cptr == 0` desliga el SC de su task actual;
- requiere RIGHT_WRITE en ambos.

`SYS_THREAD_SET_SC(sc_cptr)` sigue existiendo como **self-bind** (el hilo
llamante se liga a sí mismo) y ahora también respeta el uno-a-uno.

## Binding lifecycle

- unbind explícito: `SC_BIND(sc, 0)` o `THREAD_SET_SC(0)`.
- muerte del TCB: `task_release_sched_ctx` desliga (`kschedctx_unbind`) antes
  de soltar la ref → el SC no conserva `bound_task` stale (S2.11).
- muerte/última cap del SC: destrucción devuelve el payload a la región
  Untyped; el TCB suelta su ref en su propio teardown.
- budget exhaustion: sin cambios respecto a Ph75 (el tick decrementa
  `remaining`; agotado → `TASK_BUDGET_EXHAUSTED` hasta el refill).
- rebind: permitido tras unbind (T267).

## Instrumentación (SYS_UNTYPED_QUERY kind 4)

`sc_live / sc_hwm / sc_retyped / sc_destroyed` (+ los equivalentes de TCB y
los contadores CDT).  Diagnóstico, nunca autoridad.

## Camino de construcción de tarea (destino S2)

```
Untyped ── RETYPE2 → SC cap
SC_CONFIGURE(sc, budget, period)
… (TCB retype + TCB_CONFIGURE — increment 2)
SC_BIND(sc, tcb)
TCB_RESUME(tcb)
```

Increment 1 (esta entrega) cierra el eje SchedulingContext. El eje TCB
(retype + configure + resume desde userland) y el root-CNode-aportado quedan
para el increment 2; ver `sel4-convergence-ledger.md`.
