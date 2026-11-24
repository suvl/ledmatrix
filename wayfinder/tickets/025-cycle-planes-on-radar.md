---
type: grilling
status: open
---

## Question

A toggle to cycle through the planes that are on the radar, instead of showing each plane once.

## Notes (22-24 Nov 2026)

Today each new plane is shown once for the dwell time, then not again for 30 minutes. Wanted: a portal toggle that keeps cycling through every plane currently inside the radius while it stays there.

Decisions pending: whether weather and headlines still get turns while planes are present (a plane always takes priority today), the order of the cycle, what the log records, and how this interacts with the dwell time.

### Decided (owner, 24 Nov 2026)
- The toggle is **optional, off by default**.
- When on, every plane currently inside the radius is cycled for the dwell time while it stays in range. **No round, no breaks**: planes keep priority, so with a plane always on the radar the weather, clock and headlines are not shown. The owner accepts that.
- When the radar is empty the normal screens return as today.

Still to decide: how this meets the burn-in rules (a single plane circling for hours is a static picture, and "stillness" was defined as no plane or headline *shown*); what the cycle order is; and whether headlines still wait their turn in the queue.
