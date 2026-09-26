#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

namespace xdpu {

  class Loop;
  class PipeWireContext;
  class WaylandContext;
  struct ScreenCastCommand;
  struct Config;

  class ScreenCastPortal {
  public:
    ScreenCastPortal(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, const Config& config,
        WaylandContext& wayland, PipeWireContext& pipewire
    );
    ~ScreenCastPortal();

    ScreenCastPortal(const ScreenCastPortal&) = delete;
    ScreenCastPortal& operator=(const ScreenCastPortal&) = delete;
    ScreenCastPortal(ScreenCastPortal&&) = delete;
    ScreenCastPortal& operator=(ScreenCastPortal&&) = delete;

    void onConfigChanged(const Config& oldCfg, const Config& newCfg);
    void onScreenCastCommand(const ScreenCastCommand& command);
    void onFocusedWindowChanged(const std::optional<std::string>& identifier);
    void onFocusedOutputChanged(const std::optional<std::string>& output);
    void setActiveChangedHandler(std::function<void(bool)> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
