#pragma once
// System-tray (StatusNotifierItem) icon: a click target that shows/hides the
// overlay without the global hotkey. It rides on D-Bus, so it behaves the same
// under X11 and Wayland — where a passive X11 key grab is often swallowed by the
// compositor and the hotkey silently stops firing.
//
// Backend, exactly one TU:
//   TrayGio.cpp  — Linux: a StatusNotifierItem + com.canonical.dbusmenu menu
//                  exported on a dedicated GLib main-loop thread and registered
//                  with the session's StatusNotifierWatcher.
//   TrayNull.cpp — everywhere else: start() is a no-op returning false. macOS's
//                  NSStatusItem equivalent would live in a TrayMac.mm beside it.
//
// Like Hotkeys, the backend never runs a user callback itself: a click is queued
// and the main loop woken, so callbacks run on the main thread via drain() and
// App / GL / window calls stay single-threaded.

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "platform/TrayMenu.hpp"

namespace tt {

class Tray {
public:
    Tray();
    ~Tray();

    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    // Configure before start().
    void setIcon(std::string themeIconName) { iconName_ = std::move(themeIconName); }
    void setTitle(std::string title) { title_ = std::move(title); }
    void setActivate(std::function<void()> cb) { activate_ = std::move(cb); } // left-click
    void setMenu(std::vector<TrayItem> items) { menu_.setItems(std::move(items)); }
    void setWake(std::function<void()> wake) { wake_ = std::move(wake); }

    // Publish the icon on the session bus. Returns false when there is no session
    // bus or on the no-op backend. A missing tray host is not a failure — the icon
    // simply doesn't appear and the app runs on regardless.
    bool start();
    void stop();

    // Main thread: run any actions queued since the last call.
    void drain() {
        std::vector<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            local.swap(pending_);
        }
        for (auto& fn : local)
            if (fn) fn();
    }

    // Backend entry point (its own thread): queue an action + wake the main loop.
    void queueAction(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            pending_.push_back(std::move(fn));
        }
        if (wake_) wake_();
    }

    // Backend read accessors (used from the DBus dispatch thread).
    const TrayMenu& menu() const { return menu_; }
    const std::function<void()>& activate() const { return activate_; }
    const std::string& iconName() const { return iconName_; }
    const std::string& title() const { return title_; }

private:
    struct Impl;  // per-OS backend state

    TrayMenu menu_;
    std::function<void()> activate_;
    std::function<void()> wake_;
    std::string iconName_ = "view-list-symbolic";
    std::string title_ = "TaskTree";

    std::mutex queueMutex_;
    std::vector<std::function<void()>> pending_;

    std::unique_ptr<Impl> impl_;
};

} // namespace tt
