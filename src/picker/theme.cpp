#include "picker/theme.h"

#include "ui/style.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

using nlohmann::json;

namespace xdpu::picker {
  namespace {
    constexpr size_t kMaxThemeMessageSize = 65536;

    std::optional<Color> themeColor(const json& colors, const char* key) {
      const auto value = colors.find(key);
      if (value == colors.end() || !value->is_string()) {
        return std::nullopt;
      }
      try {
        return hex(value->get<std::string>());
      } catch (const std::invalid_argument&) {
        return std::nullopt;
      }
    }

    void closeFd(int fd) {
      if (fd >= 0) {
        while (::close(fd) < 0 && errno == EINTR) {
        }
      }
    }

    std::string themeSocketPath() {
      if (const char* configured = std::getenv("UMBRIEL_SOCKET"); configured != nullptr && configured[0] != '\0') {
        return configured;
      }
      const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
      const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
      if (runtimeDir != nullptr && runtimeDir[0] != '\0' && waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return std::string(runtimeDir) + "/umbriel-" + waylandDisplay + ".sock";
      }
      return {};
    }

    int openThemeSubscription() {
      const std::string socketPath = themeSocketPath();
      if (socketPath.empty()) {
        return -1;
      }

      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return -1;
      }

      timeval timeout{.tv_sec = 0, .tv_usec = 250000};
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      const size_t pathLength = socketPath.size();
      if (pathLength >= sizeof(address.sun_path)) {
        closeFd(fd);
        return -1;
      }
      std::memcpy(address.sun_path, socketPath.data(), pathLength);

      if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        closeFd(fd);
        return -1;
      }

