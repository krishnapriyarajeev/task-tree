#pragma once
// Caret geometry for multi-line text (the description card's editor), kept pure: the
// renderer breaks the text into visual rows (nvgTextBreakLines, which splits on '\n' and
// on width) and hands the byte ranges here; glyph positions come in through an advance
// function, so caret placement, Up/Down and Home/End are unit-testable without GL.
//
// A row is [start, end) of visible bytes; `next` is where the following row starts. For a
// hard break `next` is past the '\n'; for a soft wrap `end` may stop before the space the
// line broke at. A byte equal to a row's `next` belongs to the FOLLOWING row, so the caret
// after a newline (or at a wrap point) shows at the start of the next line.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tt {

struct TextRow { int start = 0, end = 0, next = 0; };

// NanoVG emits no row for the empty line after a trailing '\n', nor any row for empty
// text — but the caret can sit there, so append an empty row in both cases.
inline void finishRows(std::vector<TextRow>& rows, const std::string& text) {
    const int n = static_cast<int>(text.size());
    if (rows.empty() || text.back() == '\n') rows.push_back({n, n, n});
}

// The row that shows the caret at `byte`.
inline int rowOfByte(const std::vector<TextRow>& rows, std::size_t byte) {
    const int b = static_cast<int>(byte);
    int found = static_cast<int>(rows.size()) - 1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (b >= rows[i].start && b <= rows[i].next) {
            found = i;
            if (b < rows[i].next) break;   // == next: keep looking, the next row owns it
        }
    }
    return std::max(0, found);
}

// The code-point boundary in `row` nearest to `x` (row-relative). `adv(from, to)` is the
// advance width of text[from, to). The usual caret rule: stop at the first boundary whose
// glyph midpoint lies right of x.
template <class Adv>
std::size_t byteAtX(const std::string& text, const TextRow& row, float x, const Adv& adv) {
    if (x <= 0.f) return static_cast<std::size_t>(row.start);
    int i = row.start;
    float prev = 0.f;
    while (i < row.end) {
        int j = i + 1;
        while (j < row.end && (static_cast<unsigned char>(text[j]) & 0xC0) == 0x80) ++j;
        const float a = adv(row.start, j);
        if (x < (prev + a) * 0.5f) return static_cast<std::size_t>(i);
        prev = a;
        i = j;
    }
    return static_cast<std::size_t>(row.end);
}

// Up (dir -1) / Down (+1) from `caret`, aiming for column `x` (row-relative; callers keep
// the column from where vertical motion began, so it survives short rows). Past the first
// row goes to the start of the text, past the last to its end — as in any editor.
template <class Adv>
std::size_t verticalMove(const std::string& text, const std::vector<TextRow>& rows,
                         std::size_t caret, int dir, float x, const Adv& adv) {
    const int r = rowOfByte(rows, caret) + dir;
    if (r < 0) return 0;
    if (r >= static_cast<int>(rows.size())) return text.size();
    return byteAtX(text, rows[r], x, adv);
}

inline std::size_t rowHome(const std::vector<TextRow>& rows, std::size_t caret) {
    return static_cast<std::size_t>(rows[rowOfByte(rows, caret)].start);
}
inline std::size_t rowEnd(const std::vector<TextRow>& rows, std::size_t caret) {
    return static_cast<std::size_t>(rows[rowOfByte(rows, caret)].end);
}

} // namespace tt
