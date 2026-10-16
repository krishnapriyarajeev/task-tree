// Dependency-free verification of the single-line text field's editing + selection
// model (src/ui/TextInput.cpp). Built by CMake as the `textinput_tests` CTest target.
// Uses the real GLFW key constants (header-only) so the codes match the app exactly.
#include "ui/TextInput.hpp"
#include "ui/TextRows.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

using namespace tt;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }                \
    } while (0)

// Convenience: press `key` with modifiers, type text, etc.
static void key(TextInput& t, int k, int mods = 0) { t.onKey(k, mods); }
static void type(TextInput& t, const std::string& s) { for (char c : s) t.onChar((unsigned char)c); }

static TextInput seeded(const std::string& s) {
    TextInput t;
    t.setText(s);   // caret at end, no selection
    return t;
}

static void testCaretMotion() {
    std::printf("[textinput] caret motion\n");
    TextInput t = seeded("hello world");
    CHECK(t.caret() == 11, "setText puts caret at end");
    CHECK(!t.hasSelection(), "setText clears selection");

    key(t, GLFW_KEY_HOME);
    CHECK(t.caret() == 0, "Home -> 0");
    key(t, GLFW_KEY_RIGHT);
    CHECK(t.caret() == 1, "Right -> 1");
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_CONTROL);
    CHECK(t.caret() == 6, "Ctrl+Right jumps to start of 'world'");
    key(t, GLFW_KEY_LEFT, GLFW_MOD_CONTROL);
    CHECK(t.caret() == 0, "Ctrl+Left jumps back to start of 'hello'");
    key(t, GLFW_KEY_END);
    CHECK(t.caret() == 11, "End -> size");
    CHECK(!t.hasSelection(), "plain motion never selects");
}

static void testSelectAllAndCopyText() {
    std::printf("[textinput] select all\n");
    TextInput t = seeded("abcdef");
    t.selectAll();
    CHECK(t.hasSelection(), "selectAll selects");
    CHECK(t.selBegin() == 0 && t.selEnd() == 6, "selectAll spans whole text");
    CHECK(t.selectedText() == "abcdef", "selectedText is the whole string");
    // caret at end, plain Left collapses to the near (left) edge.
    key(t, GLFW_KEY_LEFT);
    CHECK(!t.hasSelection() && t.caret() == 0, "plain Left collapses to selBegin");

    t.selectAll();
    key(t, GLFW_KEY_RIGHT);
    CHECK(!t.hasSelection() && t.caret() == 6, "plain Right collapses to selEnd");
}

static void testShiftExtend() {
    std::printf("[textinput] shift extends\n");
    TextInput t = seeded("hello world");
    key(t, GLFW_KEY_HOME);
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT);
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT);
    CHECK(t.hasSelection(), "shift+right selects");
    CHECK(t.selBegin() == 0 && t.selEnd() == 2, "two shift+rights -> [0,2)");
    CHECK(t.selectedText() == "he", "selected 'he'");
    key(t, GLFW_KEY_RIGHT, GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);
    CHECK(t.selEnd() == 6, "Ctrl+Shift+Right extends by word to 6");
    CHECK(t.selectedText() == "hello ", "selected up to start of 'world'");
    key(t, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);
    CHECK(t.selEnd() == 5, "shift+left shrinks selection back to 5");

    // Anchor stays fixed while the caret sweeps past it and back.
    TextInput u = seeded("abcdef");
    key(u, GLFW_KEY_HOME);
    key(u, GLFW_KEY_RIGHT); key(u, GLFW_KEY_RIGHT); key(u, GLFW_KEY_RIGHT); // caret at 3, no sel
    key(u, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);   // anchor 3, caret 2
    key(u, GLFW_KEY_LEFT, GLFW_MOD_SHIFT);   // caret 1
    CHECK(u.selBegin() == 1 && u.selEnd() == 3, "leftward selection [1,3)");
    CHECK(u.selectedText() == "bc", "selected 'bc' backwards");
}

static void testReplaceOnType() {
    std::printf("[textinput] typing replaces selection\n");
    TextInput t = seeded("hello world");
    key(t, GLFW_KEY_HOME);
    for (int i = 0; i < 5; ++i) key(t, GLFW_KEY_RIGHT, GLFW_MOD_SHIFT); // select "hello"
    CHECK(t.selectedText() == "hello", "selected the first word");
    type(t, "Hi");
    CHECK(t.text() == "Hi world", "typing replaced the selection");
    CHECK(!t.hasSelection() && t.caret() == 2, "caret after inserted text, no selection");
}

static void testDeleteBackspaceSelection() {
    std::printf("[textinput] delete/backspace remove selection\n");
    TextInput t = seeded("hello world");
    t.selectAll();
    key(t, GLFW_KEY_BACKSPACE);
    CHECK(t.text().empty(), "backspace deletes whole selection");
    CHECK(!t.hasSelection() && t.caret() == 0, "empty, caret 0");

    TextInput u = seeded("hello world");
    u.moveCaretTo(0, false);
    u.moveCaretTo(5, true);   // select "hello"
    key(u, GLFW_KEY_DELETE);
    CHECK(u.text() == " world", "delete removes selection only");
    CHECK(u.caret() == 0, "caret at deletion point");
}

