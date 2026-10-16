#pragma once
// NanoVG-based scene renderer. Owns no window/GL-context lifetime (main creates the
// NVGcontext); it only issues draw calls and measures node sizes. All coordinates
// are logical units; HiDPI is handled by the devicePixelRatio passed to beginFrame.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/Config.hpp"
#include "layout/Geometry.hpp"
#include "model/Task.hpp"
#include "ui/TextRows.hpp"

struct NVGcontext;

namespace tt {

// Everything the renderer needs to draw the in-progress drag.
struct DragVisual {
    bool   active = false;
    TaskId dragged = 0;       // node drawn as a ghost at the cursor
    TaskId target = 0;        // hovered drop target (0 = root area)
    bool   validTarget = false;
    Rect   ghost;             // where to draw the dragged node (follows cursor)
    Vec2   fromPoint;         // preview edge start (target bottom-centre)
    Vec2   toPoint;           // preview edge end (insertion slot top-centre)
    bool   showPreviewEdge = false;
};

// How the input bar is dressed. It doubles as the command palette, so its border is
// tinted per mode (find / select / parent) and it can show a right-aligned status.
struct InputStyle {
    bool quickAdd = false;       // centred box instead of the bottom bar
    bool editing = false;        // in-place node text edit: selection-blue border
    bool tinted = false;         // use `border` (a palette mode) instead of the defaults
    Color border{};
    std::string chip;            // mode name, drawn as a section at the bar's left end
    float chipWidth = 0.f;       // minimum section width, so every mode reserves the same
    std::string status;          // right-aligned + dimmed, e.g. "7 matches" / "node 12"
    std::string placeholder;     // shown when empty; empty string = the default prompt
};

// Geometry for the DONE side panel (screen coordinates), computed by App.
struct DonePanelLayout {
    bool  visible = false;
    bool  pinned = false;
    Rect  panel;              // full panel rectangle
    Rect  titleBar;           // top bar holding the title + pin button
    Rect  pinButton;          // autohide toggle
    float contentClipTop = 0.f;
    float contentClipBottom = 0.f;
    int   itemCount = 0;      // total completed tasks (shown as a count badge)
};

// The workspace picker in the top-left corner, laid out by App (screen coords). The button
// shows where you are as a breadcrumb ("main › work › amqp"); when open, a card below it
// lists main + every workspace (the current one checked) and ends with "+ New workspace".
struct ViewPickerLayout {
    Rect button;
    std::string current;               // breadcrumb on the button
    bool open = false;
    Rect card;                         // the open list's background
    std::vector<Rect> rows;            // main, each workspace, then "+ New workspace" last
    std::vector<std::string> names;    // row labels, parallel to rows (minus the last)
    std::vector<int> depths;           // workspace nesting depth per row (indent)
    int currentRow = -1;               // the checked row
    int hoveredRow = -1;               // row under the cursor (-1 = none)
    bool buttonHovered = false;
    std::string hint;                  // key legend on the card's footer line
};

// The description card (bottom-left, left of the input bar), laid out by
// Renderer::layoutDescCard so drawing and App's hit tests share one geometry. Screen
// coordinates. `rows` are the wrapped visual rows of the description (byte ranges, see
// ui/TextRows.hpp); row i's top is textTop + i * lineH - scroll.
struct DescCardLayout {
    bool  visible = false;
    bool  collapsed = false;   // header only
    bool  editing = false;
    Rect  card;                // the whole card
    Rect  header;              // title line (always shown)
    Rect  button;              // collapse / expand toggle, in the header
    Rect  body;                // text area, clipped (empty when collapsed)
    float tx = 0.f, textTop = 0.f, lineH = 0.f, asc = 0.f, desc = 0.f;
    float scroll = 0.f, maxScroll = 0.f;   // scroll is clamped to [0, maxScroll]
    std::vector<TextRow> rows;
    std::string title;         // "#12 description"
};

// One visible row in the DONE panel's expandable, indented tree.
struct DoneRow {
    TaskId id = 0;
    Rect   rect;              // full clickable row rect (already indented + scrolled)
    int    depth = 0;
    bool   hasChildren = false;
    bool   expanded = false;
    bool   hovered = false;   // cursor is over this row (subtle highlight)
};

class Renderer {
public:
    bool init(NVGcontext* vg, const std::string& fontPath);

