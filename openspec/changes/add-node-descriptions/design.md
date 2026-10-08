## Context

Tasks are rows in `tasks` (SQLite, schema v5) and `Task` structs in a `Forest`. Every
per-task field already flows through the same machinery: `sameRow`/upsert for incremental
saves, `equivalent()` for round-trip proofs, `History` snapshots for undo, the
`data_version` poll for live reload, and `tt`'s load → mutate → save-with-baseline. A
description is one more field on that path, so the data side is small. The work is in the
UI: TaskTree has exactly one text field today (the single-line input bar), and a
description needs multi-line display and editing.

## Decisions

### 1. A plain `std::string description` on `Task`, stored as a column
Not a side table: it is 1:1 with the task, is soft-deleted with it, and must be in undo
snapshots and baseline diffs exactly like `text`. `sameRow` compares it; the upsert writes
it. The JSON store gains `"description"`. Empty string = none (no separate flag).

### 2. The card lives bottom-left, anchored to the input bar's baseline
The input bar is centred and at most 620 px wide, so the bottom-left is free — and unlike
the bottom-right, never under the DONE panel. The card is ~360 px wide (clamped to the free
width left of the bar), grows with its content up to ~45 % of the window height, then
scrolls (mouse wheel over it). Same chrome as the palette drop-up (`drawFieldChrome`) so it
reads as part of the HUD.

Shown only while a canvas node is selected. Header line: node id + "description", the
collapse button, and an "edit" hint. An empty description shows a dim placeholder
("no description — Ctrl+E to add").

### 3. Collapse is view state, remembered in the store's meta
`Ctrl+D` / the button toggle a collapsed state where only the header shows. Persisted as
meta `desc_collapsed` (beside `current_view`), not in the Forest — undo must not toggle it.

### 4. Editing happens in the card, with its own TextInput
The input bar stays single-line and keeps its meaning (tasks and palette commands). The
card gets a second `TextInput` instance as its buffer. While editing:
- Enter inserts `\n` (TextInput's Submit is intercepted); Ctrl+Enter saves; clicking
  outside the card saves; Esc cancels.
- Rendering wraps on both `\n` and width via `nvgTextBreakLines`, giving visual rows with
  byte ranges; caret and selection are drawn from those rows, and click / Up / Down map
  through them. This row layout is a pure helper (byte ranges in, positions out) so caret
  navigation is unit-testable without GL.
- Keys that act on the canvas (Delete, Ctrl+T, Ctrl+↑/↓, palette prefixes) are inert while
  the card is editing — keyboard focus is the card's.
- Save = one `History` snapshot + `save()`, only if the text changed.

### 4a. Text undo inside the editor, in TextInput
`TextInput` gains a bounded undo/redo stack of `{text, caret, anchor}` snapshots, pushed
before every mutation (typing, paste, cut, the deletes). Consecutive typed characters
coalesce into one step, broken by a pause (~1 s), whitespace, a caret move, or any other
kind of edit — so Ctrl+Z steps back a word or a burst, not a character. Redo is cleared by
a new edit. `clear()` / `setText()` reset the stack. Pure and unit-tested.

While the card is editing, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y go to the card's `TextInput`.
Everywhere else they keep their current meaning (app-level undo of the tree), including
in the input bar — giving the bar text undo too is possible later, but changes what
Ctrl+Z means while typing a task, so it is not part of this change.

### 5. Marker: a small "lines" glyph in the node's top band
Three short horizontal strokes, drawn left of the date chip, in the id-badge colour. Drawn
only when `description` is non-empty. It is dropped (like the date chip) when the band is
too narrow.

### 6. Search ranks description hits below text hits
`rankMatches` keeps its current order for text matches, then appends description-only
matches ordered the same way (match position, length, id). One list, so ↑/↓, the amber
rings and `tt find` all agree. The drop-up row shows the node's text, as now.

### 7. CLI: one `desc` verb
`tt desc <node>` prints the raw description (exactly, no trailing decoration).
`tt desc <node> <text>` replaces; `-` as the text reads stdin to EOF; `--append` appends
(inserting a newline first when non-empty); `--clear` empties. Writes go through the one
`saveStore` call site. JSON task shape gains `"description"`.

### 8. Schema v6 migration
`ALTER TABLE tasks ADD COLUMN description TEXT NOT NULL DEFAULT ''` inside the existing
one-transaction upgrade. Older builds refuse a v6 store (existing guard), so restart the
app on the new binary before any `tt` write — same as v5.

## Risks / trade-offs

- **Second text editor.** Multi-line caret/selection logic is new code; keeping the row
  mapping pure and tested limits the risk.
- **External edits while editing.** The app already defers reload during an in-bar edit;
  the same deferral applies while the card is editing, and the app wins a same-row
  conflict on save, as today.
- **Long descriptions** are measured on every relayout of the card only (not the canvas),
  so canvas performance is unaffected.
