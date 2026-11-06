#include "loop/loop.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>

namespace xdpu {

  namespace {

    constexpr int kMaxEpollEvents = 32;

    void closeFd(int fd) {
      if (fd >= 0) {
        while (::close(fd) < 0 && errno == EINTR) {
        }
      }
    }

  } // namespace

  struct Loop::Impl {
    struct FdEntry {
      int fd = -1;
      uint32_t events = 0;
      FdCallback callback;
    };

    struct TimerEntry {
      int fd = -1;
      TimerCallback callback;
    };

    int epollFd = -1;
    int nextId = 1;
    bool running = false;
    std::map<int, FdEntry> fds;
    std::map<int, TimerEntry> timers;

    int allocateId() { return nextId++; }
  };

  Loop::Loop() : m_impl(std::make_unique<Impl>()) {
    m_impl->epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_impl->epollFd < 0) {
      throw std::runtime_error(std::string("epoll_create1 failed: ") + std::strerror(errno));
    }
  }

  Loop::~Loop() {
    for (const auto& [id, timer] : m_impl->timers) {
      (void)id;
      closeFd(timer.fd);
    }
    m_impl->timers.clear();
    m_impl->fds.clear();
    closeFd(m_impl->epollFd);
  }

  int Loop::addFd(int fd, uint32_t events, FdCallback cb) {
    if (fd < 0 || !cb) {
      return 0;
    }

    const int id = m_impl->allocateId();
    epoll_event event{};
    event.events = events;
    event.data.u64 = static_cast<uint64_t>(id);

    if (::epoll_ctl(m_impl->epollFd, EPOLL_CTL_ADD, fd, &event) < 0) {
      std::println(stderr, "loop: epoll_ctl ADD failed for fd {}: {}", fd, std::strerror(errno));
      return 0;
    }

    m_impl->fds.emplace(id, Impl::FdEntry{fd, events, std::move(cb)});
    return id;
  }

  void Loop::updateFd(int id, uint32_t events) {
    auto it = m_impl->fds.find(id);
    if (it == m_impl->fds.end()) {
      return;
    }

    epoll_event event{};
    event.events = events;
    event.data.u64 = static_cast<uint64_t>(id);

    if (::epoll_ctl(m_impl->epollFd, EPOLL_CTL_MOD, it->second.fd, &event) < 0) {
      std::println(stderr, "loop: epoll_ctl MOD failed for fd {}: {}", it->second.fd, std::strerror(errno));
      return;
    }

    it->second.events = events;
  }

  void Loop::removeFd(int id) {
    auto it = m_impl->fds.find(id);
    if (it == m_impl->fds.end()) {
      return;
    }

    if (::epoll_ctl(m_impl->epollFd, EPOLL_CTL_DEL, it->second.fd, nullptr) < 0 && errno != EBADF && errno != ENOENT) {
      std::println(stderr, "loop: epoll_ctl DEL failed for fd {}: {}", it->second.fd, std::strerror(errno));
    }

    m_impl->fds.erase(it);
  }

  int Loop::addTimer(int timeoutMs, TimerCallback cb) {
    if (!cb) {
      return 0;
    }

    const int timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd < 0) {
      std::println(stderr, "loop: timerfd_create failed: {}", std::strerror(errno));
      return 0;
    }

    itimerspec spec{};
    if (timeoutMs <= 0) {
      spec.it_value.tv_nsec = 1;
    } else {
      spec.it_value.tv_sec = timeoutMs / 1000;
      spec.it_value.tv_nsec = static_cast<long>(timeoutMs % 1000) * 1000 * 1000;
    }

    if (::timerfd_settime(timerFd, 0, &spec, nullptr) < 0) {
      std::println(stderr, "loop: timerfd_settime failed: {}", std::strerror(errno));
      closeFd(timerFd);
      return 0;
    }

    const int id = m_impl->allocateId();
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = static_cast<uint64_t>(id);

    if (::epoll_ctl(m_impl->epollFd, EPOLL_CTL_ADD, timerFd, &event) < 0) {
      std::println(stderr, "loop: epoll_ctl ADD failed for timerfd {}: {}", timerFd, std::strerror(errno));
      closeFd(timerFd);
      return 0;
    }

    m_impl->timers.emplace(id, Impl::TimerEntry{timerFd, std::move(cb)});
    m_impl->fds.emplace(
        id, Impl::FdEntry{timerFd, EPOLLIN, [this, id, timerFd](uint32_t) {
                            uint64_t expirations = 0;
                            while (::read(timerFd, &expirations, sizeof(expirations)) < 0) {
                              if (errno == EINTR) {
                                continue;
                              }
                              if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                std::println(stderr, "loop: timerfd read failed: {}", std::strerror(errno));
                              }
                              break;
                            }

                            auto timerIt = m_impl->timers.find(id);
                            if (timerIt == m_impl->timers.end()) {
                              return;
                            }

                            auto callback = std::move(timerIt->second.callback);
                            if (callback) {
                              callback();
                            }
                            removeTimer(id);
                          }}
    );
    return id;
  }

  void Loop::removeTimer(int id) {
    auto timerIt = m_impl->timers.find(id);
    if (timerIt == m_impl->timers.end()) {
      removeFd(id);
      return;
    }

    const int timerFd = timerIt->second.fd;
    m_impl->timers.erase(timerIt);
    removeFd(id);
    closeFd(timerFd);
  }

  void Loop::run() {
    m_impl->running = true;

    while (m_impl->running) {
      epoll_event events[kMaxEpollEvents]{};
      const int count = ::epoll_wait(m_impl->epollFd, events, kMaxEpollEvents, -1);
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        std::println(stderr, "loop: epoll_wait failed: {}", std::strerror(errno));
        break;
      }

      for (int i = 0; i < count && m_impl->running; ++i) {
        const int id = static_cast<int>(events[i].data.u64);
        auto it = m_impl->fds.find(id);
        if (it == m_impl->fds.end()) {
          continue;
        }

        auto callback = it->second.callback;
        if (callback) {
          callback(events[i].events);
        }
      }
    }

    m_impl->running = false;
  }

  void Loop::quit() { m_impl->running = false; }

} // namespace xdpu
