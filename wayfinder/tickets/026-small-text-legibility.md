---
type: grilling
status: closed
---

## Question

How do we stop capital N, M and W (and similar pairs) being confused in the small text, for example "NOW" read as "MOW"?

## Notes (24 Nov 2026)

Cause found in the code: the labels and most secondary text use the Tom Thumb font, which is 3 pixels wide for capitals, so N, M and W cannot be told apart at that width. Seen on the real panel in the weather screen (`NOW`, `+3H`, `+6H`). The same font carries route lines, aircraft types and "to <city>" lines, so the confusion can also happen in names.

Options to choose between: replace the weather labels with something without those letters (clock hours such as 21h, 00h, 03h, which also say more than "+3H"); a wider font (5 px) for labels, at the cost of space; hand-drawn 4 or 5 px glyphs for N, M and W only; lower case for words (n, m, w differ more). Decision pending, and whether to apply the same rule to every screen.

## Resolution (owner, 24 Nov 2026)

- **Weather labels become the clock hour each column refers to** (for example 21h, 00h, 03h) instead of NOW, +3H, +6H. There is no N, M or W in them, and an hour says more than an offset. Digits and "h" are clear in the 3 pixel font.
- It depends on the clock being right for the chosen place, so it builds on the time zone ticket (022). With the default Lisbon zone it already works.
- Not changed for now: other small text (route, aircraft type, "to <city>"). If a real confusion shows up there, reopen as a new ticket with the concrete example.
- The preview page and the portal thumbnails use the same renderer and must show the same labels.
