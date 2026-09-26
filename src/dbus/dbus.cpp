#include "dbus/dbus.h"

#include "config/config.h"
#include "dbus/screencast.h"
#include "dbus/screenshot.h"
#include "loop/loop.h"
#include "pipewire/pipewire.h"
#include "umbriel/ipc.h"
#include "wayland/wayland.h"

#include <csignal>
#include <cstdio>
#include <exception>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <sys/epoll.h>

namespace xdpu {

  namespace {

    constexpr char kBusName[] = "org.freedesktop.impl.portal.desktop.umbriel";
    constexpr char kDesktopPath[] = "/org/freedesktop/portal/desktop";

    uint32_t pollEventsToEpoll(short events) {
      uint32_t epollEvents = 0;
      if ((events & POLLIN) != 0) {
        epollEvents |= EPOLLIN;
      }
      if ((events & POLLOUT) != 0) {
        epollEvents |= EPOLLOUT;
      }
      if ((events & POLLPRI) != 0) {
        epollEvents |= EPOLLPRI;
      }
      if ((events & POLLERR) != 0) {
        epollEvents |= EPOLLERR;
      }
      if ((events & POLLHUP) != 0) {
        epollEvents |= EPOLLHUP;
      }
      return epollEvents;
    }

  } // namespace

  struct DbusPortal::Impl {
    Loop& loop;
    Config config;
    WaylandContext& wayland;
    PipeWireContext& pipewire;
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> desktopObject;
    std::unique_ptr<ScreenshotPortal> screenshot;
    std::unique_ptr<ScreenCastPortal> screencast;
    int busFd = -1;
    int busFdWatch = 0;
    int eventFd = -1;
    int eventFdWatch = 0;
    int timeoutTimer = 0;

    Impl(Loop& loop, const Config& config, WaylandContext& wayland, PipeWireContext& pipewire)
        : loop(loop), config(config), wayland(wayland), pipewire(pipewire) {
      std::signal(SIGPIPE, SIG_IGN);
      connection = sdbus::createSessionBusConnection(sdbus::ServiceName{kBusName});
      desktopObject = sdbus::createObject(*connection, sdbus::ObjectPath{kDesktopPath});

      screenshot = std::make_unique<ScreenshotPortal>(loop, *connection, *desktopObject, this->config, wayland);
      screencast =
          std::make_unique<ScreenCastPortal>(loop, *connection, *desktopObject, this->config, wayland, pipewire);

      refreshPollData();
      drain();
    }

    ~Impl() {
      if (timeoutTimer != 0) {
        loop.removeTimer(timeoutTimer);
        timeoutTimer = 0;
      }
      if (busFdWatch != 0) {
        loop.removeFd(busFdWatch);
        busFdWatch = 0;
      }
      if (eventFdWatch != 0) {
        loop.removeFd(eventFdWatch);
        eventFdWatch = 0;
      }
      screencast.reset();
      screenshot.reset();
      desktopObject.reset();
      connection.reset();
    }

    void updateWatchedFd(int fd, uint32_t events, int& currentFd, int& watchId) {
      if (fd < 0 || events == 0) {
        if (watchId != 0) {
          loop.removeFd(watchId);
          watchId = 0;
        }
        currentFd = -1;
        return;
      }

      if (watchId != 0 && fd != currentFd) {
        loop.removeFd(watchId);
        watchId = 0;
        currentFd = -1;
      }

      auto callback = [this](uint32_t) { drain(); };
      if (watchId == 0) {
        watchId = loop.addFd(fd, events, std::move(callback));
        if (watchId == 0) {
          std::fprintf(stderr, "dbus: failed to register bus fd %d with event loop\n", fd);
          return;
        }
        currentFd = fd;
      } else {
        loop.updateFd(watchId, events);
      }
    }

    void refreshPollData() {
      if (!connection) {
        return;
      }

      sdbus::IConnection::PollData pollData{};
      try {
        pollData = connection->getEventLoopPollData();
      } catch (const std::exception& error) {
        std::fprintf(stderr, "dbus: getEventLoopPollData failed: %s\n", error.what());
        return;
      }

      updateWatchedFd(pollData.fd, pollEventsToEpoll(pollData.events), busFd, busFdWatch);
      updateWatchedFd(pollData.eventFd, EPOLLIN, eventFd, eventFdWatch);

      if (timeoutTimer != 0) {
        loop.removeTimer(timeoutTimer);
        timeoutTimer = 0;
      }
      const int timeoutMs = pollData.getPollTimeout();
      if (timeoutMs >= 0) {
        timeoutTimer = loop.addTimer(timeoutMs, [this]() { drain(); });
      }
    }

    void drain() {
      if (!connection) {
        return;
      }
      try {
        while (connection->processPendingEvent()) {
        }
      } catch (const std::exception& error) {
        std::fprintf(stderr, "dbus: processPendingEvent failed: %s\n", error.what());
      }
      refreshPollData();
    }
  };

  DbusPortal::DbusPortal(Loop& loop, const Config& config, WaylandContext& wayland, PipeWireContext& pipewire)
      : m_impl(std::make_unique<Impl>(loop, config, wayland, pipewire)) {}

  DbusPortal::~DbusPortal() = default;

  void DbusPortal::onConfigChanged(const Config& oldCfg, const Config& newCfg) {
    m_impl->config = newCfg;
    if (m_impl->screenshot) {
      m_impl->screenshot->onConfigChanged(oldCfg, newCfg);
    }
    if (m_impl->screencast) {
      m_impl->screencast->onConfigChanged(oldCfg, newCfg);
    }
  }

  void DbusPortal::onScreenCastCommand(const ScreenCastCommand& command) {
    if (m_impl->screencast) {
      m_impl->screencast->onScreenCastCommand(command);
    }
  }

  void DbusPortal::onFocusedWindowChanged(const std::optional<std::string>& identifier) {
    if (m_impl->screencast) {
      m_impl->screencast->onFocusedWindowChanged(identifier);
    }
  }

  void DbusPortal::onFocusedOutputChanged(const std::optional<std::string>& output) {
    if (m_impl->screencast) {
      m_impl->screencast->onFocusedOutputChanged(output);
    }
  }

  void DbusPortal::setScreenCastActiveHandler(std::function<void(bool)> handler) {
    if (m_impl->screencast) {
      m_impl->screencast->setActiveChangedHandler(std::move(handler));
    }
  }

} // namespace xdpu
