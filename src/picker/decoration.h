#pragma once

#include "wayland/wayland_extension.h"

#include <cstdint>

class ToplevelSurface;
class WaylandConnection;
struct zxdg_decoration_manager_v1;

namespace xdpu::picker {

  // Binds zxdg_decoration_manager_v1, which WaylandConnection does not bind itself.
  class DecorationManager final : public WaylandExtension {
  public:
    explicit DecorationManager(WaylandConnection& connection);
    ~DecorationManager() override = default;

    void
    onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
    void onGlobalRemove(std::uint32_t name) override;
    void releaseGlobals() override;

    zxdg_decoration_manager_v1* manager() const noexcept { return m_manager; }

  private:
    zxdg_decoration_manager_v1* m_manager = nullptr;
    std::uint32_t m_global = 0;
  };

  // Asks the compositor to draw the window's decorations, as GTK did with GTK_CSD=0, so Umbriel's negotiated
  // title bar mode applies. Call before the surface is initialized; without a decoration manager it does nothing.
  void requestServerSideDecoration(ToplevelSurface& surface, DecorationManager& decorations);

} // namespace xdpu::picker
