#pragma once

#include "dbus/global_shortcuts_credentials.h"
#include "wayland/global_shortcuts.h"

#include <memory>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

namespace xdpu {
  class Loop;
  class WaylandContext;

  class GlobalShortcutsPortal {
  public:
    GlobalShortcutsPortal(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, WaylandContext& wayland,
        GlobalShortcutCredentials credentials = GlobalShortcutCredentials{}, GlobalShortcutTimeouts timeouts = {}
    );
    ~GlobalShortcutsPortal();
    GlobalShortcutsPortal(const GlobalShortcutsPortal&) = delete;
    GlobalShortcutsPortal& operator=(const GlobalShortcutsPortal&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace xdpu
