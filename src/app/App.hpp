#pragma once
// Application state machine + orchestration. Owns the interaction state (mode,
// text input, drag) and drives model -> layout -> render. GLFW input is forwarded
// in via the on*() methods (main installs the trampolines).

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/Config.hpp"
#include "app/Palette.hpp"
#include "layout/Geometry.hpp"
#include "layout/TidyLayout.hpp"
#include "llm/IClassifier.hpp"
#include "model/History.hpp"
#include "model/Task.hpp"
#include "platform/IPlatform.hpp"
#include "render/Renderer.hpp"
#include "ui/DragController.hpp"
#include "ui/TextInput.hpp"

namespace tt {

namespace store { class Session; }

class App {
public:
    App(IPlatform& platform, Renderer& renderer, IClassifier& classifier,
        Config& cfg, Forest& forest, store::Session& session);

    // Hotkey actions.
    void toggleOverlay();   // show/hide the full overlay
    void showQuickAdd();    // show just the input box
    void hide();

    // GLFW input (forwarded by main).
    void onChar(unsigned int codepoint);
    void onKey(int key, int action, int mods);
    void onMouseButton(int button, int action, int mods);
    void onCursorPos(double x, double y);
    void onCursorEnter(bool entered);
    void onScroll(double dx, double dy);

    // Called from a classifier worker thread; enqueues a result and wakes the loop.
    void pushClassification(TaskId newTask, ClassifyResult result);

    // Main-loop hooks.
    void applyPendingClassifications();
    void pollStore();   // notice another process's store writes and reload
    void drawScene(int winW, int winH, float devicePixelRatio);
    bool visible() const { return mode_ != Mode::Hidden; }
    double desiredTimeout() const;   // <0 => block indefinitely

private:
    enum class Mode { Hidden, Full, QuickAdd };

    void relayoutIfNeeded();
    void forceRelayout() { needsRelayout_ = true; }
    void commitInput();
    void flashPath(TaskId leaf);   // briefly highlight root -> leaf after an add/classify
    // Current canvas tree (excluding DONE and `exclude`) as an indented "[id] text"
    // outline for the classifier.
    std::string buildTreeOutline(TaskId exclude) const;
    void save();
    void reloadFromStore();  // replace forest_ with disk and re-derive dependent state
    void undo();   // Ctrl+Z: restore the previous forest state
    void redo();   // Ctrl+Shift+Z / Ctrl+Y: reapply an undone state
    void afterHistoryChange();  // shared relayout/cleanup after an undo or redo
    void deleteSelected();      // Delete/Backspace: remove the selected subtree (undoable)
    // Ctrl+click reparent: select a node, then Ctrl+click another to move the selection
    // (and its subtree) in as that node's last child. Drag-free alternative to dragging.
    void   reparentSelected(TaskId newParent);   // undoable move; no-op if invalid
    TaskId reparentTargetAt(Vec2 world) const;   // valid target under `world` (0 = none)
    void   updateReparentCue(bool ctrlHeld);     // refresh the green target ring
    bool   ctrlHeld() const;                     // either Ctrl key down right now
    // In-place text editing: the input bar is reused to edit an existing node's text.
    // editingNode_ != 0 routes a commit to update that node instead of adding a task.
    void startEditing(TaskId id);          // seed the input bar with the node's text
    void commitEdit(const std::string& txt); // apply the edit (or cancel if empty/unchanged)
    void cancelEditing();                  // Esc: abandon the edit, leave the node as-is
    TaskId hitTest(Vec2 p) const;
    TaskId collapseHandleHit(Vec2 p) const;  // node whose collapse handle contains p (0 = none)
    void   toggleCollapse(TaskId id);        // flip a node's collapsed flag + relayout
    bool caretOn() const;
    DragVisual buildDragVisual();

    // Screen cursor -> canvas/world coordinates (accounts for pan + zoom).
    Vec2 worldMouse() const {
        return {(mouse_.x - pan_.x) / zoom_, (mouse_.y - pan_.y) / zoom_};
    }
    void moveOverlayToNextMonitor();

    // DONE panel helpers.
    void layoutDonePanel(int winW, int winH);
    bool pointInPanel(Vec2 p) const;
    TaskId hitTestDone(Vec2 p) const;
    void handleDoubleClick();

    IPlatform&      platform_;
    Renderer&       renderer_;
    IClassifier&    classifier_;
    Config&         cfg_;
    Forest&         forest_;
    store::Session& session_;   // the held store connection (saves + external-change poll)