static void testPasteReplacesSelection() {
    std::printf("[textinput] paste replaces selection\n");
    TextInput t = seeded("hello world");
    t.moveCaretTo(0, false);
    t.moveCaretTo(5, true);   // select "hello"
    t.insert("goodbye");
    CHECK(t.text() == "goodbye world", "paste replaced selection");
    CHECK(!t.hasSelection(), "no selection after paste");
    // Control chars in a paste are still stripped.
    TextInput u = seeded("");
    u.insert("a\nb\tc");
    CHECK(u.text() == "abc", "paste strips control chars");
}

static void testMouseCaretAndWord() {
    std::printf("[textinput] mouse caret + word select\n");
    TextInput t = seeded("hello world");
    t.moveCaretTo(3, false);
    CHECK(t.caret() == 3 && !t.hasSelection(), "click places caret, clears selection");
    t.moveCaretTo(8, true);
    CHECK(t.selBegin() == 3 && t.selEnd() == 8, "drag extends [3,8)");
    CHECK(t.selectedText() == "lo wo", "dragged selection text");

    t.selectWordAt(7);   // inside "world"
    CHECK(t.selectedText() == "world", "double-click selects the word");
    t.selectWordAt(2);   // inside "hello"
    CHECK(t.selectedText() == "hello", "double-click selects a different word");

    // Clamping past the end.
    t.moveCaretTo(999, false);
    CHECK(t.caret() == 11, "moveCaretTo clamps to size");
}

static void testUtf8Boundaries() {
    std::printf("[textinput] utf-8 boundaries\n");
    // "é" is 2 bytes (0xC3 0xA9); "…" is 3 bytes. String: "aé…b" -> bytes: a(1) é(2) …(3) b(1) = 7 bytes.
    TextInput t = seeded("a\xC3\xA9\xE2\x80\xA6"
                         "b");
    CHECK(t.text().size() == 7, "byte length 7");
    // A click landing mid-codepoint snaps to the boundary (start of "é" at byte 1).
    t.moveCaretTo(2, false);
    CHECK(t.caret() == 1, "click inside 'é' snaps to its start");
    // Word-select over the whole non-space run.
    t.selectWordAt(0);
    CHECK(t.selectedText() == "a\xC3\xA9\xE2\x80\xA6" "b", "word covers the multibyte run");
}

// A fake clock for the undo coalescing window (TextInput::setClock).
static double g_now = 100.0;
static TextInput clocked(const std::string& s = "") {
    TextInput t;
    t.setClock([] { return g_now; });
    t.setText(s);
    return t;
}

static void testUndoCoalescing() {
    std::printf("[textinput] undo: typing coalesces into word-sized steps\n");
    TextInput t = clocked();
    CHECK(!t.canUndo(), "fresh field has nothing to undo");
    type(t, "hello");
    type(t, " world");
    CHECK(t.text() == "hello world", "typed");
    CHECK(t.undo() && t.text() == "hello", "undo removes the second word, keeps the first");
    CHECK(t.caret() == 5, "caret restored with the text");
    CHECK(t.redo() && t.text() == "hello world", "redo restores it");
    CHECK(!t.redo(), "nothing further to redo");
    CHECK(t.undo() && t.undo() && t.text().empty(), "second undo removes the first word");
    CHECK(!t.undo(), "bottom of the stack");

    // A pause breaks a burst even mid-word.
    TextInput p = clocked();
    type(p, "ab");
    g_now += 1.5;
    type(p, "cd");
    CHECK(p.undo() && p.text() == "ab", "a pause starts a new step");

    // A caret move breaks it too.
    TextInput m = clocked();
    type(m, "abc");
    key(m, GLFW_KEY_LEFT);
    type(m, "X");
    CHECK(m.text() == "abXc", "typed after moving");
    CHECK(m.undo() && m.text() == "abc", "the post-move typing is its own step");

    // A new edit after an undo clears redo.
    TextInput r = clocked();
    type(r, "one two");
    r.undo();
    type(r, "!");
    CHECK(!r.redo(), "a fresh edit forks history: redo cleared");
}

static void testUndoEditKinds() {
    std::printf("[textinput] undo: every mutation is undoable, one step each\n");
    TextInput t = clocked("hello world");
    key(t, GLFW_KEY_BACKSPACE);
    CHECK(t.text() == "hello worl", "backspace");
    CHECK(t.undo() && t.text() == "hello world", "backspace undone");
    key(t, GLFW_KEY_BACKSPACE, GLFW_MOD_CONTROL);
    CHECK(t.undo() && t.text() == "hello world", "ctrl+backspace undone");
    key(t, GLFW_KEY_HOME);
    key(t, GLFW_KEY_DELETE);
    CHECK(t.undo() && t.text() == "hello world", "delete undone");
    t.selectAll();
    t.insert("pasted");
    CHECK(t.text() == "pasted" && t.undo() && t.text() == "hello world", "paste undone");
    t.selectAll();
    t.deleteSelection();
    CHECK(t.undo() && t.text() == "hello world", "cut undone");
    CHECK(t.hasSelection(), "selection restored with the text");
    // Motion alone records nothing.
    TextInput m = clocked("abc");
    key(m, GLFW_KEY_LEFT);
    key(m, GLFW_KEY_HOME);
    CHECK(!m.canUndo(), "caret motion is not an edit");
    // setText / clear reset the stack.
    type(m, "x");
    m.setText("new");
    CHECK(!m.canUndo(), "setText resets history");
}

