#pragma once
// Pure, header-only model of the tray's popup menu — the id bookkeeping the
// DBusMenu protocol needs, kept out of the GIO backend so it can be unit-tested
// on every platform (see tests/tray_tests.cpp). The backend (TrayGio.cpp) walks
// this to build the com.canonical.dbusmenu layout and to turn an incoming click
// id back into the action to run.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace tt {

struct TrayItem {
    std::string label;
    std::function<void()> onClick;  // empty for separators / non-clickable rows
    bool separator = false;

    static TrayItem sep() { return TrayItem{"", nullptr, true}; }
};

// DBusMenu numbers every node: the root is 0 and each on-screen row (separators
// included, so ids track the visible order) gets 1..N in insertion order.
class TrayMenu {
public:
    static constexpr int kRootId = 0;

    void setItems(std::vector<TrayItem> items) { items_ = std::move(items); }
    const std::vector<TrayItem>& items() const { return items_; }

    static int idForIndex(std::size_t i) { return static_cast<int>(i) + 1; }

    // The clickable action bound to a DBusMenu id, or nullptr if the id is the
    // root, a separator, out of range, or otherwise not clickable. Pure lookup —
    // it does not invoke the action (the backend queues it for the main thread).
    const std::function<void()>* actionForId(int id) const {
        if (id < 1 || static_cast<std::size_t>(id) > items_.size()) return nullptr;
        const TrayItem& it = items_[static_cast<std::size_t>(id) - 1];
        if (it.separator || !it.onClick) return nullptr;
        return &it.onClick;
    }

private:
    std::vector<TrayItem> items_;
};

} // namespace tt
