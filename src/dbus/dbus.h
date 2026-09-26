#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace xdpu {

  class Loop;
  class PipeWireContext;
  class WaylandContext;
  struct ScreenCastCommand;
  struct Config;

  class DbusPortal {
  public:
    DbusPortal(Loop& loop, const Config& config, WaylandContext& wayland, PipeWireContext& pipewire);
    ~DbusPortal();

    DbusPortal(const DbusPortal&) = delete;
    DbusPortal& operator=(const DbusPortal&) = delete;
    DbusPortal(DbusPortal&&) = delete;
    DbusPortal& operator=(DbusPortal&&) = delete;

    void onConfigChanged(const Config& oldCfg, const Config& newCfg);
    void onScreenCastCommand(const ScreenCastCommand& command);
    void onFocusedWindowChanged(const std::optional<std::string>& identifier);
    void onFocusedOutputChanged(const std::optional<std::string>& output);
    void setScreenCastActiveHandler(std::function<void(bool)> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
