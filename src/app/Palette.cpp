#include "app/Palette.hpp"

#include <algorithm>
#include <cctype>

namespace tt::palette {
namespace {

std::string trim(const std::string& s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

// Digits -> id, with a cap so a pasted 30-digit number can't wrap around into a real id.
TaskId toId(const std::string& digits) {
    TaskId id = 0;
    for (char c : digits) {
        if (id > (1ull << 40)) return 0;   // absurd: no such node, treat as "none given"
        id = id * 10 + static_cast<TaskId>(c - '0');
    }
    return id;
}

} // namespace

const std::vector<ModeInfo>& modes() {
    static const std::vector<ModeInfo> kModes = {
        {Mode::Find, '?', "find", "highlight matching nodes and jump between them",
         "text to find…"},
        {Mode::Select, ':', "select", "select a node — its id, or text to match",
         "node id, or text to match…"},
        {Mode::Parent, '>', "parent", "make a node the parent of the selected one",
         "node id, or text to match…"},
        {Mode::Workspace, '@', "workspace", "go to a workspace (a new name creates one)",
         "workspace name…"},
        {Mode::NewWorkspace, '+', "new workspace", "create a workspace inside this view",
         "name of the new workspace…"},
        {Mode::Notes, '#', "notes", "open a note (a new title creates one)",
         "note title…"},
    };
    return kModes;
}

const ModeInfo* infoFor(Mode m) {
    for (const ModeInfo& i : modes())
        if (i.mode == m) return &i;
    static const ModeInfo kMenu{Mode::Menu, '/', "mode", "pick what the bar does",
                                "pick a mode…"};
    return (m == Mode::Menu) ? &kMenu : nullptr;
}

Mode modeForPrefix(char c) {
    if (c == '/') return Mode::Menu;
    for (const ModeInfo& i : modes())
        if (i.prefix == c) return i.mode;
    return Mode::Add;
}

Command interpret(Mode mode, const std::string& arg) {
    Command c;
    switch (mode) {
        case Mode::Add:
            c.body = trim(arg);
            return c;
        case Mode::Menu:
            return c;                      // the menu itself does nothing until Enter
        case Mode::Find:
            c.kind = Kind::Find;
            c.query = trim(arg);
            return c;
        case Mode::Workspace:
            c.kind = Kind::GoWorkspace;
            c.query = trim(arg);
            return c;
        case Mode::NewWorkspace:
            c.kind = Kind::NewWorkspace;
            c.body = trim(arg);
            return c;
        case Mode::Notes:
            c.kind = Kind::GoNote;
            c.query = trim(arg);
            return c;
        case Mode::Select:
        case Mode::Parent:
            break;
    }

    // Select / Parent: digits mean "by id", anything else is a text query. A leading '?'
    // forces text mode, so ':?12' looks for the text "12" instead of node 12.
    std::string tail = arg;
    const bool forcedText = !tail.empty() && tail[0] == '?';
    if (forcedText) tail.erase(0, 1);
    const std::string t = trim(tail);
    const bool byId = !forcedText && allDigits(t);

    if (mode == Mode::Select) c.kind = byId ? Kind::SelectId : Kind::SelectText;
    else                      c.kind = byId ? Kind::ParentId : Kind::ParentText;
    if (byId) c.id = toId(t);
    else      c.query = t;
    return c;
}

std::vector<TaskId> rankMatches(const Forest& f, const std::string& query, std::size_t limit,
                                TaskId scope, bool descriptions) {
    std::vector<TaskId> out;
    const std::string q = lower(trim(query));
    if (q.empty()) return out;

    struct Hit {
        bool inDesc;       // a description-only match: always after every text match
        std::size_t pos;   // where the query matched (0 = the text starts with it)
        std::size_t len;   // matched field's length: a shorter one is the tighter match
        TaskId id;
    };
    std::vector<Hit> hits;
    for (const auto& [id, t] : f.nodes) {
        if (f.isInDoneSection(id)) continue;
        if (scope != kMainView && !f.isDescendantOf(id, scope)) continue;
        const std::string lt = lower(t.text);
        if (const std::size_t pos = lt.find(q); pos != std::string::npos) {
            hits.push_back({false, pos, lt.size(), id});
            continue;
        }
        if (!descriptions || t.description.empty()) continue;
        const std::string ld = lower(t.description);
        if (const std::size_t pos = ld.find(q); pos != std::string::npos)
            hits.push_back({true, pos, ld.size(), id});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.inDesc != b.inDesc) return !a.inDesc;
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.len != b.len) return a.len < b.len;
        return a.id < b.id;
    });

    const std::size_t n = (limit == 0) ? hits.size() : std::min(limit, hits.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(hits[i].id);
    return out;
}

std::vector<TaskId> rankWorkspaces(const Forest& f, const std::string& query) {
    std::vector<TaskId> out;
    const std::string q = lower(trim(query));
    struct Hit { std::size_t pos, len, order; TaskId id; };
    std::vector<Hit> hits;
    const std::vector<TaskId> all = f.workspaces();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::string ln = lower(f.get(all[i])->text);
        const std::size_t pos = q.empty() ? 0 : ln.find(q);
        if (pos == std::string::npos) continue;
        hits.push_back({pos, q.empty() ? 0 : ln.size(), i, all[i]});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.len != b.len) return a.len < b.len;
        return a.order < b.order;
    });
    for (const Hit& h : hits) out.push_back(h.id);
    return out;
}

std::vector<NoteId> rankNotes(const Forest& f, const std::string& query) {
    std::vector<NoteId> out;
    const std::string q = lower(trim(query));
    if (q.empty()) {
        for (const auto& [id, n] : f.notes) out.push_back(id);   // map order = creation order
        return out;
    }
    struct Hit { bool inBody; std::size_t pos, len; NoteId id; };
    std::vector<Hit> hits;
    for (const auto& [id, n] : f.notes) {
        const std::string lt = lower(n.title);
        if (const std::size_t pos = lt.find(q); pos != std::string::npos) {
            hits.push_back({false, pos, lt.size(), id});
            continue;
        }
        const std::string lb = lower(n.body);
        if (const std::size_t pos = lb.find(q); pos != std::string::npos)
            hits.push_back({true, pos, lb.size(), id});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.inBody != b.inBody) return !a.inBody;
        if (a.pos != b.pos) return a.pos < b.pos;
        if (a.len != b.len) return a.len < b.len;
        return a.id < b.id;
    });
    for (const Hit& h : hits) out.push_back(h.id);
    return out;
}

} // namespace tt::palette
