---
type: grilling
status: closed
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

## Resolution (owner, 24 Nov 2026)

- **Optional toggle on the portal, off by default.** When on, every plane inside the radius is cycled, each for the dwell time, while it stays in range; planes keep priority, so weather, clock and headlines appear only when the radar is empty.
- **Brightness:** cycling planes stay at the owner's full brightness, all day if that is how it falls out. This is a flight tracker first; the other screens are add-ons. **Stillness means exactly: no new plane for the configured time (2 minutes by default)**, and it can only dim the non-plane screens (weather, clock, idle) because a plane on screen is never dimmed. Pixel drift still applies to every static picture, cards included. Night blanking applies as agreed (planes do not wake it).
- Defaults chosen without asking, owner may object: the cycle order is nearest plane first and re-evaluated at every fetch; a plane that leaves the radar drops out of the cycle at the next fetch (30 s); headlines wait in their queue (maximum 3, oldest dropped) until the radar is empty; the recent planes log still records each plane once, when it first enters.
