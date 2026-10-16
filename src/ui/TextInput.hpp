#pragma once
// Single-line UTF-8 text field. Caret is a byte offset that always sits on a UTF-8
// code-point boundary. GLFW-agnostic API (onKey takes an int keycode) so the model
// is testable; the .cpp maps GLFW key constants internally.
//
// Selection: a second byte offset, the anchor, marks the other end of the highlighted
// range. The selection is [selBegin(), selEnd()); it is empty when anchor == caret.
// Shift+motion extends it, plain motion collapses it, and typing / paste / Backspace /
// Delete all replace it — i.e. the field behaves like a normal OS text box.
//
// Undo: every mutation pushes a {text, caret, anchor} snapshot first, so undo()/redo()
// step through the field's own history. Consecutive typed characters coalesce into one
// step — broken by a pause (kCoalesceGap), whitespace, a caret move, or any other kind of
// edit — so an undo takes back a word or a burst, not a character. The caller decides
// which keys reach undo() (the app routes Ctrl+Z here only while the description card
// edits; the input bar keeps Ctrl+Z as tree undo).
//
// Multi-line mode (the description card): Enter inserts '\n' and Ctrl+Enter submits;
// pasted newlines are kept. Single-line (the input bar, default) is unchanged.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tt {

class TextInput {
public:
    enum class Action { None, Submit, Cancel };

    void clear() { text_.clear(); caret_ = 0; anchor_ = 0; resetHistory(); }
    void setText(const std::string& s) {   // seed + caret to end; starts a fresh history
        text_ = s; caret_ = s.size(); anchor_ = caret_; resetHistory();
    }
    void setMultiline(bool m) { multiline_ = m; }
    bool focused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    void onChar(unsigned int codepoint);          // printable input (replaces any selection)
    Action onKey(int glfwKey, int glfwMods);      // editing / motion / submit / cancel keys
    void insert(const std::string& utf8);         // e.g. clipboard paste (replaces any selection)

    const std::string& text() const { return text_; }
    std::size_t caret() const { return caret_; }

    // Selection.
    bool hasSelection() const { return anchor_ != caret_; }
    std::size_t selBegin() const { return anchor_ < caret_ ? anchor_ : caret_; }
    std::size_t selEnd()   const { return anchor_ < caret_ ? caret_ : anchor_; }
    std::string selectedText() const { return text_.substr(selBegin(), selEnd() - selBegin()); }
    void selectAll() { anchor_ = 0; caret_ = text_.size(); }
    void deleteSelection();                            // erase it; caret ends at its start

    // Mouse-driven caret placement. `byte` is snapped to a code-point boundary. When
    // `extend` is true the anchor is kept (drag / shift-click), otherwise it collapses.
    void moveCaretTo(std::size_t byte, bool extend);
    void selectWordAt(std::size_t byte);               // double-click: the run around `byte`

    // The field's own undo/redo (see the header comment). Return false when there is
    // nothing to step to.
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // Seconds, monotonic — only differences matter. Tests inject a fake one.
    void setClock(std::function<double()> now) { now_ = std::move(now); }

    static constexpr double kCoalesceGap = 1.0;   // a pause this long starts a new step
    static constexpr std::size_t kMaxUndo = 200;

private:
    std::size_t prevBoundary(std::size_t i) const;
    std::size_t nextBoundary(std::size_t i) const;
    std::size_t prevWordStart(std::size_t i) const;  // Ctrl+Backspace / Ctrl+Left
    std::size_t nextWordEnd(std::size_t i) const;     // Ctrl+Delete
    std::size_t nextWordStart(std::size_t i) const;   // Ctrl+Right
    std::size_t alignBoundary(std::size_t i) const;   // round down to a code-point boundary
    std::pair<std::size_t, std::size_t> wordBounds(std::size_t i) const;
    void eraseSelection();   // deleteSelection without the undo step (callers took one)

    struct Snap { std::string text; std::size_t caret, anchor; };
    enum class EditKind { None, Typing, Other };
    // Push the pre-edit state unless this edit continues a typing burst.
    void beginEdit(EditKind kind, bool whitespace = false);
    void resetHistory() { undo_.clear(); redo_.clear(); lastKind_ = EditKind::None; }
    double now() const;

    std::vector<Snap> undo_, redo_;
    EditKind lastKind_ = EditKind::None;
    double lastEditAt_ = 0.0;
    std::size_t lastCaret_ = 0;     // caret after the last edit (a move breaks coalescing)
    std::function<double()> now_;   // empty = steady_clock
    bool multiline_ = false;

    std::string text_;
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;   // other end of the selection; == caret_ means no selection
    bool focused_ = false;
};

} // namespace tt
