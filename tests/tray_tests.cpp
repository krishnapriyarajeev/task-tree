// Dependency-free verification of the tray seam's pure part: the DBusMenu id
// model (TrayMenu.hpp), which assigns menu ids and maps an incoming click id
// back to an action. Header-only, so this runs on every platform — the live
// StatusNotifierItem/DBus wiring in TrayGio.cpp is verified by running the app.
//   g++ -std=c++17 -I ../src tray_tests.cpp
#include "platform/TrayMenu.hpp"

#include <cstdio>

using namespace tt;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                            \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) { ++g_fail; std::printf("  FAIL: %s\n", msg); }               \
    } while (0)

int main() {
    std::printf("[tray] menu id assignment\n");
    {
        // Root is id 0; on-screen items get 1..N in order.
        CHECK(TrayMenu::kRootId == 0, "root id is 0");
        CHECK(TrayMenu::idForIndex(0) == 1, "first item is id 1");
        CHECK(TrayMenu::idForIndex(2) == 3, "third item is id 3");
    }

    std::printf("[tray] click -> action resolution\n");
    {
        int toggles = 0, quits = 0;
        TrayMenu menu;
        // The shipped menu: "Show/Hide", a separator, then "Quit".
        std::vector<TrayItem> items;
        items.push_back(TrayItem{"Show/Hide TaskTree", [&] { ++toggles; }, false});
        items.push_back(TrayItem::sep());
        items.push_back(TrayItem{"Quit", [&] { ++quits; }, false});
        menu.setItems(std::move(items));

        // A clickable id resolves to its action but is NOT invoked by lookup.
        const std::function<void()>* a1 = menu.actionForId(1);
        CHECK(a1 != nullptr, "id 1 resolves to an action");
        CHECK(toggles == 0, "actionForId does not invoke the callback");
        if (a1) (*a1)();
        CHECK(toggles == 1, "invoking the resolved action runs the right callback");

        // The separator occupies id 2 but has no action; Quit keeps its id 3 despite it.
        CHECK(menu.actionForId(2) == nullptr, "separator id has no action");
        const std::function<void()>* a3 = menu.actionForId(3);
        CHECK(a3 != nullptr, "id 3 (after the separator) resolves");
        if (a3) (*a3)();
        CHECK(quits == 1, "id 3 runs Quit, not Show/Hide");
        CHECK(toggles == 1, "Quit did not also fire Show/Hide");

        // Root and out-of-range ids resolve to nothing.
        CHECK(menu.actionForId(TrayMenu::kRootId) == nullptr, "root id has no action");
        CHECK(menu.actionForId(0) == nullptr, "id 0 has no action");
        CHECK(menu.actionForId(4) == nullptr, "past-the-end id has no action");
        CHECK(menu.actionForId(-1) == nullptr, "negative id has no action");
    }

    std::printf("[tray] %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
