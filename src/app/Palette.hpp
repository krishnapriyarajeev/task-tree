#pragma once
// Command palette for the input bar — the app's only text field.
//
// A leading symbol typed into an *empty* bar switches the bar's MODE. The symbol is
// consumed: it never appears in the text, the bar shows a coloured mode chip on the left
// instead, and what you type after it is just the mode's argument. Esc, or Backspace with
// an empty argument, drops back to plain add mode.
//
//   (none)    Add      type a task, Enter adds it (LLM-classified as before)
//   /         Menu     a picker: the bar shows the highlighted mode, ↑/↓ + Enter takes it
//                      (typing a mode's symbol jumps straight there)
//   ?         Find     highlight matches, walk them, jump to one
//   :         Select   select a node: "12" by id, anything else by text
//   >         Parent   make a node the parent of the selection (same two ways)
//   @         Workspace     go to a workspace by name (no match: Enter creates it)
//   +         NewWorkspace  create a workspace here: a new node, marked, entered
//   #         Notes    open a note by title (no match: Enter creates one)
//
// In Select/Parent a leading '?' in the argument forces a text query, so the ':?12' /
// '>?query' spellings work exactly as typed. To add a task that starts with a prefix
// symbol, type a space first (the bar is then non-empty, so symbols are literal; commit
// trims the space).
//
// Pure + dependency-free: the grammar and the match ranking are unit-tested in
// tests/core_tests.cpp and the App layer owns every side effect.

#include <cstddef>
#include <string>
#include <vector>

#include "model/Task.hpp"

namespace tt::palette {

// What the bar is doing. Drives the chip label, the accent colour and the drop-up.
enum class Mode { Add, Menu, Find, Select, Parent, Workspace, NewWorkspace, Notes };

// The resolved action for the current (mode, argument) pair.
enum class Kind { AddTask, Find, SelectId, SelectText, ParentId, ParentText,
                  GoWorkspace, NewWorkspace, GoNote };

struct Command {
    Kind kind = Kind::AddTask;
    TaskId id = 0;         // SelectId / ParentId — 0 when none was given
    std::string query;     // Find / SelectText / ParentText, trimmed
    std::string body;      // AddTask / NewWorkspace — the text, trimmed

    bool isCommand() const { return kind != Kind::AddTask; }
    // Modes whose target comes from the ranked match list (↑/↓ and the drop-up apply).
    bool picksByText() const {
        return kind == Kind::Find || kind == Kind::SelectText || kind == Kind::ParentText;
    }
    bool selects() const { return kind == Kind::SelectId || kind == Kind::SelectText; }
    bool reparents() const { return kind == Kind::ParentId || kind == Kind::ParentText; }
    // The workspace switcher, picked from rankWorkspaces (↑/↓ and the drop-up apply).
    bool picksWorkspace() const { return kind == Kind::GoWorkspace; }
    // The notes list, picked from rankNotes (↑/↓ and the drop-up apply).
    bool picksNote() const { return kind == Kind::GoNote; }
};

// One row of the '/' menu, and the source of every mode's chip label + accent.
struct ModeInfo {
    Mode mode;
    char prefix;
    const char* name;      // chip label: "find" / "select" / "parent"
    const char* blurb;     // menu row description
    const char* hint;      // placeholder shown while the argument is empty
};

// The modes the '/' menu offers, in display order (Add and Menu aren't listed).
const std::vector<ModeInfo>& modes();
const ModeInfo* infoFor(Mode m);              // nullptr for Add
Mode modeForPrefix(char c);                   // Mode::Add when `c` isn't a prefix symbol

// Resolve the pending action. `arg` is the bar text with the prefix already stripped.
Command interpret(Mode mode, const std::string& arg);

// Case-insensitive substring matches over canvas nodes (the DONE section is excluded —
// it isn't on the canvas), best first: earliest match position, then shorter text (a
// tighter match), then smaller id so the order is stable across runs. `limit` 0 = all.
// `scope` restricts the search to that node's subtree (the workspace being shown);
// kMainView searches everything.
// Descriptions are searched too: description-only matches follow every text match, ranked
// the same way among themselves (position and length within the description). Pass
// `descriptions = false` for text-only — the CLI resolves node NAMES that way, so a task
// that merely mentions a name in its notes never makes the name ambiguous.
std::vector<TaskId> rankMatches(const Forest& f, const std::string& query,
                                std::size_t limit = 0, TaskId scope = kMainView,
                                bool descriptions = true);

// Workspaces whose text contains `query` (case-insensitive), ranked like rankMatches, ties
// in Forest::workspaces() order — which an empty query returns as is.
std::vector<TaskId> rankWorkspaces(const Forest& f, const std::string& query);

// Notes for the '#' list. An empty query returns every note, open or closed, oldest first.
// Otherwise case-insensitive substring matches: title matches first, then body-only
// matches, each ranked like rankMatches (match position, then length, then id).
std::vector<NoteId> rankNotes(const Forest& f, const std::string& query);

} // namespace tt::palette