static void testMultiline() {
    std::printf("[textinput] multi-line mode\n");
    TextInput t = clocked("ab");
    CHECK(t.onKey(GLFW_KEY_ENTER, 0) == TextInput::Action::Submit, "single-line: Enter submits");
    t.setMultiline(true);
    CHECK(t.onKey(GLFW_KEY_ENTER, 0) == TextInput::Action::None, "multi-line: Enter is an edit");
    CHECK(t.text() == "ab\n", "Enter inserted a newline");
    CHECK(t.onKey(GLFW_KEY_ENTER, GLFW_MOD_CONTROL) == TextInput::Action::Submit,
          "Ctrl+Enter submits");
    CHECK(t.onKey(GLFW_KEY_KP_ENTER, GLFW_MOD_CONTROL) == TextInput::Action::Submit,
          "keypad Ctrl+Enter too");
    t.insert("x\r\ny\tz");
    CHECK(t.text() == "ab\nx\ny\tz", "paste keeps newlines (CRLF folded) and tabs");
    TextInput s = clocked();
    s.insert("x\ny");
    CHECK(s.text() == "xy", "single-line paste still drops newlines");
}

static void testTextRows() {
    std::printf("[textrows] multi-line row mapping\n");
    // "ab\ncd ef\n" wrapped as: [ab] [cd] [ef] [] — a soft wrap's row ends before the space
    // it broke at (end < next, as NanoVG reports it); the final empty row follows the
    // trailing newline. Monospace 10px per byte for the advance function.
    const std::string text = "ab\ncd ef\n";
    std::vector<TextRow> rows = {{0, 2, 3}, {3, 5, 6}, {6, 8, 9}};
    finishRows(rows, text);
    CHECK(rows.size() == 4 && rows[3].start == 9 && rows[3].end == 9, "trailing empty row added");
    std::vector<TextRow> none;
    finishRows(none, "");
    CHECK(none.size() == 1 && none[0].start == 0, "empty text has one empty row");

    CHECK(rowOfByte(rows, 0) == 0, "start -> row 0");
    CHECK(rowOfByte(rows, 2) == 0, "before the newline -> row 0");
    CHECK(rowOfByte(rows, 3) == 1, "after the newline -> row 1");
    CHECK(rowOfByte(rows, 6) == 2, "a soft-wrap boundary belongs to the next row");
    CHECK(rowOfByte(rows, 8) == 2, "end of row 2");
    CHECK(rowOfByte(rows, 9) == 3, "after the trailing newline -> the empty row");

    const auto adv = [](int from, int to) { return 10.f * static_cast<float>(to - from); };
    CHECK(byteAtX(text, rows[1], 0.f, adv) == 3, "x 0 -> row start");
    CHECK(byteAtX(text, rows[1], 14.f, adv) == 4, "nearest glyph edge (rounds down)");
    CHECK(byteAtX(text, rows[1], 16.f, adv) == 5, "nearest glyph edge (rounds up)");
    CHECK(byteAtX(text, rows[1], 999.f, adv) == 5, "past the end -> row end");

    CHECK(verticalMove(text, rows, 4, +1, 10.f, adv) == 7, "down keeps the column");
    CHECK(verticalMove(text, rows, 7, -1, 10.f, adv) == 4, "up keeps the column");
    CHECK(verticalMove(text, rows, 1, +1, 999.f, adv) == 5, "down clamps to a shorter row");
    CHECK(verticalMove(text, rows, 1, -1, 10.f, adv) == 0, "up on the first row -> start");
    CHECK(verticalMove(text, rows, 9, +1, 0.f, adv) == 9, "down on the last row -> end");
    CHECK(rowHome(rows, 7) == 6 && rowEnd(rows, 7) == 8, "home/end of the caret's row");
    CHECK(rowHome(rows, 2) == 0 && rowEnd(rows, 2) == 2, "home/end before a newline");

    // UTF-8: never land inside a code point.
    const std::string u = "é\né";   // 2 bytes, newline, 2 bytes
    std::vector<TextRow> ur = {{0, 2, 3}, {3, 5, 5}};
    finishRows(ur, u);
    CHECK(byteAtX(u, ur[1], 9.f, adv) == 3 || byteAtX(u, ur[1], 9.f, adv) == 5,
          "byteAtX stops on code-point boundaries");
}

int main() {
    testCaretMotion();
    testSelectAllAndCopyText();
    testShiftExtend();
    testReplaceOnType();
    testDeleteBackspaceSelection();
    testPasteReplacesSelection();
    testMouseCaretAndWord();
    testUtf8Boundaries();
    testUndoCoalescing();
    testUndoEditKinds();
    testMultiline();
    testTextRows();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
