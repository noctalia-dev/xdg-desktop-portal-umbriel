#include "umbriel/ipc.h"

#include "loop/loop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace xdpu {

  namespace {

    std::string socketPath() {
      if (const char* configured = std::getenv("UMBRIEL_SOCKET"); configured != nullptr && configured[0] != '\0') {
        return configured;
      }
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const char* display = std::getenv("WAYLAND_DISPLAY");
      if (runtime == nullptr || runtime[0] == '\0' || display == nullptr || display[0] == '\0') {
        return {};
      }
      return std::string(runtime) + "/umbriel-" + display + ".sock";
    }

    void closeFd(int fd) {
      if (fd >= 0) {
        while (::close(fd) < 0 && errno == EINTR) {
        }
      }
    }

  } // namespace

  struct UmbrielIpc::Impl {
    Loop& loop;
    ScreenCastHandler screenCastHandler;
    FocusHandler focusedWindowHandler;
    FocusHandler focusedOutputHandler;
    int fd = -1;
    int watch = 0;
    std::string input;
    std::string output;
    size_t writeOffset = 0;

    Impl(
        Loop& loop, ScreenCastHandler screenCastHandler, FocusHandler focusedWindowHandler,
        FocusHandler focusedOutputHandler
    )
        : loop(loop), screenCastHandler(std::move(screenCastHandler)),
          focusedWindowHandler(std::move(focusedWindowHandler)), focusedOutputHandler(std::move(focusedOutputHandler)) {
      const std::string path = socketPath();
      if (path.empty()) {
        std::fprintf(stderr, "umbriel-ipc: compositor socket path is unavailable\n");
        return;
      }
      fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        std::fprintf(stderr, "umbriel-ipc: socket failed: %s\n", std::strerror(errno));
        return;
      }

      sockaddr_un address{};
      if (path.size() >= sizeof(address.sun_path)) {
        std::fprintf(stderr, "umbriel-ipc: compositor socket path is too long\n");
        closeFd(fd);
        fd = -1;
        return;
      }
      address.sun_family = AF_UNIX;
      path.copy(address.sun_path, path.size());
      if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::fprintf(stderr, "umbriel-ipc: connect failed: %s\n", std::strerror(errno));
        closeFd(fd);
        fd = -1;
        return;
      }

      constexpr std::string_view request =
          "{\"cmd\":\"subscribe\",\"events\":[\"screencast\",\"windows\",\"workspaces\"]}\n";
      size_t sent = 0;
      while (sent < request.size()) {
        const ssize_t written = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
          sent += static_cast<size_t>(written);
          continue;
        }
        if (written < 0 && errno == EINTR) {
          continue;
        }
        std::fprintf(stderr, "umbriel-ipc: subscribe failed: %s\n", std::strerror(errno));
        closeFd(fd);
        fd = -1;
        return;
      }

      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::fprintf(stderr, "umbriel-ipc: failed to make socket nonblocking: %s\n", std::strerror(errno));
        closeFd(fd);
        fd = -1;
        return;
      }

      watch = loop.addFd(fd, EPOLLIN | EPOLLERR | EPOLLHUP, [this](uint32_t events) { readable(events); });
      if (watch == 0) {
        closeFd(fd);
        fd = -1;
      }
    }

    ~Impl() {
      if (watch != 0) {
        loop.removeFd(watch);
      }
      closeFd(fd);
    }

    void disconnect() {
      if (watch != 0) {
        loop.removeFd(watch);
        watch = 0;
      }
      closeFd(fd);
      fd = -1;
      output.clear();
      writeOffset = 0;
    }

    void flushOutput() {
      while (fd >= 0 && writeOffset < output.size()) {
        const ssize_t size = send(fd, output.data() + writeOffset, output.size() - writeOffset, MSG_NOSIGNAL);
        if (size > 0) {
          writeOffset += static_cast<size_t>(size);
          continue;
        }
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          loop.updateFd(watch, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
          return;
        }
        std::fprintf(stderr, "umbriel-ipc: send failed: %s\n", std::strerror(errno));
        disconnect();
        return;
      }
      output.clear();
      writeOffset = 0;
      if (watch != 0) {
        loop.updateFd(watch, EPOLLIN | EPOLLERR | EPOLLHUP);
      }
    }

    void setScreenCastActive(bool active) {
      if (fd < 0) {
        return;
      }
      output += nlohmann::json{{"cmd", "screencast-session"}, {"active", active}}.dump();
      output.push_back('\n');
      flushOutput();
    }

    void readable(uint32_t events) {
      if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
        disconnect();
        return;
      }
      if ((events & EPOLLOUT) != 0) {
        flushOutput();
        if (fd < 0) {
          return;
        }
      }
      if ((events & EPOLLIN) == 0) {
        return;
      }

      char buffer[4096];
      while (true) {
        const ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
        if (size > 0) {
          input.append(buffer, static_cast<size_t>(size));
          continue;
        }
        if (size == 0) {
          disconnect();
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          std::fprintf(stderr, "umbriel-ipc: receive failed: %s\n", std::strerror(errno));
          disconnect();
          return;
        }
        break;
      }

      while (true) {
        const size_t newline = input.find('\n');
        if (newline == std::string::npos) {
          break;
        }
        const std::string line = input.substr(0, newline);
        input.erase(0, newline + 1);
        handleLine(line);
      }
    }

    void handleLine(std::string_view line) {
      const auto event = nlohmann::json::parse(line, nullptr, false);
      if (event.is_discarded() || !event.is_object()) {
        return;
      }
      const auto data = event.find("data");
      if (data == event.end()) {
        return;
      }

      const std::string eventName = event.value("event", "");
      if (eventName == "windows") {
        std::optional<std::string> focused;
        if (data->is_array()) {
          for (const auto& window : *data) {
            if (window.is_object() && window.value("active", false)) {
              const std::string identifier = window.value("id", "");
              if (!identifier.empty()) {
                focused = identifier;
              }
              break;
            }
          }
        }
        if (focusedWindowHandler) {
          focusedWindowHandler(focused);
        }
        return;
      }
      if (eventName == "workspaces") {
        std::optional<std::string> focused;
        if (data->is_array()) {
          for (const auto& workspace : *data) {
            if (workspace.is_object() && workspace.value("focused", false)) {
              const std::string output = workspace.value("output", "");
              if (!output.empty()) {
                focused = output;
              }
              break;
            }
          }
        }
        if (focusedOutputHandler) {
          focusedOutputHandler(focused);
        }
        return;
      }
      if (eventName != "screencast" || !data->is_object()) {
        return;
      }

      ScreenCastCommand command;
      if (const auto serial = data->find("serial"); serial != data->end() && serial->is_number_unsigned()) {
        command.serial = serial->get<uint64_t>();
      } else {
        return;
      }

      const std::string kind = data->value("kind", "");
      if (kind == "clear") {
        command.kind = ScreenCastCommand::Kind::Clear;
      } else if (kind == "output") {
        command.kind = ScreenCastCommand::Kind::Output;
        command.value = data->value("output", "");
      } else if (kind == "window") {
        command.kind = ScreenCastCommand::Kind::Window;
        command.value = data->value("identifier", "");
      } else if (kind == "follow_window") {
        command.kind = ScreenCastCommand::Kind::FollowWindow;
      } else if (kind == "follow_output") {
        command.kind = ScreenCastCommand::Kind::FollowOutput;
      } else if (kind == "follow_stop") {
        command.kind = ScreenCastCommand::Kind::FollowStop;
      } else {
        return;
      }
      if ((command.kind == ScreenCastCommand::Kind::Output || command.kind == ScreenCastCommand::Kind::Window)
          && command.value.empty()) {
        return;
      }
      if (screenCastHandler) {
        screenCastHandler(command);
      }
    }
  };

  UmbrielIpc::UmbrielIpc(
      Loop& loop, ScreenCastHandler screenCastHandler, FocusHandler focusedWindowHandler,
      FocusHandler focusedOutputHandler
  )
      : m_impl(
            std::make_unique<Impl>(
                loop, std::move(screenCastHandler), std::move(focusedWindowHandler), std::move(focusedOutputHandler)
            )
        ) {}

  UmbrielIpc::~UmbrielIpc() = default;

  bool UmbrielIpc::connected() const { return m_impl->fd >= 0; }

  void UmbrielIpc::setScreenCastActive(bool active) { m_impl->setScreenCastActive(active); }

} // namespace xdpu
