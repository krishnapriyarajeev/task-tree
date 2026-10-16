// Linux tray backend: a StatusNotifierItem (the freedesktop/KDE D-Bus tray
// standard) plus a com.canonical.dbusmenu popup menu, exported on a dedicated
// GLib main-loop thread and registered with the session's StatusNotifierWatcher.
//
// SNI is D-Bus, not X11, so this works identically under X11 and Wayland and on
// any desktop that hosts a tray (KDE, GNOME + the AppIndicator extension, XFCE,
// MATE, …). Where no StatusNotifierWatcher is on the bus the RegisterStatus…
// call simply fails and no icon appears — the app is unaffected.
//
// Threading: all GLib/GDBus work happens on our thread (its own thread-default
// GMainContext, so g_dbus_connection_register_object dispatches method calls
// here). A click is turned into an action via the pure TrayMenu model and handed
// to Tray::queueAction, which wakes the main loop; the action runs on the main
// thread in Tray::drain(). See Tray.hpp.

#include "platform/Tray.hpp"

#include <future>
#include <string>
#include <thread>

#include <unistd.h>  // getpid

#include <gio/gio.h>

namespace tt {

namespace {

// Only the members the tray needs from each standard interface.
constexpr const char* kSniXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewIcon"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
    <signal name="NewTitle"/>
    <signal name="NewToolTip"/>
  </interface>
</node>)XML";

constexpr const char* kMenuXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
  </interface>
</node>)XML";

// The com.canonical.dbusmenu property dict (a{sv}) for one node. Floating.
GVariant* menuItemProps(const Tray* t, int id) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    if (id == TrayMenu::kRootId) {
        g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
    } else {
        const auto& items = t->menu().items();
        const auto idx = static_cast<std::size_t>(id) - 1;
        if (id >= 1 && idx < items.size()) {
            const TrayItem& it = items[idx];
            if (it.separator) {
                g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
            } else {
                g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(it.label.c_str()));
                g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(TRUE));
                g_variant_builder_add(&b, "{sv}", "visible", g_variant_new_boolean(TRUE));
            }
        }
    }
    return g_variant_builder_end(&b);
}

// One (ia{sv}av) layout node; the root carries every item as its children. Floating.
GVariant* menuLayoutNode(const Tray* t, int id) {
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    if (id == TrayMenu::kRootId) {
        const auto& items = t->menu().items();
        for (std::size_t i = 0; i < items.size(); ++i)
            g_variant_builder_add(&kids, "v", menuLayoutNode(t, TrayMenu::idForIndex(i)));
    }
    GVariant* props = menuItemProps(t, id);
    GVariant* children = g_variant_builder_end(&kids);
    return g_variant_new("(i@a{sv}@av)", id, props, children);
}

// ---- org.kde.StatusNotifierItem -------------------------------------------

GVariant* sniGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                         const gchar* name, GError**, gpointer user_data) {
    const Tray* t = static_cast<const Tray*>(user_data);
    if (!g_strcmp0(name, "Category")) return g_variant_new_string("ApplicationStatus");
    if (!g_strcmp0(name, "Id")) return g_variant_new_string("tasktree");
    if (!g_strcmp0(name, "Title")) return g_variant_new_string(t->title().c_str());
    if (!g_strcmp0(name, "Status")) return g_variant_new_string("Active");
    if (!g_strcmp0(name, "IconName")) return g_variant_new_string(t->iconName().c_str());
    if (!g_strcmp0(name, "IconThemePath")) return g_variant_new_string("");
    if (!g_strcmp0(name, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!g_strcmp0(name, "Menu")) return g_variant_new_object_path("/MenuBar");
    if (!g_strcmp0(name, "ToolTip")) {
        GVariant* pixmaps = g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
        return g_variant_new("(s@a(iiay)ss)", "", pixmaps, t->title().c_str(), "");
    }
    return nullptr;
}

void sniMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                   const gchar* method, GVariant*, GDBusMethodInvocation* inv,
                   gpointer user_data) {
    Tray* t = static_cast<Tray*>(user_data);
    // Left-click (Activate) and middle-click (SecondaryActivate) both toggle.
    if (!g_strcmp0(method, "Activate") || !g_strcmp0(method, "SecondaryActivate")) {
        if (t->activate()) t->queueAction(t->activate());
    }
    // ContextMenu / Scroll: nothing to do (the menu is served over dbusmenu).
    g_dbus_method_invocation_return_value(inv, nullptr);
}

// ---- com.canonical.dbusmenu ------------------------------------------------