    Mode mode_ = Mode::Hidden;
    TextInput input_;
    DragController drag_;
    LayoutParams params_;
    History history_;   // undo/redo snapshots of forest_
    // What the store last confirmed on disk — the baseline every save diffs against, so
    // each write carries only the change AND a node missing from forest_ is known to have
    // been deleted here rather than added by another writer. Seeded with the loaded forest.
    Forest lastSaved_;
    double lastStorePoll_ = 0.0;   // glfwGetTime() of the last external-change check

    // Command palette: the input bar itself, re-purposed by a leading ? / : / > (grammar +
    // match ranking in app/Palette.hpp). Ctrl+F is just "prefix the bar with ?" — there is
    // no second field. Highlights every match on canvas and previews the pending target.
    palette::Mode pmode_ = palette::Mode::Add;  // what the bar is doing (prefix consumed)
    palette::Command cmd_;                // resolved action for (pmode_, bar text)
    std::vector<TaskId> candidates_;      // ranked matches for a text-picking command
    std::vector<palette::Mode> menuItems_;   // rows of the '/' mode menu
    std::size_t candidateIdx_ = 0;        // which row ↑/↓ has landed on (nodes or modes)
    std::string lastQuery_;               // query the cursor above belongs to
    std::unordered_set<TaskId> searchHits_;  // every match (amber rings)
    TaskId previewSelect_ = 0;            // ring previewing what Enter would select
    double searchPanDue_ = 0.0;           // debounced jump to the active candidate (0 = none)
    void   updatePalette();               // re-resolve the bar; refresh candidates + previews
    void   clearPalette();                // empty the bar, back to plain add mode
    void   setPaletteMode(palette::Mode m);  // switch mode, clearing the argument
    bool   tryEnterMode(unsigned int codepoint);  // a prefix typed into an empty bar
    void   enterFindMode();               // Ctrl+F: the same as typing '?'
    void   movePaletteCursor(int delta);  // ↑/↓ through the candidate list
    void   runCommand();                  // Enter on a palette command
    TaskId activeCandidate() const;       // candidates_[candidateIdx_] (0 if none)
    TaskId commandTarget() const;         // the node the current command points at (0 = none)
    bool   canReparent(TaskId child, TaskId newParent) const;  // shared move validity
    void   revealNode(TaskId id);         // un-collapse every ancestor so `id` is on canvas
    bool   nodeOnScreen(TaskId id) const; // its rect is at least partly inside the window
    palette::Mode menuHighlight() const;  // mode the '/' picker is sitting on
    Color  paletteTint() const;           // per-mode accent (matches the canvas rings)
    std::string paletteStatus() const;    // "7 matches" / "node 12" / "no match" …
    void   drawPaletteDropUp(const Rect& inputBox);  // candidate list above the bar

    std::unordered_map<TaskId, Size> sizes_;
    std::unordered_map<TaskId, Rect> rects_;        // current layout
    std::unordered_map<TaskId, Rect> previewRects_; // layout during a drag
    Vec2 mouse_;
    bool needsRelayout_ = true;
    TaskId selected_ = 0;   // click-selected canvas node (0 = none); drawn with a ring
    TaskId editingNode_ = 0; // node whose text the input bar is editing (0 = adding, not editing)
    TaskId reparentTarget_ = 0; // node a Ctrl+click would reparent the selection under (0 = none)

    // Input-bar text selection (task 188): the box rect + the exact style are captured each
    // frame so a click can be hit-tested against the rendered layout; textSelecting_ is set
    // while a left-drag inside the bar is sweeping out a selection.
    Rect inputBox_;
    InputStyle lastInputStyle_;
    bool textSelecting_ = false;
    // Caret blink phase origin: reset on every caret move so the bar is solid the instant
    // you click or arrow, then resumes blinking (see caretOn()).
    double caretBlinkBase_ = 0.0;
    void resetCaretBlink();

    // "Hold this node still" anchor, consumed by the next relayout in drawScene: pan_ is
    // shifted so `anchorNode_` lands back on `anchorScreen_`. A Ctrl+click reparent re-packs
    // the tree, which would otherwise slide the clicked parent out from under the cursor.
    TaskId anchorNode_ = 0;
    Vec2   anchorScreen_;

    // Transient "path to the new task" flash (root -> leaf), fades over ~1.6 s.
    std::unordered_set<TaskId> highlightSet_;
    double highlightUntil_ = 0.0;

    // Ctrl+C on a selected node copies its text and shows a brief "Copied" pill above it
    // (task 186). copiedToastNode_ anchors the pill; copiedToastUntil_ is when it expires.
    void copySelectedNode();
    TaskId copiedToastNode_ = 0;
    double copiedToastUntil_ = 0.0;
    std::string toastText_;   // what the pill says ("Copied", "Moved to work", …)
    void showToast(std::string text, TaskId anchor = 0);   // anchor 0 = upper-centre

