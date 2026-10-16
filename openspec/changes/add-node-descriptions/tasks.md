## 1. Model + store
- [x] 1.1 `Task::description`; `equivalent()` compares it
- [x] 1.2 SQLite schema v6 (column + upgrade step), load/upsert/`sameRow`; JSON store field
- [x] 1.3 `Session` meta accessors for `desc_collapsed`
- [x] 1.4 Tests: round trip (with newlines), v5 → v6 upgrade, incremental save of a description edit

## 2. Search
- [x] 2.1 `rankMatches`: description-only matches after text matches
- [x] 2.2 Tests for the ordering and for scope still applying

## 3. CLI
- [x] 3.1 `tt desc <node> [text|-] [--append] [--clear]`; `description` in the JSON task shape; help text
- [x] 3.2 Tests: set, stdin, append, clear, print exactness, JSON field

## 4. Card (read-only)
- [x] 4.1 Card layout (bottom-left, left of the input bar, max height + scroll) and drawing
- [x] 4.2 Ctrl+D + button collapse, persisted
- [x] 4.3 Node marker for non-empty descriptions

## 5. Card editing
- [x] 5.1 Pure multi-line row mapping (byte ↔ row/x) + tests
- [x] 5.2 Second `TextInput` for the card; Ctrl+E / click to edit; Enter newline, Ctrl+Enter / click-away save, Esc cancel; caret, selection, Up/Down, click-to-place
- [x] 5.3 TextInput undo/redo stack with typing coalescing + tests; Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y routed to it while the card edits
- [x] 5.4 Focus rules: canvas/palette keys inert while editing; store reload deferred while editing

## 6. Docs
- [x] 6.1 `docs/OVERVIEW.md` feature entry; `CLAUDE.md` `tt` verb list gains `desc`