GVariant* menuGetProperty(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                          const gchar* name, GError**, gpointer) {
    if (!g_strcmp0(name, "Version")) return g_variant_new_uint32(3);
    if (!g_strcmp0(name, "Status")) return g_variant_new_string("normal");
    if (!g_strcmp0(name, "TextDirection")) return g_variant_new_string("ltr");
    if (!g_strcmp0(name, "IconThemePath")) return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

void menuMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                    const gchar* method, GVariant* params, GDBusMethodInvocation* inv,
                    gpointer user_data) {
    Tray* t = static_cast<Tray*>(user_data);

    if (!g_strcmp0(method, "GetLayout")) {
        gint parentId = 0, depth = 0;
        GVariant* propNames = nullptr;
        g_variant_get(params, "(ii@as)", &parentId, &depth, &propNames);
        if (propNames) g_variant_unref(propNames);
        GVariant* layout = menuLayoutNode(t, parentId);
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(u@(ia{sv}av))", static_cast<guint>(1), layout));
        return;
    }

    if (!g_strcmp0(method, "GetGroupProperties")) {
        GVariantIter* idsIter = nullptr;
        GVariant* propNames = nullptr;
        g_variant_get(params, "(ai@as)", &idsIter, &propNames);
        if (propNames) g_variant_unref(propNames);

        GVariantBuilder rb;
        g_variant_builder_init(&rb, G_VARIANT_TYPE("a(ia{sv})"));
        gint id = 0;
        bool any = false;
        while (g_variant_iter_loop(idsIter, "i", &id)) {
            any = true;
            g_variant_builder_add(&rb, "(i@a{sv})", id, menuItemProps(t, id));
        }
        g_variant_iter_free(idsIter);
        if (!any) {  // empty request means "all nodes": root + every item
            g_variant_builder_add(&rb, "(i@a{sv})", TrayMenu::kRootId,
                                  menuItemProps(t, TrayMenu::kRootId));
            const auto& items = t->menu().items();
            for (std::size_t i = 0; i < items.size(); ++i)
                g_variant_builder_add(&rb, "(i@a{sv})", TrayMenu::idForIndex(i),
                                      menuItemProps(t, TrayMenu::idForIndex(i)));
        }
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&rb)));
        return;
    }

    if (!g_strcmp0(method, "GetProperty")) {
        gint id = 0;
        const gchar* name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant* props = menuItemProps(t, id);
        GVariant* val = g_variant_lookup_value(props, name, nullptr);
        if (!val) val = g_variant_ref_sink(g_variant_new_string(""));
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", val));
        g_variant_unref(val);
        g_variant_unref(props);
        return;
    }

    if (!g_strcmp0(method, "Event")) {
        gint id = 0;
        gchar* eventId = nullptr;
        GVariant* data = nullptr;
        guint ts = 0;
        g_variant_get(params, "(isvu)", &id, &eventId, &data, &ts);
        if (!g_strcmp0(eventId, "clicked"))
            if (const std::function<void()>* a = t->menu().actionForId(id))
                t->queueAction(*a);
        g_free(eventId);
        if (data) g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, nullptr);
        return;
    }

    if (!g_strcmp0(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
        return;
    }

    g_dbus_method_invocation_return_value(inv, nullptr);
}

// When we own our well-known name, ask the watcher to show the item.
void onNameAcquired(GDBusConnection* conn, const gchar* name, gpointer) {
    g_dbus_connection_call(conn, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", name), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           nullptr, nullptr, nullptr);
}

} // namespace

struct Tray::Impl {
    std::thread thread;
    GMainContext* ctx = nullptr;
    GMainLoop* loop = nullptr;
    GDBusConnection* conn = nullptr;
    guint ownId = 0;
    guint sniReg = 0;
    guint menuReg = 0;
    GDBusNodeInfo* sniNode = nullptr;
    GDBusNodeInfo* menuNode = nullptr;
    std::string busName;

    void run(Tray* owner, std::promise<bool> ready);
};

void Tray::Impl::run(Tray* owner, std::promise<bool> ready) {
    ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);

    conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!conn) {  // no session bus (e.g. headless) — nothing to show
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
        ctx = nullptr;
        ready.set_value(false);
        return;
    }

    sniNode = g_dbus_node_info_new_for_xml(kSniXml, nullptr);
    menuNode = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    static const GDBusInterfaceVTable sniVT{sniMethodCall, sniGetProperty, nullptr, {}};
    static const GDBusInterfaceVTable menuVT{menuMethodCall, menuGetProperty, nullptr, {}};
    sniReg = g_dbus_connection_register_object(conn, "/StatusNotifierItem",
                                               sniNode->interfaces[0], &sniVT, owner,
                                               nullptr, nullptr);
    menuReg = g_dbus_connection_register_object(conn, "/MenuBar", menuNode->interfaces[0],
                                                &menuVT, owner, nullptr, nullptr);

    busName = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    ownId = g_bus_own_name_on_connection(conn, busName.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
                                         onNameAcquired, nullptr, owner, nullptr);

    loop = g_main_loop_new(ctx, FALSE);
    ready.set_value(true);
    g_main_loop_run(loop);  // returns when stop() calls g_main_loop_quit

    if (ownId) g_bus_unown_name(ownId);
    if (sniReg) g_dbus_connection_unregister_object(conn, sniReg);
    if (menuReg) g_dbus_connection_unregister_object(conn, menuReg);
    if (sniNode) g_dbus_node_info_unref(sniNode);
    if (menuNode) g_dbus_node_info_unref(menuNode);
    g_main_loop_unref(loop);
    loop = nullptr;
    g_object_unref(conn);
    conn = nullptr;
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    ctx = nullptr;
}

Tray::Tray() = default;
Tray::~Tray() { stop(); }

bool Tray::start() {
    impl_ = std::make_unique<Impl>();
    Impl* impl = impl_.get();
    std::promise<bool> ready;
    std::future<bool> fut = ready.get_future();
    impl->thread = std::thread(
        [impl, this, pr = std::move(ready)]() mutable { impl->run(this, std::move(pr)); });

    if (!fut.get()) {  // no session bus — tear the thread back down
        if (impl->thread.joinable()) impl->thread.join();
        impl_.reset();
        return false;
    }
    return true;
}

void Tray::stop() {
    if (!impl_) return;
    if (impl_->loop) g_main_loop_quit(impl_->loop);  // thread-safe; wakes the loop
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_.reset();
}

} // namespace tt
