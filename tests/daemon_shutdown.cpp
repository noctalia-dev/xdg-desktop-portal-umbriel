#include "dbus/global_shortcuts_credentials.h"
#include "global_shortcuts_fixture.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <utility>

namespace {
  using shortcut_test::Json;
  using shortcut_test::require;
  using Dict = std::map<std::string, sdbus::Variant>;
  constexpr auto kBackend = "org.freedesktop.impl.portal.desktop.umbriel";
  constexpr auto kInterface = "org.freedesktop.impl.portal.GlobalShortcuts";
  constexpr auto kDesktop = "/org/freedesktop/portal/desktop";

  struct Runtime {
    std::filesystem::path root;

    Runtime() {
      std::array<char, 48> pattern{};
      std::string_view("/tmp/xdpu-daemon-shutdown-XXXXXX").copy(pattern.data(), pattern.size());
      const auto* created = mkdtemp(pattern.data());
      require(created != nullptr, "private runtime directory creation failed");
      root = created;
      for (const auto* variable :
           {"XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
            "XDG_CONFIG_DIRS", "XDG_DATA_DIRS"}) {
        const auto path = root / variable;
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
        require(setenv(variable, path.c_str(), 1) == 0, "private environment setup failed");
      }
      require(
          setenv("PIPEWIRE_RUNTIME_DIR", (root / "XDG_RUNTIME_DIR").c_str(), 1) == 0
              && setenv("PIPEWIRE_REMOTE", "no-server", 1) == 0
              && setenv("UMBRIEL_SOCKET", (root / "no-compositor-ipc").c_str(), 1) == 0
              && setenv("WAYLAND_DISPLAY", "daemon-shutdown-isolated", 1) == 0
              && setenv("DBUS_SYSTEM_BUS_ADDRESS", ("unix:path=" + (root / "no-system-bus").string()).c_str(), 1) == 0,
          "private service environment setup failed"
      );
      for (const auto* variable :
           {"WAYLAND_SOCKET", "DISPLAY", "NOTIFY_SOCKET", "SYSTEMD_EXEC_PID", "PIPEWIRE_CONFIG_DIR",
            "PIPEWIRE_CONFIG_NAME", "PIPEWIRE_REMOTE_FD", "PIPEWIRE_CORE"}) {
        require(unsetenv(variable) == 0, "inherited environment cleanup failed");
      }
    }
    ~Runtime() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
  };

  struct Child {
    pid_t pid = -1;
    std::optional<int> status;

    ~Child() {
      if (pid > 0) {
        kill(pid, SIGKILL);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      }
    }
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    bool exited() {
      if (status) {
        return true;
      }
      int result = 0;
      const auto waited = waitpid(pid, &result, WNOHANG);
      if (waited == pid) {
        status = result;
        pid = -1;
        return true;
      }
      require(waited == 0 || (waited < 0 && errno == EINTR), "child process wait failed");
      return false;
    }
  };

  struct SocketPair {
    std::array<int, 2> fds{-1, -1};
    SocketPair() {
      require(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) == 0, "private socketpair failed");
    }
    ~SocketPair() {
      closeEnd(0);
      closeEnd(1);
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    void closeEnd(size_t index) {
      if (fds[index] >= 0) {
        close(std::exchange(fds[index], -1));
      }
    }
  };

  Json command(int fd, const Json& value) {
    shortcut_test::writeLine(fd, value);
    std::string line;
    for (;;) {
      pollfd ready{fd, POLLIN, 0};
      require(poll(&ready, 1, 2000) > 0, "private server control timed out");
      char byte = 0;
      require(read(fd, &byte, 1) == 1, "private server exited unexpectedly");
      if (byte == '\n') {
        return Json::parse(line);
      }
      line += byte;
    }
  }