    // Workspaces. A workspace is a marked task (Task::workspaceAt); entering one shows only
    // its subtree, and the canvas, DONE panel, search, new tasks and the classifier outline
    // all scope to it. `view_` is that task (kMainView = the whole forest). Which view is
    // showing is view state, persisted in the store's meta (Session::currentView) rather
    // than the Forest, so undo never moves you. Each view remembers its own camera.
    TaskId view_ = kMainView;
    struct View { Vec2 pan; float zoom = 1.f; };
    std::unordered_map<TaskId, View> views_;
    // Set by a view change; the next frame (after relayout) recentres the camera if not one
    // of the view's nodes is on screen. A remembered view can go stale — the tree changed
    // under it, or the window/monitor did — and an empty canvas reads as "my tasks are gone".
    bool verifyView_ = false;
    // Enter/leave transition: nodes present in both layouts glide from where they were on
    // screen to where they land; nodes only in the old one fade out, new ones fade in.
    bool transActive_ = false;
    double transStart_ = 0.0;
    std::unordered_map<TaskId, Rect> transFrom_;   // SCREEN rects at the moment of switching
    std::unordered_map<TaskId, Rect> animRects_;   // this frame's in-between world rects
    std::unordered_map<TaskId, float> fade_;       // this frame's per-node opacity
    void   buildTransitionFrame();                 // fills animRects_/fade_ (or ends it)

    std::vector<TaskId> wsCands_;      // '@' rows; kMainView (0) is the "main" row
    bool viewMenuOpen_ = false;        // the top-left picker's list is showing
    ViewPickerLayout picker_;          // last frame's picker geometry (for hit tests)
    std::vector<TaskId> pickerViews_;  // the view each picker row opens (kMainView first)
    void   enterView(TaskId view);             // animated switch (no-op if already there)
    void   viewUp();                           // Ctrl+Up: the enclosing workspace, or main
    void   viewDown();                         // Ctrl+Down: the selection's workspace
    void   toggleWorkspace();                  // Ctrl+T: mark/unmark the selected node
    void   createWorkspace(const std::string& name);  // new node in this view, marked, entered
    void   ensureView();                       // leave a view whose node is gone/done/unmarked
    bool   inView(TaskId id) const;            // is `id` inside the current view's subtree?
    std::string viewName(TaskId view) const;   // "main" or the workspace's text
    std::string breadcrumb() const;            // "main › work › amqp"
    std::vector<std::pair<TaskId, int>> workspaceOutline() const;  // tree order + depth
    bool   hasWsCandidate() const { return candidateIdx_ < wsCands_.size(); }
    std::size_t paletteRows() const;           // rows in whichever drop-up list is open
    void   layoutViewPicker(int winW);
    bool   handleViewPickerClick();            // true if the click belonged to the picker

    // Canvas view transform (screen = pan + zoom * world). Screen-space UI is unaffected.
    Vec2  pan_;
    float zoom_ = 1.f;
    TaskId focusNode_ = 0;   // centre this node on the next frame (0 = none); glides there

    // Smooth camera glide shared by the search auto-pan and the new-node pan: pan_ eases
    // from panFrom_ to panTo_ over kPanAnimDur (ease-out cubic); the loop wakes ~60fps
    // meanwhile. Any manual pan/zoom pre-empts it via cancelPanAnim().
    void  startPanTo(Vec2 target);        // begin a glide (snaps instead if already ~there)
    void  cancelPanAnim() { panAnimActive_ = false; }
    bool  panAnimActive_ = false;
    Vec2  panFrom_, panTo_;
    double panAnimStart_ = 0.0;

    bool  panning_ = false;
    Vec2  panGrab_;     // screen cursor at pan start
    Vec2  panOrigin_;   // pan_ at pan start

    // DONE side panel state.
    bool  pinned_ = false;           // autohide off when true
    bool  doneHover_ = false;        // mouse in the reveal/keep zone (autohide)
    float scrollY_ = 0.f;
    float doneMaxScroll_ = 0.f;
    int   lastWinW_ = 0, lastWinH_ = 0;
    DonePanelLayout donePanel_;
    std::vector<DoneRow> doneRows_;               // visible, indented DONE rows
    std::unordered_set<TaskId> doneExpanded_;     // which DONE nodes show their children
    double lastClickTime_ = 0.0;
    Vec2   lastClickPos_;

