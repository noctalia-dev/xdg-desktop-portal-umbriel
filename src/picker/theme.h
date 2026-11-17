#pragma once

#include "core/poll_source.h"
#include "render/core/color.h"
#include "ui/palette.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace xdpu::picker {

  // Umbriel's `theme` IPC event: the compositor's [colors] and appearance.corner_radius.
  struct UmbrielTheme {
    Color background;
    Color textPrimary;
    Color textMuted;
    Color accentPrimary;
    Color accentSecondary;
    Color warning;
    Color error;
    int cornerRadius = 0;

    bool operator==(const UmbrielTheme&) const = default;
  };

  // One IPC line; nullopt unless it is a complete, valid theme event.
  std::optional<UmbrielTheme> parseThemeEvent(std::string_view line);
  // Spreads Umbriel's seven colors over the Noctalia palette roles the controls use.
  Palette paletteFromTheme(const UmbrielTheme& theme);
  // Noctalia's default dark palette, for sessions without an Umbriel socket.
  Palette fallbackPalette();
  // Corner radius in pixels to the library's radius scale, where 1.0 is Style::radiusXl.
  float radiusScale(int cornerRadius);
  // Sets the palette, and the radius scale when a theme is given. True when the radius scale changed, which
  // needs a scene rebuild: controls read radii only when they are built.
  bool applyThemeGlobals(const std::optional<UmbrielTheme>& theme);

  // Subscription to Umbriel's theme events. Without a reachable socket it stays idle and adds no fd.
  class ThemeSubscription final : public PollSource {
  public:
    using Handler = std::function<void(const UmbrielTheme&)>;

    ThemeSubscription();
    ~ThemeSubscription() override;
    ThemeSubscription(const ThemeSubscription&) = delete;
    ThemeSubscription& operator=(const ThemeSubscription&) = delete;

    // Blocks until Umbriel's reply to the subscription carries the current theme, or the timeout passes.
    std::optional<UmbrielTheme> waitForInitial(std::chrono::milliseconds timeout);
    // Called for each later theme that differs from the last one seen.
    void setHandler(Handler handler);

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  private:
    void doAddPollFds(std::vector<pollfd>& fds) override;
    // Reads what the socket holds; false once it is closed, failed, or overflowing.
    bool readAvailable();
    // The newest valid theme among the complete lines buffered so far.
    std::optional<UmbrielTheme> takeLatest();
    void close();

    int m_fd = -1;
    std::string m_buffer;
    std::optional<UmbrielTheme> m_last;
    Handler m_handler;
  };

} // namespace xdpu::picker