      constexpr std::string_view request = "{\"cmd\":\"subscribe\",\"events\":[\"theme\"]}\n";
      size_t sent = 0;
      while (sent < request.size()) {
        const ssize_t size = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size <= 0) {
          closeFd(fd);
          return -1;
        }
        sent += static_cast<size_t>(size);
      }

      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        closeFd(fd);
        return -1;
      }
      return fd;
    }
  } // namespace

  std::optional<UmbrielTheme> parseThemeEvent(std::string_view line) {
    try {
      const json envelope = json::parse(line);
      if (!envelope.is_object() || envelope.value("event", "") != "theme") {
        return std::nullopt;
      }
      const auto values = envelope.find("data");
      if (values == envelope.end() || !values->is_object()) {
        return std::nullopt;
      }

      const auto background = themeColor(*values, "background");
      const auto textPrimary = themeColor(*values, "text_primary");
      const auto textMuted = themeColor(*values, "text_muted");
      const auto accentPrimary = themeColor(*values, "accent_primary");
      const auto accentSecondary = themeColor(*values, "accent_secondary");
      const auto warning = themeColor(*values, "warning");
      const auto error = themeColor(*values, "error");
      const auto cornerRadius = values->find("corner_radius");
      if (!background
          || !textPrimary
          || !textMuted
          || !accentPrimary
          || !accentSecondary
          || !warning
          || !error
          || cornerRadius == values->end()
          || !cornerRadius->is_number_integer()) {
        return std::nullopt;
      }
      const auto radius = cornerRadius->get<long long>();
      if (radius < 0 || radius > 500) {
        return std::nullopt;
      }

      return UmbrielTheme{
          .background = *background,
          .textPrimary = *textPrimary,
          .textMuted = *textMuted,
          .accentPrimary = *accentPrimary,
          .accentSecondary = *accentSecondary,
          .warning = *warning,
          .error = *error,
          .cornerRadius = static_cast<int>(radius),
      };
    } catch (const json::exception&) {
      return std::nullopt;
    }
  }

  Palette paletteFromTheme(const UmbrielTheme& theme) {
    const Color& bg = theme.background;
    const auto container = [&bg](const Color& accent) { return lerpColor(bg, accent, 0.20F); };
    const auto raised = [&](float amount) { return lerpColor(bg, theme.textPrimary, amount); };
    return {
        .primary = theme.accentPrimary,
        .onPrimary = bg,
        .primaryContainer = container(theme.accentPrimary),
        .onPrimaryContainer = theme.textPrimary,
        .secondary = theme.accentSecondary,
        .onSecondary = bg,
        .secondaryContainer = container(theme.accentSecondary),
        .onSecondaryContainer = theme.textPrimary,
        .tertiary = theme.warning,
        .onTertiary = bg,
        .tertiaryContainer = container(theme.warning),
        .onTertiaryContainer = theme.textPrimary,
        .error = theme.error,
        .onError = bg,
        .errorContainer = container(theme.error),
        .onErrorContainer = theme.textPrimary,
        .surface = bg,
        .onSurface = theme.textPrimary,
        .onSurfaceVariant = theme.textMuted,
        .surfaceContainerLow = raised(0.04F),
        .surfaceContainer = raised(0.07F),
        .surfaceContainerHigh = raised(0.10F),
        .surfaceContainerHighest = raised(0.13F),
        .outlineVariant = raised(0.18F),
        .shadow = hex("#000000"),
        .scrim = hex("#000000"),
        .hover = theme.accentSecondary,
        .onHover = bg,
    };
  }

  Palette fallbackPalette() {
    return {
        .primary = hex("#f7e9a8"),
        .onPrimary = hex("#302c1e"),
        .primaryContainer = hex("#585552"),
        .onPrimaryContainer = hex("#f7e9a8"),
        .secondary = hex("#b3b8ed"),
        .onSecondary = hex("#252740"),
        .secondaryContainer = hex("#444667"),
        .onSecondaryContainer = hex("#b3b8ed"),
        .tertiary = hex("#a0ddc0"),
        .onTertiary = hex("#1d3029"),
        .tertiaryContainer = hex("#3e5159"),
        .onTertiaryContainer = hex("#a0ddc0"),
        .error = hex("#ed8296"),
        .onError = hex("#3b2029"),
        .errorContainer = hex("#55364d"),
        .onErrorContainer = hex("#f68b9e"),
        .surface = hex("#15162e"),
        .onSurface = hex("#ece9f4"),
        .onSurfaceVariant = hex("#a5a7c6"),
        .surfaceContainerLow = hex("#1a1b35"),
        .surfaceContainer = hex("#20213d"),
        .surfaceContainerHigh = hex("#27284a"),
        .surfaceContainerHighest = hex("#2e2f58"),
        .outlineVariant = hex("#3e4166"),
        .shadow = hex("#0b0c1b"),
        .scrim = hex("#000000"),
        .hover = hex("#a0ddc0"),
        .onHover = hex("#1d3029"),
    };
  }

  float radiusScale(int cornerRadius) { return static_cast<float>(cornerRadius) / Style::radiusXl; }

  bool applyThemeGlobals(const std::optional<UmbrielTheme>& theme) {
    setPalette(theme ? paletteFromTheme(*theme) : fallbackPalette());
    if (!theme) {
      return false;
    }
    const float previous = Style::cornerRadiusScale();
    Style::setCornerRadiusScale(radiusScale(theme->cornerRadius));
    return Style::cornerRadiusScale() != previous;
  }

  ThemeSubscription::ThemeSubscription() : m_fd(openThemeSubscription()) {}

  ThemeSubscription::~ThemeSubscription() { close(); }

  std::optional<UmbrielTheme> ThemeSubscription::waitForInitial(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_fd >= 0) {
      const auto remaining =
          std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        break;
      }
      pollfd fd{.fd = m_fd, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&fd, 1, static_cast<int>(remaining));
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready <= 0) {
        break;
      }
      const bool open = readAvailable();
      if (auto theme = takeLatest()) {
        m_last = theme;
        if (!open) {
          close();
        }
        return theme;
      }
      if (!open) {
        close();
      }
    }
    return std::nullopt;
  }

  void ThemeSubscription::setHandler(Handler handler) { m_handler = std::move(handler); }

  void ThemeSubscription::doAddPollFds(std::vector<pollfd>& fds) {
    if (m_fd >= 0) {
      fds.push_back({.fd = m_fd, .events = POLLIN, .revents = 0});
    }
  }

  void ThemeSubscription::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
    if (m_fd < 0 || startIdx >= fds.size() || fds[startIdx].fd != m_fd || fds[startIdx].revents == 0) {
      return;
    }
    const bool open = readAvailable() && (fds[startIdx].revents & (POLLHUP | POLLERR | POLLNVAL)) == 0;
    if (auto theme = takeLatest(); theme && theme != m_last) {
      m_last = theme;
      if (m_handler) {
        m_handler(*theme);
      }
    }
    if (!open) {
      close();
    }
  }

  bool ThemeSubscription::readAvailable() {
    char chunk[4096];
    while (true) {
      const ssize_t size = recv(m_fd, chunk, sizeof(chunk), 0);
      if (size > 0) {
        m_buffer.append(chunk, static_cast<size_t>(size));
        if (m_buffer.size() > kMaxThemeMessageSize) {
          return false;
        }
        continue;
      }
      if (size == 0) {
        return false;
      }
      if (errno == EINTR) {
        continue;
      }
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }
  }

  std::optional<UmbrielTheme> ThemeSubscription::takeLatest() {
    std::optional<UmbrielTheme> latest;
    size_t newline = 0;
    while ((newline = m_buffer.find('\n')) != std::string::npos) {
      if (auto theme = parseThemeEvent(std::string_view(m_buffer).substr(0, newline))) {
        latest = theme;
      }
      m_buffer.erase(0, newline + 1);
    }
    return latest;
  }

  void ThemeSubscription::close() {
    closeFd(std::exchange(m_fd, -1));
    m_buffer.clear();
  }

} // namespace xdpu::picker