    // Description card: the selected canvas node's description, bottom-left (see
    // Renderer::layoutDescCard). Ctrl+D collapses it (persisted in the store's meta, so
    // undo never toggles it); Ctrl+E or a click on its body edits in place, in its own
    // multi-line TextInput — the input bar keeps its meaning. While the card edits,
    // keyboard focus is the card's: canvas and palette keys are inert, Ctrl+Z steps the
    // card's text history, and store reloads are deferred.
    TextInput descInput_;
    TaskId descEditing_ = 0;       // node whose description the card is editing (0 = none)
    bool   descCollapsed_ = false;
    float  descScroll_ = 0.f;
    TaskId descScrollNode_ = 0;    // the node descScroll_ belongs to (reset on a change)
    bool   descSelecting_ = false; // a left-drag in the card body is sweeping a selection
    float  descColumnX_ = -1.f;    // Up/Down's remembered column (-1 = take the caret's)
    DescCardLayout descCard_;      // last frame's geometry, for hit tests
    TaskId descNode() const;       // the node the card shows (0 = no card)
    DescCardLayout layoutDescFor(TaskId id) const;   // current geometry, current text
    void   toggleDescCollapsed();
    void   startDescEdit();
    void   commitDescEdit();
    void   cancelDescEdit();
    void   handleDescKey(int key, int mods);
    std::size_t descByteAt(const DescCardLayout& L, Vec2 p) const;

    // Note cards. A note is a card on the canvas (world coordinates, so it pans and zooms
    // with the tasks), drawn over the tree in the view it belongs to. A click on its title or
    // text edits that field in place; dragging the header moves the card; × closes it (kept:
    // '#' opens it again). While a card edits, keyboard focus is the card's, as for the
    // description card. Ctrl+N makes a new note at the cursor.
    TextInput noteTitleInput_;          // single line
    TextInput noteBodyInput_;           // multi-line
    NoteId    noteEditing_ = 0;         // card being edited (0 = none)
    NoteField noteField_ = NoteField::None;
    bool      noteIsNew_ = false;       // made by Ctrl+N or '#', not committed yet
    Forest    noteBefore_;              // undo checkpoint from before the new note existed
    bool      noteSelecting_ = false;   // a left-drag in the edited field sweeps a selection
    // How far an edited long title is shifted left. It moves only to keep the caret in view
    // (keepNoteTitleCaretVisible), never as a side effect of a click, so a click or a drag
    // maps the cursor through the same offset that was drawn.
    float     noteTitleScroll_ = 0.f;
    void      keepNoteTitleCaretVisible(const NoteCardLayout& L);
    float     noteColumnX_ = -1.f;      // Up/Down's remembered column in the body
    NoteId    noteDrag_ = 0;            // card whose header is held (0 = none)
    bool      noteDragMoved_ = false;   // past the click threshold: a move, not a click
    Vec2      noteDragGrab_;            // world cursor at the press
    Vec2      noteDragOrigin_;          // the card's x, y at the press
    Vec2      noteDragScreen_;          // screen cursor at the press (click threshold)
    Forest    noteDragBefore_;          // undo checkpoint for the move
    std::vector<NoteCardLayout> noteCards_;   // this frame's visible cards, bottom to top
    std::vector<NoteId> noteCands_;     // rows of the '#' list
    TextInput& noteInput() { return noteField_ == NoteField::Title ? noteTitleInput_ : noteBodyInput_; }
    bool   noteVisible(const Note& n) const;              // open, and in the current view
    NoteCardLayout layoutNote(NoteId id) const;           // current geometry, editor text included
    const NoteCardLayout* noteCardAt(Vec2 world) const;   // topmost visible card under a point
    std::size_t noteByteAt(const NoteCardLayout& L, NoteField field, Vec2 world) const;
    bool   overScreenUi() const;        // the cursor is on the bar, a panel or a card above the canvas
    void   createNote(const std::string& title, Vec2 world);   // new card, its title or text editing
    void   startNoteEdit(NoteId id, NoteField field);
    void   commitNoteEdit();
    void   cancelNoteEdit();
    void   endNoteDrag();               // record a finished move (undo + save)
    void   handleNoteKey(int key, int mods);
    bool   handleNoteMouse(int button, int action, int mods);  // true if a card took the event
    void   closeNote(NoteId id);
    void   deleteEditedNote();          // Ctrl+Shift+Delete while editing (undoable)
    void   openNote(NoteId id);         // '#': open it, go to its view, bring it on screen
    bool   hasNoteCandidate() const { return candidateIdx_ < noteCands_.size(); }

    std::mutex pendingMutex_;
    std::vector<std::pair<TaskId, ClassifyResult>> pending_;
};

} // namespace tt
