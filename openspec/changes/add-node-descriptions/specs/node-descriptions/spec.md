## ADDED Requirements

### Requirement: Every task can carry a description
A task SHALL have a free-form, multi-line plain-text `description`, empty by default,
persisted in both stores, included in undo, incremental saves and live reload exactly like
the task's text.

#### Scenario: Round trip
- **WHEN** a task's description is set and the store is saved and reloaded
- **THEN** the description is byte-for-byte identical, including newlines

#### Scenario: Upgrade from schema v5
- **WHEN** the app opens a v5 store
- **THEN** every task has an empty description, nothing else changes, and the store is v6

### Requirement: The selected node's description is shown in a card
While a canvas node is selected, the overlay SHALL show its description in a card at the
bottom-left, clear of the input bar.

#### Scenario: Empty description
- **WHEN** the selected node has no description
- **THEN** the card shows a placeholder naming the shortcut to add one

#### Scenario: Nothing selected
- **WHEN** no node is selected
- **THEN** no card is shown

### Requirement: The card can be collapsed
Ctrl+D or the card's button SHALL toggle the card between full and header-only, and the
choice SHALL survive restarts. It SHALL NOT be affected by undo.

### Requirement: Descriptions are editable in the card
Ctrl+E or a click on the card's body SHALL edit the selected node's description in place.
Enter inserts a newline; Ctrl+Enter or clicking outside saves; Esc cancels. A save that
changes the text SHALL be one undo step.

#### Scenario: Cancel
- **WHEN** the user edits and presses Esc
- **THEN** the description is unchanged and no undo entry is added

#### Scenario: Undo while editing
- **WHEN** the user types two words while editing and presses Ctrl+Z
- **THEN** the second word is removed and the first remains; Ctrl+Shift+Z restores it
- **AND** the tree's own undo history is untouched

### Requirement: Nodes with a description are marked
A node whose description is non-empty SHALL show a small marker in its top band.

### Requirement: Search covers descriptions
Text search (`?`, `:`/`>` text queries, `tt find`) SHALL also match descriptions, ranking
every match in a task's text above every description-only match.

### Requirement: The CLI reads and writes descriptions
`tt desc <node>` SHALL print the description; `tt desc <node> <text>` SHALL replace it,
with `-` reading stdin; `--append` SHALL append a line; `--clear` SHALL empty it. Every JSON
task shape SHALL include `description`.
