#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "defaults.h"
#include "picker/main_loop.h"
#include "picker/picker_window.h"
#include "picker/request.h"
#include "picker/theme.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "wayland/key_repeat_poll_source.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <print>
#include <stdexcept>
#include <utility>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  volatile std::sig_atomic_t g_stopRequested = 0;

  void stopRequested(int) { g_stopRequested = 1; }

  // The library's controls match these actions: buttons and checkboxes on Validate, Tab focus on TabNext/Previous.
  void initializeKeys() {
    const std::array<std::pair<KeybindAction, std::uint32_t>, 5> keys{{
        {KeybindAction::Cancel, XKB_KEY_Escape},
        {KeybindAction::Left, XKB_KEY_Left},
        {KeybindAction::Right, XKB_KEY_Right},
        {KeybindAction::Up, XKB_KEY_Up},
        {KeybindAction::Down, XKB_KEY_Down},
    }};
    for (const auto& [action, sym] : keys) {
      KeybindMatcher::setMatcher(action, [sym](std::uint32_t key, std::uint32_t modifiers) {
        return key == sym && modifiers == 0;
      });
    }
    KeybindMatcher::setMatcher(KeybindAction::TabNext, [](std::uint32_t sym, std::uint32_t modifiers) {
      return sym == XKB_KEY_Tab && modifiers == 0;
    });
    KeybindMatcher::setMatcher(KeybindAction::TabPrevious, [](std::uint32_t sym, std::uint32_t modifiers) {
      return KeySymbol::isTab(sym) && modifiers == KeyMod::Shift;
    });
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t sym, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(sym);
    });
  }

  std::filesystem::path assetsDirectory() {
    if (const char* configured = std::getenv("UMBRIEL_PICKER_ASSETS_DIR");
        configured != nullptr && configured[0] != '\0') {
      return configured;
    }
    return XDPU_PICKER_ASSETS_DIR;
  }

  void run(const xdpu::picker::ShareRequest& request) {
    using namespace xdpu::picker;

    setApplicationIdentity({.name = "umbriel-share-picker", .envPrefix = "UMBRIEL_PICKER"});
    initLogLevelFromEnvironment();
    const auto assets = assetsDirectory();
    paths::setAssetRoots({.application = assets, .library = assets});
    initializeKeys();

    WaylandConnection connection;
    if (!connection.connect()) {
      throw std::runtime_error("could not connect to Wayland: " + connection.describeDisplayError());
    }
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    AnimationManager animations;

    // The palette and radii must be final before the first control is built.
    ThemeSubscription theme;
    applyThemeGlobals(theme.waitForInitial(std::chrono::milliseconds(250)));

    std::optional<PickerWindow> window;
    window.emplace(request, connection, renderer, animations);
    connection.setPointerEventCallback([&window](const PointerEvent& event) { window->pointer(event); });
    connection.setKeyboardEventCallback([&window](const KeyboardEvent& event) { window->keyboard(event); });
    theme.setHandler([&window](const UmbrielTheme& next) { window->themeChanged(next); });

    DeferredCallPollSource deferred;
    TimerPollSource timers;
    KeyRepeatPollSource repeat(connection);
    MainLoop loop(connection, {&deferred, &timers, &repeat, &theme}, [&window] {
      return !window->done() && g_stopRequested == 0;
    });
    try {
      loop.run();
    } catch (...) {
      window->cancel();
      throw;
    }

    window->cancel();
    // Unmap before waiting for preview cleanup so the compositor can return focus.
    window->closeSurface();
    wl_display_flush(connection.display());
    window->stopPreviews();
    connection.setPointerEventCallback({});
    connection.setKeyboardEventCallback({});
    theme.setHandler({});
    window.reset();
    renderer.cleanup();
    graphics.cleanup();
  }
} // namespace

int main() {
  const auto request = xdpu::picker::parseRequest(xdpu::picker::readStdin());

  const auto previousTerm = std::signal(SIGTERM, stopRequested);
  const auto previousInt = std::signal(SIGINT, stopRequested);
  int status = EXIT_SUCCESS;
  try {
    run(request);
  } catch (const std::exception& error) {
    std::println(stderr, "umbriel-share-picker: {}", error.what());
    status = EXIT_FAILURE;
  }
  std::signal(SIGTERM, previousTerm);
  std::signal(SIGINT, previousInt);
  return status;
}