    // Measure each node's box (width clamped to cfg.maxNodeWidth, height grown to
    // fit wrapped text) into `out`. Must use the same font/size as drawing.
    void measureSizes(const Forest& f, const Config& cfg,
                      std::unordered_map<TaskId, Size>& out) const;

    void beginFrame(int winW, int winH, float devicePixelRatio);
    void endFrame();

    void drawScrim(float w, float h, const Config& cfg);

    // Draw all edges then all nodes, translated by the canvas `pan` offset. The
    // dragged node (dv.dragged) is skipped here and drawn as a ghost; the drop target
    // is highlighted. `pathHi` nodes (and edges between them) get a fading highlight
    // at intensity `pathStrength` (0 = none) — used to flash the path to a new task.
    // `reparentTarget` (Ctrl held over a valid node) gets a green ring: the node the
    // selection would become a child of on click.
    void drawTree(const Forest& f, const std::unordered_map<TaskId, Rect>& rects,
                  const Config& cfg, const DragVisual& dv, Vec2 pan, float zoom,
                  const std::unordered_set<TaskId>& pathHi, float pathStrength,
                  const std::unordered_set<TaskId>& searchHits, TaskId selected = 0,
                  TaskId reparentTarget = 0,
                  const std::unordered_map<TaskId, float>* fade = nullptr);

    // The input widget. In quick-add mode it's a centred box with a drop shadow; otherwise
    // a bar at the bottom of the overlay. `[selBegin, selEnd)` is highlighted as the current
    // text selection (pass an empty range for none). Returns the box rect so callers can
    // stack UI on top of it (the palette drop-up) without recomputing the wrapped height.
    Rect drawInput(float screenW, float screenH, const std::string& text,
                   std::size_t caretByte, std::size_t selBegin, std::size_t selEnd,
                   bool caretOn, const Config& cfg, const InputStyle& style);

    // Byte offset in `text` nearest to screen `point`, using the exact layout drawInput
    // produces for (text, caretByte, style) at this window size — drives click-to-place and
    // drag-select in the bar. Only measures text, so it is safe to call between frames.
    std::size_t inputByteAt(float screenW, float screenH, const std::string& text,
                            std::size_t caretByte, const InputStyle& style, Vec2 point) const;

    // A small transient pill (e.g. "Copied") centred horizontally at cx with its bottom edge
    // at cyBottom — the copy confirmation shown above a node.
    void drawToast(float cx, float cyBottom, const std::string& text, const Config& cfg) const;

    // Command-palette drop-up: candidate rows in a card sitting directly above the input
    // box, best match first. Each row is (lead, rest) — an id badge or a mode symbol, then
    // its text; every lead gets the same column width so the text lines up. `activeRow` is
    // the ↑/↓ cursor, `moreCount` renders "+N more", `hint` is the small key legend on the
    // footer line, `tint` matches the bar's border.
    void drawPalette(const Rect& inputBox,
                     const std::vector<std::pair<std::string, std::string>>& rows, int activeRow,
                     int moreCount, const std::string& hint, const Color& tint,
                     const Config& cfg);

    // Description card (see DescCardLayout). Layout only measures text, so App may call it
    // between frames (hit tests); `scroll` comes back clamped. When `caretByte` is given
    // and editing, the scroll is nudged to keep that row in view.
    DescCardLayout layoutDescCard(float screenW, float screenH, const Rect& inputBox,
                                  TaskId id, const std::string& text, bool collapsed,
                                  bool editing, float scroll,
                                  std::size_t caretByte = std::string::npos) const;
    void drawDescCard(const DescCardLayout& L, const std::string& text, std::size_t caretByte,
                      std::size_t selBegin, std::size_t selEnd, bool caretOn,
                      const Config& cfg) const;
    // Advance width of text[from, to) in the card's font — the advance function the
    // ui/TextRows helpers take, so caret maths matches what is drawn.
    float textAdvance(const std::string& text, int from, int to) const;

