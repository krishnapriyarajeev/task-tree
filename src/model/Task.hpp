#pragma once
// Task + Forest data model. Pure data + tree operations, no rendering deps, so it
// is stack-agnostic (reused verbatim on the qt6 / imgui branches) and unit testable.

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace tt {

using TaskId = std::uint64_t;
using NoteId = std::uint64_t;   // its own id space: note #3 and task #3 are unrelated

// A parent id of 0 means "no parent" — the task is a top-level root.
constexpr TaskId kNoParent = 0;

// The whole-forest view ("main"), as opposed to a workspace rooted at a task.
constexpr TaskId kMainView = 0;

// `doneAt` for a task that is done but whose completion time isn't known — tasks completed
// before the date was recorded. It has to be distinguishable from "not done" (0) without
// inventing a completion date, so done-ness is `doneAt != 0`, not `doneAt > 0`.
constexpr std::int64_t kDoneAtUnknown = -1;

struct Task {
    TaskId id = 0;
    TaskId parent = kNoParent;
    std::string text;               // UTF-8
    std::vector<TaskId> children;   // ordered; sibling order IS the layout order
    bool collapsed = false;         // when true, layout hides this node's subtree (chevron toggles)
    int  status = 0;                // 0 = default, 1 = in progress (yellow), 2 = priority (orange)
    std::int64_t createdAt = 0;     // epoch ms when created (0 = unknown, e.g. pre-existing)
    // Completion, stored the same way deletion is: a timestamp IS the flag, so the two can
    // never disagree. 0 = not done, > 0 = done then, kDoneAtUnknown = done at an unknown
    // time. Done tasks keep their parent and their sibling slot — completing one does NOT
    // move it. The DONE section is derived from this (see Forest::doneSectionRoots) and the
    // canvas layout skips done subtrees, so un-doing puts a task back exactly where it was,
    // because it never left.
    std::int64_t doneAt = 0;
    // Workspace: a task can be marked as one, which makes it a place you can enter — the
    // overlay then shows only its subtree (Ctrl+Down in, Ctrl+Up out). Same convention as
    // doneAt: the timestamp IS the flag (0 = not a workspace, else when it was made one).
    // Its name is just its text. The task does not move: main always shows everything.
    std::int64_t workspaceAt = 0;
    // Free multi-line plain text (UTF-8, \n-separated), empty = none. No structure is
    // imposed: what goes in it is the writer's business — usually an agent recording the
    // ticket, branch or PR that makes the task actionable. Last, so positional
    // initialisers of the older fields keep compiling.
    std::string description;

    bool isDone() const { return doneAt != 0; }
    bool isWorkspace() const { return workspaceAt != 0; }
};

// A note: a card with a title and free text, separate from the task tree. It sits on the
// canvas at (x, y) in world coordinates, so it pans and zooms with the tasks, and it
// belongs to the view it was made in (`view`: kMainView or a workspace task). Closing a
// card hides it without deleting it; the notes list ('#') opens it again.
struct Note {
    NoteId id = 0;
    std::string title;              // single line, UTF-8; empty = untitled
    std::string body;               // free multi-line text, \n-separated
    TaskId view = kMainView;        // the view that shows it
    float x = 0.f, y = 0.f;         // top-left corner, world coordinates
    float w = 0.f;                  // card width in world units; 0 = the default
    bool open = true;               // false = closed: kept, but not drawn
    std::int64_t createdAt = 0;     // epoch ms (0 = unknown)
};

// A forest of task trees. `roots` holds the top-level tasks in display order — done ones
// included, since a task's position never depends on whether it is done.
class Forest {
public:
    std::unordered_map<TaskId, Task> nodes;
    std::vector<TaskId> roots;      // top-level tasks (parent == kNoParent), done or not
    TaskId nextId = 1;
    // Notes live here too, beside the tasks, so undo snapshots, store reloads and the
    // baseline-diffed save all cover them with no extra machinery. Ordered by id, which
    // is creation order: the notes list shows them that way.
    std::map<NoteId, Note> notes;
    NoteId nextNoteId = 1;

    Task*       get(TaskId id)       { auto it = nodes.find(id); return it == nodes.end() ? nullptr : &it->second; }
    const Task* get(TaskId id) const { auto it = nodes.find(id); return it == nodes.end() ? nullptr : &it->second; }
    bool exists(TaskId id) const { return nodes.count(id) != 0; }
    std::size_t size() const { return nodes.size(); }

    // Create a task with a fresh id. If `parent` is a real node the task is appended
    // to its children, otherwise it becomes a new root. Returns the new id.
    TaskId addTask(std::string text, TaskId parent = kNoParent, std::int64_t createdAt = 0);

    // ---- workspaces ----
    // Live workspaces (marked, and not in the DONE section), in the order they were made.
    std::vector<TaskId> workspaces() const;
    // The nearest workspace at or above `from`, walking up but stopping BEFORE `stopAt`
    // (kMainView = walk to the top). 0 when there is none. Ctrl+Down enters
    // nearestWorkspace(selection, currentView); Ctrl+Up goes to
    // nearestWorkspace(parent of currentView, kMainView).
    TaskId nearestWorkspace(TaskId from, TaskId stopAt = kMainView) const;

    // Is `node` inside the subtree rooted at `ancestor` (walking parent links up)?
    // Returns true when node == ancestor as well.
    bool isDescendantOf(TaskId node, TaskId ancestor) const;

    // Is `node` in the DONE section — i.e. is it done, or does any ancestor's done flag
    // hide it? Equivalently: is it off the canvas?
    bool isInDoneSection(TaskId node) const;

    // Tops of the DONE section: done tasks whose parent is NOT itself in the DONE section.
    // Derived, never stored, so it cannot drift from the flags. A done child of a live
    // parent is one of these — it heads its own DONE entry while staying in place on the
    // parent's children list. Ordered newest-completed first (doneAt, then id descending
    // for tasks completed before doneAt was recorded).
    std::vector<TaskId> doneSectionRoots() const;
    // The same, restricted to the subtree at `scope` (kMainView = everything).
    std::vector<TaskId> doneSectionRoots(TaskId scope) const;

    // Move `child` under `newParent` (kNoParent → make it a root) at position `index`
    // among the destination's children (clamped). Rejects moves that would create a
    // cycle (dropping a node onto itself or one of its own descendants). Returns
    // false if the move was rejected or ids are invalid.
    bool reparent(TaskId child, TaskId newParent, int index);

    // Remove a task and its whole subtree. Returns number of tasks removed.
    std::size_t removeSubtree(TaskId id);

    // ---- notes ----
    Note*       note(NoteId id)       { auto it = notes.find(id); return it == notes.end() ? nullptr : &it->second; }
    const Note* note(NoteId id) const { auto it = notes.find(id); return it == notes.end() ? nullptr : &it->second; }
    // Create an open note with a fresh id at world position (x, y) in `view`. Returns its id.
    NoteId addNote(std::string title, TaskId view, float x, float y, std::int64_t createdAt = 0);
    bool removeNote(NoteId id) { return notes.erase(id) != 0; }
    // The view a note shows in: its own, or main when that workspace is gone, done or
    // unmarked — so a note never becomes unreachable along with its workspace.
    TaskId noteView(const Note& n) const;

    // Complete a task (with its subtree), taking it off the canvas and into the DONE
    // section. The node does not move: parent and sibling slot are untouched. `doneAtMs` is
    // the completion time — the model owns no clock, so the caller passes it, as with
    // addTask's createdAt; 0 is stored as kDoneAtUnknown so the task is still done. Returns
    // false if it is already done or invalid.
    bool markDone(TaskId id, std::int64_t doneAtMs = kDoneAtUnknown);

    // Clear the done flag, which puts the task back exactly where it was — same parent,
    // same position among its siblings. Only when an ancestor is still done (so the task
    // would remain invisible) is it promoted to a root instead. Returns false if the task
    // is not done or invalid.
    bool restoreFromDone(TaskId id);

    // Make a freshly-loaded forest internally consistent WITHOUT discarding the order the
    // loader recovered: a dangling parent link becomes a root, `children` lists are filtered
    // to tasks that really point back (and deduped), a task its parent never listed is
    // appended to that parent, and a parent-less task missing from `roots` is appended there.
    // Needed because `parent` and `children` are two encodings of one edge and a file can
    // disagree with itself — an older or hand-edited file may carry `parent` links with no
    // `children` arrays at all. Without this such a task exists but is unreachable, since
    // both the layout and the DONE panel walk `children`.
    void repairAfterLoad();

private:
    void detachFromParent(TaskId child); // remove id from its current parent/roots list
    void collectSubtree(TaskId id, std::vector<TaskId>& out) const;
};

// Deep equality: same ids, same nextId, same roots order, and every Task field (including
// children order) identical. Deliberately exact — a store migration uses it to prove a
// round-trip lost nothing, so "close enough" would defeat the point.
bool equivalent(const Forest& a, const Forest& b);

// Every Note field identical. Shared by equivalent() and the SQLite save's row diff.
bool sameNote(const Note& a, const Note& b);

} // namespace tt
