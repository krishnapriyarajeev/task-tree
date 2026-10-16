# Code audit — dead code & optimization (2026-08-05)

Full-app sweep for **dead code** and **unoptimized code**, prompted after `83c24dc`
(ttd 152, "remove dead code — unused Hotkeys accessors"). That commit fixed *instances*
of the unused-accessor pattern, not the *class* — the same pattern survives elsewhere,
and there is real per-frame render waste.

## How this was checked
- **Compiler pass:** re-compiled all 16 of our translation units (headers covered
  transitively) with `-Wall -Wextra -Wunused -Wunused-but-set-variable -Wunreachable-code
  -Wshadow -Wredundant-decls -fsyntax-only`. Result: **0 warnings** — no compiler-detectable
  dead code (unused locals, dead stores, unused static fns/params, unreachable, shadowing).
- **Cross-reference pass:** every public/member symbol grep'd across `src/` + `tests/`;
  a symbol is called "dead" only when the grep shows no caller. Each dead-code row below
  was independently grep-verified.
- **Coverage gap:** the two macOS backends (`HotkeysMac.mm`, `NativeWindowMac.mm`) can't
  compile on Linux, so they missed the `-Wunused` sweep. Their symbols were cross-referenced
  (all live), but they did not get the compiler pass.

---

## Dead code — verified (grep proves zero callers)

- [ ] `DragController::cursor()` — `src/ui/DragController.hpp:26` — unused accessor
      (member `cursor_` is used internally; the getter is never called). Same pattern as ttd 152.
- [ ] `DragController::insertIndex()` — `src/ui/DragController.hpp:24` — unused accessor.
- [ ] `TextInput` focus mechanism — `src/ui/TextInput.hpp:17,18,36` **+ 6 call sites in
      `App.cpp`** (`:59, :68, :74, :240, :590, :1164`). `focused()` is never called →
      `focused_` is write-only → every `setFocused(...)` is a dead store. Remove the getter,
      setter, member, and all 6 calls together — the whole feature is inert.
- [ ] `#include <cctype>` — `src/app/App.cpp:4` — unused (`trim()` uses `find_first_not_of`).
- [ ] `#include <limits>` — `src/app/App.cpp:8` — unused (no `numeric_limits`).
- [ ] `#include <cmath>` — `src/layout/Geometry.hpp:6` — unused (only `std::min`, from `<algorithm>`).
- [ ] `#include <functional>` + `#include "platform/Hotkey.hpp"` — `src/platform/IPlatform.hpp:7,9`
      — unused here. Header — confirm the 2 consumers still compile before deleting.
- [ ] `#include <utility>` + `#include <vector>` — `src/llm/IClassifier.hpp:9,10` — unused.

### Looks dead but is NOT — keep
- `store::deletedRows()` / `struct DeletedRow` (`Store.hpp`) — test-only, but documented
  foundation for a future "restore deleted" view.
- `History::canUndo()` / `canRedo()` — test-only; completes the class API.
- `Rect::left()` (`Geometry.hpp:27`) — test-only; completes the symmetric accessor set.
- All `IPlatform` / `IClassifier` virtuals and both `.mm` backends — dispatched/compiled
  per-platform, not dead.

---

## Unoptimized code

### Worth fixing
- [ ] **DONE panel laid out every frame while hidden** — `App.cpp:823` calls
      `layoutDonePanel()` *before* the `if (mode_ == Mode::Full)` gate, but `doneRows_`/
      `donePanel_` are consumed only when `donePanel_.visible`. So the full-forest scan
      (`:893`), `doneSectionRoots()` (`:901`, scan + sort), and one NanoVG `measureTextHeight`
      **per done row** (`:932`, 43+ rows) all run and are discarded whenever the panel is
      hidden — always in QuickAdd, autohidden in Full — at up to 60 fps during pan/flash
      animations. **Fix:** keep `donePanel_ = L;` (`:895`), guard `:893-894` and `:901-960`
      behind `if (L.visible)`. `doneMaxScroll_`/`scrollY_` recompute the frame it re-shows.
- [ ] **Per-node, per-frame recompute in the draw loop** — `Renderer.cpp`, `drawTree`→
      `drawNode` runs over every node each frame and rebuilds stable values:
      - `shortDate(t.createdAt, nowMs)` — `:270` — 2×`localtime_r` + string build per node.
      - `std::to_string(id)` — `:157` — heap string per node.
      - `nvgTextBounds` on the id label — `:163`.
      - `nvgTextBounds` on the date chip — `:180`.
      - `countDescendants(f, id)` — `:273` — recursive subtree walk per collapsed node.
      All depend only on id / structure / calendar-day. Cache per node (mirror the existing
      `NodeTextCache`); invalidate on structural edit or day-rollover. Do this as its own
      change with a before/after frame-time check.

### Smaller / cold-path
- [ ] `OpenAiClassifier::classify` — `OpenAiClassifier.cpp:154` — worker lambda captures the
      multi-KB task-tree string by `[=]` copy; use move-capture. Cold path (once per task
      creation) but a clean win.
- [ ] Constant palette mode-name widths (`"find"`/`"select"`/`"parent"`) re-measured every
      frame in palette mode — `App.cpp:856-859`. Measure once / lazily.
- [ ] LLM log strings (`"endpoint: " + ... + ...`) concatenated at the call site even when
      logging is compiled out in prod — `OpenAiClassifier.cpp:44-49, 78-80, …`. Gate behind
      the `if (L)` check. Low priority.
- [ ] `drawInput` re-wraps text + heap-allocs a `vector<Row>` every frame — `Renderer.cpp:414`.
      Single widget, overlay-only; minor.

### Negligible at this scale — noted, not worth changing
- `doneSectionRoots()` comparator does 2 hash lookups/compare + parent-chain walk per node
  (`Forest.cpp:58`) — DONE-panel rebuild only.
- `repairAfterLoad()` worst-case O(n²) via inner `std::find` (`Forest.cpp:158`) — once per load.
- Whole-`Forest` copies per mouse event (`App.cpp:508, 682, 979, 991`) and `toggleCollapse`
  routing a view toggle through the full store-diff+copy (`:1020`) — event-driven, µs-cheap
  for hundreds of tasks (by design — see `History.hpp`).
- Possible float-key cache miss re-wrap in `drawNode` (`Renderer.cpp:147` vs `:83`) — needs
  instrumentation to confirm it actually misses; no visual bug either way.

### Readability / dedup (not dead, not perf)
- `trim()` duplicated (slightly different impls) in `App.cpp:26` and `Palette.cpp:9`.
- Identical preview-refresh (`previewSelect_ = …; reparentTarget_ = …;`) in `updatePalette`
  (`App.cpp:219`) and `movePaletteCursor` (`:251`) — extract a helper.

---

## Suggested sequencing (ttd-152 style: focused commits)
1. **Dead code** — the 2 `DragController` accessors, the `TextInput` focus mechanism + its
   6 call sites, and the 5 unused includes. Build + `ctest`, one commit.
2. **DONE-panel per-frame guard** — biggest perf win, low risk. Separate commit.
3. **Per-node render caching** — bigger; do with a before/after benchmark. Separate commit.
