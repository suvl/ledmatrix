---
type: grilling
status: open
---

## Question

Burn-in protection: which screens stay static for long, and what stops them ageing the same pixels?

## Notes (22-24 Nov 2026)

Concern raised by the owner: a screen that sits on the panel for most of the time (and the clock screen likewise) may burn in the pixels.

Candidate measures to choose between: slow pixel shifting of static screens; shorter dwell with rotation between weather, clock and headlines; dimming or blanking after some time or at night hours (needs the time zone, ticket 022); moving the clock position; a brightness cap for static content. Decisions pending: which screens count as static, the night schedule if any, and whether blanking is acceptable.

### Owner input (24 Nov 2026, photo of the panel)
The screen that sits on the panel most is the three-column weather screen (layout C: NOW, +3H, +6H, with icons and temperatures), which shows whenever no plane or headline is due. The clock screen has the same problem. Both are fully static, with bright white and yellow pixels at the same positions for hours.

### Decisions so far (owner, 24 Nov 2026)
All four measures, each configurable on the portal:
1. **Pixel drift** of static screens (clock, weather, idle): one pixel every few minutes in a small loop.
2. **Rotation limit:** swap between the clock and the weather screen every N minutes; default 15 minutes; **default off**.
3. **Dim after stillness:** after N minutes with nothing new to show, dim; default 2 minutes, default dim level 10 percent.
4. **Night blanking:** panel dark between two clock times; default 00:00 to 06:00. Depends on the time zone ticket (022) for the right local time.

Still to decide: whether a new plane or a button press wakes the panel from the dim and from the blanking; whether "dim to 10 percent" means 10 percent of full brightness or 10 percent of the owner's chosen level; which of these are on by default (the owner gave defaults for all; rotation is explicitly off); how the dim and the wake fade; what counts as "stillness" (no plane and no headline shown).
