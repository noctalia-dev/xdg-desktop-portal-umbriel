#include "config/config.h"
#include "dbus/dbus.h"
#include "loop/loop.h"
#include "pipewire/pipewire.h"
#include "umbriel/ipc.h"
#include "wayland/wayland.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

int main() {
  try {
    xdpu::Loop loop;
    auto config = xdpu::loadConfig();

    xdpu::WaylandContext wayland(loop);
    if (!wayland.connected()) {
      return 1;
    }
    xdpu::PipeWireContext pipewire(loop);
    xdpu::DbusPortal portal(loop, config, wayland, pipewire);
    xdpu::UmbrielIpc umbrielIpc(
        loop, [&portal](const xdpu::ScreenCastCommand& command) { portal.onScreenCastCommand(command); },
        [&portal](const std::optional<std::string>& identifier) { portal.onFocusedWindowChanged(identifier); },
        [&portal](const std::optional<std::string>& output) { portal.onFocusedOutputChanged(output); }
    );
    portal.setScreenCastActiveHandler([&umbrielIpc](bool active) { umbrielIpc.setScreenCastActive(active); });

    xdpu::ConfigWatcher watcher(loop, [&](const auto& oldCfg, const auto& newCfg) {
      config = newCfg;
      portal.onConfigChanged(oldCfg, newCfg);
    });

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
      std::fprintf(stderr, "main: sigprocmask failed: %s\n", std::strerror(errno));
      return 1;
    }

    const int sigFd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigFd < 0) {
      std::fprintf(stderr, "main: signalfd failed: %s\n", std::strerror(errno));
      return 1;
    }

    const int sigWatch = loop.addFd(sigFd, EPOLLIN, [&](uint32_t) {
      signalfd_siginfo info{};
      while (true) {
        const ssize_t size = read(sigFd, &info, sizeof(info));
        if (size == static_cast<ssize_t>(sizeof(info))) {
          loop.quit();
          continue;
        }
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          std::fprintf(stderr, "main: signalfd read failed: %s\n", std::strerror(errno));
        }
        break;
      }
      loop.quit();
    });

    if (sigWatch == 0) {
      std::fprintf(stderr, "main: unable to add signalfd to loop\n");
      close(sigFd);
      return 1;
    }

    loop.run();
    loop.removeFd(sigWatch);
    close(sigFd);
    return wayland.connected() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "main: fatal error: %s\n", error.what());
    return 1;
  }
}
