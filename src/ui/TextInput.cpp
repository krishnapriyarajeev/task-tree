#include "ui/TextInput.hpp"

#include <algorithm>
#include <chrono>

#include <GLFW/glfw3.h>

namespace tt {
namespace {

// Append the UTF-8 encoding of `cp` to `s`.
void appendUtf8(std::string& s, unsigned int cp) {
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }
bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

} // namespace

std::size_t TextInput::prevBoundary(std::size_t i) const {
    if (i == 0) return 0;
    --i;
    while (i > 0 && isContinuation(static_cast<unsigned char>(text_[i]))) --i;
    return i;
}

std::size_t TextInput::nextBoundary(std::size_t i) const {
    if (i >= text_.size()) return text_.size();
    ++i;
    while (i < text_.size() && isContinuation(static_cast<unsigned char>(text_[i]))) ++i;
    return i;
}

std::size_t TextInput::prevWordStart(std::size_t i) const {
    while (i > 0 && isWs(text_[i - 1])) --i;   // skip whitespace before the caret
    while (i > 0 && !isWs(text_[i - 1])) --i;   // skip the word (multibyte-safe: word runs stop at whitespace)
    return i;
}

std::size_t TextInput::nextWordEnd(std::size_t i) const {
    const std::size_t n = text_.size();
    while (i < n && isWs(text_[i])) ++i;        // skip whitespace after the caret
    while (i < n && !isWs(text_[i])) ++i;       // skip the word
    return i;
}

std::size_t TextInput::nextWordStart(std::size_t i) const {
    const std::size_t n = text_.size();
    while (i < n && !isWs(text_[i])) ++i;       // skip rest of the current word
    while (i < n && isWs(text_[i])) ++i;        // then whitespace -> start of next word
    return i;
}

std::size_t TextInput::alignBoundary(std::size_t i) const {
    if (i >= text_.size()) return text_.size();
    while (i > 0 && isContinuation(static_cast<unsigned char>(text_[i]))) --i;
    return i;
}

std::pair<std::size_t, std::size_t> TextInput::wordBounds(std::size_t i) const {
    const std::size_t n = text_.size();
    if (n == 0) return {0, 0};
    if (i >= n) i = n - 1;                        // inspect a real char at the edge
    const bool ws = isWs(text_[i]);              // select the run of the same class
    std::size_t b = i, e = i;
    while (b > 0 && isWs(text_[b - 1]) == ws) --b;
    while (e < n && isWs(text_[e]) == ws) ++e;
    return {b, e};
}

double TextInput::now() const {
    if (now_) return now_();
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void TextInput::beginEdit(EditKind kind, bool whitespace) {
    const double t = now();
    // A typed character continues the burst when the last edit was typing, recent, left
    // the caret exactly here, and this character isn't whitespace (a space starts the
    // next word's step, so undo takes back " world" and leaves "hello").
    const bool continues = kind == EditKind::Typing && lastKind_ == EditKind::Typing &&
                           !whitespace && !hasSelection() && caret_ == lastCaret_ &&
                           t - lastEditAt_ < kCoalesceGap;
    if (!continues) {
        undo_.push_back({text_, caret_, anchor_});
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
    redo_.clear();
    lastKind_ = kind;
    lastEditAt_ = t;
}

bool TextInput::undo() {
    if (undo_.empty()) return false;
    redo_.push_back({text_, caret_, anchor_});
    const Snap s = std::move(undo_.back());
    undo_.pop_back();
    text_ = s.text; caret_ = s.caret; anchor_ = s.anchor;
    lastKind_ = EditKind::None;   // the next keystroke starts a fresh step
    return true;
}

bool TextInput::redo() {
    if (redo_.empty()) return false;
    undo_.push_back({text_, caret_, anchor_});
    const Snap s = std::move(redo_.back());
    redo_.pop_back();
    text_ = s.text; caret_ = s.caret; anchor_ = s.anchor;
    lastKind_ = EditKind::None;
    return true;
}

void TextInput::deleteSelection() {
    if (!hasSelection()) return;
    beginEdit(EditKind::Other);
    eraseSelection();
}

void TextInput::eraseSelection() {
    const std::size_t b = selBegin(), e = selEnd();
    text_.erase(b, e - b);
    caret_ = anchor_ = b;
    lastCaret_ = caret_;
}

void TextInput::moveCaretTo(std::size_t byte, bool extend) {
    caret_ = alignBoundary(std::min(byte, text_.size()));
    if (!extend) anchor_ = caret_;
}

void TextInput::selectWordAt(std::size_t byte) {
    const auto [b, e] = wordBounds(std::min(byte, text_.size()));
    anchor_ = b;
    caret_ = e;
}

void TextInput::onChar(unsigned int codepoint) {
    beginEdit(EditKind::Typing, codepoint < 0x80 && isWs(static_cast<char>(codepoint)));
    if (hasSelection()) eraseSelection();
    std::string enc;
    appendUtf8(enc, codepoint);
    text_.insert(caret_, enc);
    caret_ += enc.size();
    anchor_ = caret_;
    lastCaret_ = caret_;
}

void TextInput::insert(const std::string& utf8) {
    // Drop control characters (e.g. newlines from a multi-line clipboard) — except, in
    // multi-line mode, newlines (CRLF / CR folded to LF) and tabs.
    std::string clean;
    clean.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const char c = utf8[i];
        if (multiline_ && c == '\r') {
            clean.push_back('\n');
            if (i + 1 < utf8.size() && utf8[i + 1] == '\n') ++i;
        } else if (static_cast<unsigned char>(c) >= 0x20 || (c & 0x80) ||
                   (multiline_ && (c == '\n' || c == '\t'))) {
            clean.push_back(c);
        }
    }
    if (clean.empty() && !hasSelection()) return;   // nothing would change: no undo step
    beginEdit(EditKind::Other);
    if (hasSelection()) eraseSelection();
    text_.insert(caret_, clean);
    caret_ += clean.size();
    anchor_ = caret_;
    lastCaret_ = caret_;
}

TextInput::Action TextInput::onKey(int key, int mods) {
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    const bool shift = (mods & GLFW_MOD_SHIFT) != 0;
    switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            if (multiline_ && !ctrl) { onChar('\n'); return Action::None; }
            return Action::Submit;
        case GLFW_KEY_ESCAPE:
            return Action::Cancel;
        case GLFW_KEY_BACKSPACE:
            if (hasSelection()) { deleteSelection(); break; }
            if (caret_ > 0) {
                beginEdit(EditKind::Other);
                const std::size_t from = ctrl ? prevWordStart(caret_) : prevBoundary(caret_);
                text_.erase(from, caret_ - from);
                caret_ = from;
            }
            anchor_ = caret_;
            break;
        case GLFW_KEY_DELETE:
            if (hasSelection()) { deleteSelection(); break; }
            if (caret_ < text_.size()) {
                beginEdit(EditKind::Other);
                const std::size_t to = ctrl ? nextWordEnd(caret_) : nextBoundary(caret_);
                text_.erase(caret_, to - caret_);
            }
            anchor_ = caret_;
            break;
        // Plain Left/Right with a selection collapse to its near edge; otherwise they move
        // by a glyph or a word. Shift keeps the anchor, so the selection grows/shrinks.
        case GLFW_KEY_LEFT:
            if (hasSelection() && !shift) caret_ = selBegin();
            else caret_ = ctrl ? prevWordStart(caret_) : prevBoundary(caret_);
            if (!shift) anchor_ = caret_;
            break;
        case GLFW_KEY_RIGHT:
            if (hasSelection() && !shift) caret_ = selEnd();
            else caret_ = ctrl ? nextWordStart(caret_) : nextBoundary(caret_);
            if (!shift) anchor_ = caret_;
            break;
        case GLFW_KEY_HOME:  caret_ = 0;             if (!shift) anchor_ = caret_; break;
        case GLFW_KEY_END:   caret_ = text_.size();  if (!shift) anchor_ = caret_; break;
        default: break;
    }
    return Action::None;
}

} // namespace tt
