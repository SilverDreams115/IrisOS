# Fase 23 — Device / driver isolation hardening

Status: ACCEPTED — implemented in this phase.  Companion to
`service-authority-minimization.md` (Fase 22) and the earlier hardening docs.
Fase 22 minimized what caps a service holds over its peers; Fase 23 hardens what
a driver can reach in HARDWARE.  The audit and tests T164–T171 prove that a
compromised driver is contained by its device caps: it cannot cross a port
range, forge a port or IRQ, route or ack a line it holds no cap for, or reach
the framebuffer without the flag.

**No kernel bug was found.**  The device-authority model was already tight; this
phase audits it, proves containment with a compromised-driver stand-in, and
documents the threat model.  It is tests + docs only, no kernel change (an
IRQ-route instrument was prototyped and dropped — every route attempt from
ring-3 iris_test is an authority failure, so the live-route count is always
baseline and the notification-live gauge already catches a leaked route).  The
runtime selftest suite rises from 159/159 to 167/167.

The bar for this phase is explicit.  Not accepted: *"the driver did nothing
wrong."*  Accepted: *"even a hostile driver's authority ends exactly where its
cap ends."*

---

## The device-authority model

Three device-capability kinds, each resolved through the standard dual resolver
(CPtr or handle) with an explicit rights check and no fallback:

### KIoPort — I/O port access

- `SYS_CAP_CREATE_IOPORT(bootcap, base, count)` requires a KBootstrapCap with
  `IRIS_BOOTCAP_HW_ACCESS` AND `[base, base+count)` inside the kernel whitelist
  (PS/2 0x60+5, COM2 0x2F8+8, COM1 0x3F8+8, ACPI-PM 0x600+8).  Nothing else can
  be claimed; the kernel owns PIC/PIT and does not list them.
- `SYS_IOPORT_IN(cap, offset)` / `SYS_IOPORT_OUT(cap, offset, val)` access
  `cap->base_port + offset`, bounded by `offset >= cap->count → INVALID_ARG`.
  A driver can never reach a port outside its cap's range.  IN needs RIGHT_READ,
  OUT needs RIGHT_WRITE.
- The offset is masked to 16 bits then range-checked, so no arithmetic can walk
  past the range.

### KIrqCap — interrupt line authority

- `SYS_CAP_CREATE_IRQCAP(bootcap, irq)` requires HW_ACCESS; the cap embeds the
  authorized `irq_num`.
- `SYS_IRQ_ROUTE_REGISTER(irqcap, notif, proc)` needs RIGHT_ROUTE on the IRQ cap
  (whose embedded irq_num is the ONLY line it can route — no arbitrary IRQ),
  RIGHT_WRITE on a KNotification destination, and RIGHT_READ|ROUTE on the owner
  proc cap.  Registration replaces any prior route for that line
  (last-registration-wins), releasing the old notification and re-masking or
  unmasking the PIC line accordingly.
- `SYS_IRQ_ACK(irqcap)` needs RIGHT_ROUTE and unmasks exactly the cap's
  embedded line.  A driver can ack only its own IRQ.
- On driver death, `irq_routing_unregister_owner` (called from
  `kprocess_teardown`) clears every route the process owned, releases the
  destination notification, and re-masks the line — no route survives its owner.

### Framebuffer — one-shot bounded VMO

- `SYS_FRAMEBUFFER_VMO(bootcap, out)` requires `IRIS_BOOTCAP_FRAMEBUFFER`.  It is
  ONE-SHOT: the real fb service consumes it at boot (`fb_params_valid = 0`), so
  every later call — even from a cap that carries FRAMEBUFFER — returns
  NOT_FOUND.  The returned KVmo wraps exactly `[phys, size]` of the framebuffer,
  so a mapping of it cannot exceed the framebuffer region (bounded MMIO).

## Driver authority manifest

| Driver | Caps retained | Rights | Device scope | Reason |
|---|---|---|---|---|
| kbd | own ep / IRQ notif / 0x60 ioport / IRQ cap (line 1) | R / WAIT / R / ROUTE | 0x60–0x64 + IRQ1 only | serve kbd.ep, read scancodes, ack IRQ1 |
| console | own ep / 0x3F8 ioport | R / R\|W | 0x3F8–0x3FF only | serve console, UART I/O |
| fb | FRAMEBUFFER bootcap (restricted) | R | framebuffer region only | map + paint framebuffer (one-shot VMO) |

None of the three holds `HW_ACCESS`, so none can forge a new port or IRQ cap.
kbd/console hold no FRAMEBUFFER; fb holds no HW_ACCESS.  kbd's IRQ cap embeds
line 1, so it cannot touch any other line.  console holds no IRQ cap at all.

## Threat model — a compromised driver

If `kbd` is fully compromised it can: read/write ports 0x60–0x64, wait on and
ack IRQ1, and serve/receive on its own endpoint.  It CANNOT: reach any other
port (cap range-bounded), route or ack any other IRQ (cap embeds line 1, and it
holds no proc-ROUTE to route at all), forge a new device cap (no HW_ACCESS),
reach the framebuffer (no FRAMEBUFFER), or call any peer service (no client
caps — Fase 22).  `console` is even narrower: one port range, no IRQ authority.
`fb` can only map the framebuffer VMO it already holds.

