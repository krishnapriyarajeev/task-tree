## Why

A task's text is a one-line label. The context that makes it actionable — which Linear
ticket, which repo and branch, which PR, what "done" means — has nowhere to live, so it
ends up crammed into the label or kept outside TaskTree entirely. The concrete consumer is
agents: loops that walk the tree and check, per task, whether it is finished. They need
somewhere durable, per node, to read and write that context, and the user needs to be
able to see and fix what they wrote.

## What Changes

- **A `description` on every task**: free, multi-line plain text, empty by default. No
  structure is imposed — what goes in it is the writer's (usually an agent's) business.
- **Description card** in the overlay's free bottom-left corner, showing the selected
  node's description (the right edge belongs to the DONE panel).
- **Ctrl+D** (or the card's button) collapses the card to a slim header and back; the
  choice is remembered across restarts.
- **Ctrl+E** (or clicking the card's body) edits the description in place, in the card:
  Enter = newline, Ctrl+Enter or clicking away = save, Esc = cancel. While editing,
  Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) undo and redo the text itself, in word-sized steps; a
  saved edit is one app-level undo step.
- **Marker** on nodes that have a description, in the node's top band, so notes are
  visible without selecting every node.
- **Search**: `?` find, `:` / `>` text queries and `tt find` also match descriptions,
  ranked below matches in the task's own text.
- **CLI**: `tt desc <node>` prints it; `tt desc <node> "text"` sets it; `tt desc <node> -`
  reads stdin; `--append` adds a line instead of replacing; `tt desc <node> --clear`.
  `tt show` and every `--json` task shape gain `description`.
- **Schema v6**: `tasks.description TEXT NOT NULL DEFAULT ''`.

## Non-goals

- Any parsed structure, keys, or conventions inside a description (agents own that).
- Markdown rendering, links, or rich text — plain wrapped text only.
- Showing descriptions on hover.
- Descriptions on the DONE panel's rows (select a done task: not possible today; out of
  scope here).

## Impact

- `src/model` (Task field, both stores, schema v6), `src/app` (card state, keys, editing,
  search ranking in `Palette`), `src/render` (card + editor drawing, node marker),
  `src/cli` (`desc` verb, JSON field), tests for each, `docs/OVERVIEW.md`, `CLAUDE.md`'s
  `tt` verb list.
