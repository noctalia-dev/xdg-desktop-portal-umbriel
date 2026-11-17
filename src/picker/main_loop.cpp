#include "picker/main_loop.h"

#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <wayland-client.h>

namespace xdpu::picker {

  MainLoop::MainLoop(WaylandConnection& wayland, std::vector<PollSource*> sources, std::function<bool()> running)
      : m_wayland(wayland), m_sources(std::move(sources)), m_running(std::move(running)) {}

  void MainLoop::run() {
    using Clock = std::chrono::steady_clock;
    std::unordered_map<PollSource*, Clock::time_point> deadlines;
    auto* display = m_wayland.display();
    while (m_running()) {
      if (wl_display_dispatch_pending(display) < 0) {
        throw std::runtime_error("Wayland dispatch failed");
      }
      Surface::drainPendingFrameWork();
      Surface::drainPendingRenders();
      if (!m_running()) {
        break;
      }

      std::vector<pollfd> fds{{.fd = wl_display_get_fd(display), .events = POLLIN, .revents = 0}};
      std::vector<std::size_t> starts;
      int timeout = -1;
      const auto now = Clock::now();
      for (auto* source : m_sources) {
        starts.push_back(source->addPollFds(fds));
        const int requested = source->pollTimeoutMs();
        if (requested >= 0) {
          const auto proposed = now + std::chrono::milliseconds(requested);
          auto [it, inserted] = deadlines.try_emplace(source, proposed);
          if (!inserted) {
            it->second = std::min(it->second, proposed);
          }
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(it->second - now).count();
          const int vote = static_cast<int>(std::max<std::int64_t>(0, remaining));
          timeout = timeout < 0 ? vote : std::min(timeout, vote);
        } else {
          deadlines.erase(source);
        }
      }
      if (Surface::hasPendingFrameWork() || Surface::hasPendingRenders()) {
        timeout = 0;
      }

      // Every successful prepare_read is paired with read_events or cancel_read.
      if (wl_display_prepare_read(display) != 0) {
        continue;
      }
      if (wl_display_flush(display) < 0) {
        if (errno == EAGAIN) {
          fds.front().events |= POLLOUT;
        } else {
          wl_display_cancel_read(display);
          throw std::runtime_error("Wayland flush failed");
        }
      }
      const int result = ::poll(fds.data(), fds.size(), timeout);
      if (result < 0) {
        wl_display_cancel_read(display);
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("event polling failed");
      }
      if ((fds.front().revents & POLLIN) != 0) {
        if (wl_display_read_events(display) < 0) {
          throw std::runtime_error("Wayland connection lost");
        }
      } else {
        wl_display_cancel_read(display);
      }
      if ((fds.front().revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("Wayland connection closed");
      }
      if (wl_display_dispatch_pending(display) < 0) {
        throw std::runtime_error("Wayland dispatch failed");
      }
      const auto dispatchedAt = Clock::now();
      for (std::size_t i = 0; i < m_sources.size(); ++i) {
        if (auto it = deadlines.find(m_sources[i]); it != deadlines.end() && dispatchedAt >= it->second) {
          deadlines.erase(it);
        }
        m_sources[i]->dispatch(fds, starts[i]);
      }
    }
  }

} // namespace xdpu::picker