  void until(sdbus::IConnection& bus, const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
      require(std::chrono::steady_clock::now() < deadline, "daemon shutdown check timed out");
      while (bus.processPendingEvent()) {
      }
      const auto data = bus.getEventLoopPollData();
      const auto busTimeout = data.getPollTimeout();
      const auto timeout = busTimeout < 0 ? 10 : std::min(busTimeout, 10);
      std::array<pollfd, 2> fds{{{data.fd, data.events, 0}, {data.eventFd, POLLIN, 0}}};
      require(poll(fds.data(), fds.size(), timeout) >= 0 || errno == EINTR, "private bus poll failed");
    }
  }

  void checkShutdown(const char* executable, int signal) {
    SocketPair display;
    SocketPair control;
    Child server;
    server.pid = fork();
    require(server.pid >= 0, "private server fork failed");
    if (server.pid == 0) {
      display.closeEnd(0);
      control.closeEnd(0);
      try {
        {
          shortcut_test::Server privateServer(display.fds[1], control.fds[1], true);
          wl_display_run(privateServer.display);
        }
        _exit(0);
      } catch (...) {
        _exit(1);
      }
    }
    display.closeEnd(1);
    control.closeEnd(1);
    auto bus = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.portal.Desktop"});
    Child daemon;
    daemon.pid = fork();
    require(daemon.pid >= 0, "daemon fork failed");
    if (daemon.pid == 0) {
      control.closeEnd(0);
      const int flags = fcntl(display.fds[0], F_GETFD);
      if (flags < 0 || fcntl(display.fds[0], F_SETFD, flags & ~FD_CLOEXEC) != 0) {
        _exit(2);
      }
      // The launcher must not supply the mask that the daemon needs to establish.
      sigset_t unblocked;
      sigemptyset(&unblocked);
      sigaddset(&unblocked, SIGINT);
      sigaddset(&unblocked, SIGTERM);
      std::signal(SIGINT, SIG_DFL);
      std::signal(SIGTERM, SIG_DFL);
      if (sigprocmask(SIG_UNBLOCK, &unblocked, nullptr) != 0
          || setenv("WAYLAND_SOCKET", std::to_string(display.fds[0]).c_str(), 1) != 0) {
        _exit(2);
      }
      execl(executable, executable, static_cast<char*>(nullptr));
      _exit(2);
    }
    display.closeEnd(0);
    auto broker = sdbus::createProxy(
        *bus, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
    );
    auto portal = sdbus::createProxy(*bus, sdbus::ServiceName{kBackend}, sdbus::ObjectPath{kDesktop});
    until(*bus, [&] {
      require(!daemon.exited(), "daemon exited before becoming ready");
      bool owned = false;
      broker->callMethod("NameHasOwner")
          .onInterface("org.freedesktop.DBus")
          .withArguments(std::string{kBackend})
          .storeResultsTo(owned);
      if (!owned) {
        return false;
      }
      try {
        sdbus::Variant version;
        portal->callMethod("Get")
            .withTimeout(std::chrono::seconds(1))
            .onInterface("org.freedesktop.DBus.Properties")
            .withArguments(std::string{kInterface}, std::string{"version"})
            .storeResultsTo(version);
        require(version.get<uint32_t>() == 1, "unexpected shortcut portal version");
        return true;
      } catch (const sdbus::Error&) {
        return false;
      }
    });
    const sdbus::ObjectPath path{"/org/freedesktop/portal/desktop/session/1_1/shutdown"};
    uint32_t response = 99;
    Dict results;
    portal->callMethod("CreateSession")
        .withTimeout(std::chrono::seconds(1))
        .onInterface(kInterface)
        .withArguments(
            sdbus::ObjectPath{"/org/freedesktop/portal/desktop/request/1_1/create"}, path,
            std::string{"org.test.Shutdown"}, Dict{}
        )
        .storeResultsTo(response, results);
    require(response == 0 && results.empty(), "daemon did not create a shortcut session");

    bool completed = false;
    bool closed = false;
    std::optional<sdbus::Error> pendingError;
    auto session = sdbus::createProxy(*bus, sdbus::ServiceName{kBackend}, path);
    session->uponSignal("Closed").onInterface("org.freedesktop.impl.portal.Session").call([&] { closed = true; });
    using Shortcuts = std::vector<sdbus::Struct<std::string, Dict>>;
    portal->callMethodAsync("BindShortcuts")
        .onInterface(kInterface)
        .withArguments(
            sdbus::ObjectPath{"/org/freedesktop/portal/desktop/request/1_1/bind"}, path, Shortcuts{}, std::string{},
            Dict{}
        )
        .uponReplyInvoke([&](std::optional<sdbus::Error> error, uint32_t status, Dict data) {
          pendingError = std::move(error);
          response = status;
          results = std::move(data);
          completed = true;
        });
    until(*bus, [&] {
      if (command(control.fds[0], {{"op", "sessions"}}) == 0) {
        return false;
      }
      const auto seen = command(control.fds[0], {{"op", "seen"}});
      return !seen.empty() && seen.back()["kind"] == "bind";
    });
    require(!completed, "bind completed without a compositor response");
    require(kill(daemon.pid, signal) == 0, "daemon termination signal failed");
    until(*bus, [&] { return daemon.exited(); });
    require(
        WIFEXITED(*daemon.status) && WEXITSTATUS(*daemon.status) == 0,
        "termination signal bypassed normal daemon cleanup"
    );
    until(*bus, [&] { return completed && closed; });
    require(!pendingError && response == 2 && results.empty(), "shutdown did not fail its pending bind cleanly");
    until(*bus, [&] { return !command(control.fds[0], {{"op", "alive"}}).get<bool>(); });
    auto acquired = xdpu::GlobalShortcutCredentials{}.acquire("org.test.Shutdown");
    require(acquired.status == xdpu::GlobalShortcutCacheStatus::Ready, "shutdown leaked the credential lock");
    (void)command(control.fds[0], {{"op", "quit"}});
    until(*bus, [&] { return server.exited(); });
    require(WIFEXITED(*server.status) && WEXITSTATUS(*server.status) == 0, "private server failed");
  }
} // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "daemon executable required");
    const Runtime runtime;
    checkShutdown(argv[1], SIGTERM);
    checkShutdown(argv[1], SIGINT);
    std::println("daemon shutdown handles SIGTERM and SIGINT with pending requests");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "daemon shutdown: {}", error.what());
    return EXIT_FAILURE;
  }
}