    // The workspace picker (see ViewPickerLayout): button always, list card when open.
    void drawViewPicker(const ViewPickerLayout& layout, const Config& cfg);
    // Height of the picker's rows / button, so App can lay it out before drawing.
    float viewPickerRowHeight() const { return fontSize_ + 14.f; }

    // The DONE side panel: a floating rounded card (drop shadow + subtle border, dark
    // surface with green accents), a header with title + count badge + pin toggle, and a
    // scissor-clipped, indented, expandable list of done rows (top-level items as cards,
    // children with indent guides, hover highlight, chevrons/checks).
    void drawDonePanel(const DonePanelLayout& layout, const Forest& f,
                       const std::vector<DoneRow>& rows, const Config& cfg);

    // Wrapped height of `text` at the given content width (for card sizing).
    float measureTextHeight(const std::string& text, float width) const;
    // Single-line advance width of `text` (App sizes the input bar's mode section with it).
    float measureTextWidth(const std::string& text) const;

    float fontSize() const { return fontSize_; }

private:
    void drawNode(const Rect& r, const std::string& text, const Config& cfg,
                  bool highlight, float alphaMul, TaskId id, int status,
                  const std::string& dateChip, bool workspace = false,
                  bool hasDescription = false) const;
    void drawEdge(const Rect& parent, const Rect& child, const Config& cfg,
                  float alphaMul = 1.f) const;

    // Collapse/expand handle on a node with children: a small disc on the bottom edge.
    // Collapsed -> shows the hidden-descendant count; expanded -> a minus sign.
    void drawCollapseHandle(const Rect& node, bool collapsed, int hiddenCount,
                            const Config& cfg) const;

    // Shared chrome for the input + search fields (drop shadow, rounded fill, border).
    // They are the same component; only the border colour differs.
    void drawFieldChrome(float bx, float by, float w, float h,
                         const Config& cfg, const Color& border) const;

    // Shared geometry for the input field: the wrapped visual rows plus the box + text
    // origin they sit in. drawInput draws from it; inputByteAt hit-tests against it, so the
    // clickable positions and the drawn glyphs always coincide.
    struct InputRow { int start, end, next; };
    struct InputLayout {
        Rect box;
        float tx = 0.f, textTop = 0.f, lineH = 0.f, asc = 0.f, desc = 0.f;
        float chipW = 0.f, contentW = 0.f;
        std::vector<InputRow> rows;
        int firstRow = 0, shown = 0, caretRow = 0;
    };
    InputLayout layoutInput(float screenW, float screenH, const std::string& text,
                            std::size_t caretByte, const InputStyle& style) const;

    // Blinking text caret: a vertical bar of caretHeight(), centred on centerY at x.
    void drawCaret(float x, float centerY, const Config& cfg) const;
    // `s` shortened with a trailing ellipsis until it fits `maxW` (palette rows are one line).
    std::string fitText(const std::string& s, float maxW) const;
    // Caret height, sized between the text glyphs and the field box (used by both fields).
    float caretHeight() const { return fontSize_ * 1.5f; }

    // Reserved top band inside a node for the id badge (so text never overlaps it).
    float idBandHeight() const { return fontSize_ * 0.62f + 12.f; }

    // Per-node wrapped-text cache. Node text never changes, so the line breaks are
    // computed once (at scale 1) and reused for both sizing and drawing — this keeps
    // wrapping identical at every zoom level (no reflow -> no clipping).
    struct NodeTextCache {
        std::string text;
        float width = -1.f;
        float lineH = 0.f;
        std::vector<std::string> lines;
    };
    const NodeTextCache& layoutText(TaskId id, const std::string& text, float contentW) const;

    NVGcontext* vg_ = nullptr;
    int   font_ = -1;
    float fontSize_ = 18.f;
    float padX_ = 14.f;
    float padY_ = 10.f;
    float minNodeW_ = 90.f;
    mutable std::unordered_map<TaskId, NodeTextCache> textCache_;
};

} // namespace tt