The T165/T166 stand-in makes this concrete: a `lifecycle_probe` minted ONLY a
driver-like ioport cap (and nothing else) runs `LP_CMD_DEV_PROBE`, a battery of
escalation attempts — forge a port cap, forge an IRQ cap, read/write out of
range, ack an unheld IRQ — and reports a breach bitmask.  A contained driver
reports 0: every escalation is denied by the kernel.  The teeth check (an
in-range IN succeeds) proves the 0 means "denied", not "never tried".

## Invariants D1–D20

```text
D1  I/O port access requires a valid ioport cap.
D2  I/O port access cannot leave the granted range.
D3  I/O port access respects the cap's rights.
D4  Wrong-type device cap fails clean.
D5  Stale/revoked device cap fails clean.
D6  Invalid I/O size/offset fails with no side effect.
D7  A driver cannot use another driver's ports.
D8  IRQ route requires a valid IRQ cap.
D9  IRQ route cannot take another line (cap embeds its irq_num).
D10 IRQ ack requires RIGHT_ROUTE on the matching cap.
D11 Duplicate route replaces (last-registration-wins), documented.
D12 IRQ delivery creates no phantom cap or handle.
D13 Driver death clears its routes / waiters / notifications.
D14 Device cap derivation stays rights-monotonic.
D15 A device cap cannot become general authority.
D16 Framebuffer/MMIO access is bounded to the granted region.
D17 A compromised driver cannot spoof peer services.
D18 Device fuzz induces no kernel fault.
D19 Device cleanup causes no live-count drift.
D20 The driver authority manifest matches what drivers receive.
```

## Tests

```text
T164  ioport boundaries: in-range IN/OUT work; every out-of-range offset is
      INVALID_ARG; wrong-type cap, missing rights (READ-only denies OUT,
      WRITE-only denies IN), and a stale cap all fail; non-whitelisted ranges
      (CMOS 0x70, a range spilling past PS/2) cannot be created.
T165  kbd containment: a probe minted a READ-only ioport cap and nothing else
      reports breach 0 (no escalation); teeth: an in-range IN succeeds while an
      OUT through the READ cap stays denied.
T166  console containment: a probe minted an RW ioport cap still cannot cross
      its range or forge new authority — breach 0.
T167  IRQ route/ack authority: ack with a valid cap works; wrong-type, ROUTE-
      less (DUPLICATE-only derivation) and cross-authority attempts are denied;
      iris_test cannot route at all (its proc cap lacks ROUTE) — containment.
T168  framebuffer containment: the one-shot VMO is NOT_FOUND after boot even
      with a FRAMEBUFFER-carrying cap; a wrong-type auth cap is ACCESS_DENIED.
T169  device derivation monotonicity: a READ ioport cap cannot re-derive a
      working WRITE; a revoked cap is dead; a ROUTE-less IRQ cap cannot ack.
T170  driver death cleanup: probe drivers holding ioport caps, parked and
      killed, leave process/task/handle/endpoint/notification counts at baseline.
T171  device fuzz: seeded mix of whitelisted/non-whitelisted creates, in/out at
      valid and out-of-range offsets, derive/revoke, IRQ failure paths, and a
      compromised-driver probe — no access crosses a cap, no probe escalates,
      no drift.
```

`lifecycle_probe` gained `LP_CMD_DEV_PROBE` (the escalation battery).  Safe test
hardware: COM2 (0x2F8, whitelisted, unwired in headless QEMU) as the dummy port,
IRQ 5 (unused) as the dummy line.

## Remaining gaps

- **Framebuffer VMO runtime exercise**: the VMO is one-shot-consumed by fb at
  boot, so its mapping and out-of-range write cannot be driven from iris_test.
  The bounded-region property is asserted at the authority layer (the VMO wraps
  exactly `[phys, size]`); a full runtime MMIO-boundary test would need a second
  framebuffer-like region reserved for testing.
- **IRQ route success + death cleanup from ring-3**: iris_test cannot install a
  route (it holds no proc cap with RIGHT_ROUTE — itself a containment property).
  The route-cleanup path (`irq_routing_unregister_owner` in teardown) is
  exercised by the real svcmgr→kbd routing and service restart, and a leaked
  route would keep its notification alive (caught by the notification-live
  gauge).  A dedicated killable-router test would need a proc-ROUTE cap.
- **MMIO beyond the framebuffer**: IRIS has no general MMIO-cap model yet; when
  one is added it must carry the same range-bounded, rights-checked contract.

## Adding a new driver without widening authority

1. Give it the SMALLEST device caps it needs: an ioport cap over exactly its
   register range, an IRQ cap for exactly its line, nothing more.
2. Never grant a driver `HW_ACCESS`, `FRAMEBUFFER`, `KDEBUG`, or a spawn cap —
   those let it forge device authority or escalate.  Drivers receive already-
   minted device caps, not the bootstrap cap that creates them.
3. Never grant a driver a proc cap with RIGHT_ROUTE unless it must route its own
   IRQ; prefer svcmgr owning the route on the driver's behalf.
4. Keep the driver off the peer client-endpoint slots (Fase 22 `client_eps`)
   unless it genuinely calls that peer.
5. The range-bounded, rights-checked, cap-embedded-irq_num contract is enforced
   by the kernel — a new driver inherits containment automatically as long as it
   is handed narrow caps.
