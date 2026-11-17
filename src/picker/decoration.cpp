#include "picker/decoration.h"

#include "wayland/toplevel_surface.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <memory>
#include <string_view>
#include <wayland-client.h>

namespace xdpu::picker {

  DecorationManager::DecorationManager(WaylandConnection& /*connection*/) {}

  void DecorationManager::onGlobal(
      wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t /*version*/
  ) {
    if (interface == zxdg_decoration_manager_v1_interface.name && m_manager == nullptr) {
      m_manager = static_cast<zxdg_decoration_manager_v1*>(
          wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1)
      );
      m_global = name;
    }
  }

  void DecorationManager::onGlobalRemove(std::uint32_t name) {
    if (name == m_global && m_manager != nullptr) {
      zxdg_decoration_manager_v1_destroy(m_manager);
      m_manager = nullptr;
      m_global = 0;
    }
  }

  void DecorationManager::releaseGlobals() {
    if (m_manager != nullptr) {
      zxdg_decoration_manager_v1_destroy(m_manager);
      m_manager = nullptr;
    }
    m_global = 0;
  }

  void requestServerSideDecoration(ToplevelSurface& surface, DecorationManager& decorations) {
    // Shared by both hooks; the role may be destroyed and recreated.
    auto role = std::make_shared<zxdg_toplevel_decoration_v1*>(nullptr);
    surface.setRoleLifecycleCallbacks(
        [role, &decorations](xdg_toplevel* toplevel) {
          if (decorations.manager() == nullptr) {
            return;
          }
          *role = zxdg_decoration_manager_v1_get_toplevel_decoration(decorations.manager(), toplevel);
          zxdg_toplevel_decoration_v1_set_mode(*role, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        },
        [role] {
          if (*role != nullptr) {
            zxdg_toplevel_decoration_v1_destroy(*role);
            *role = nullptr;
          }
        }
    );
  }

} // namespace xdpu::picker
