#pragma once

#include "core/deferred_call.h"
#include "core/poll_source.h"
#include "core/timer_manager.h"

#include <functional>
#include <poll.h>
#include <vector>

class WaylandConnection;

namespace xdpu::picker {

  // Polls the Wayland display and the sources until `running` turns false; the same
  // ordering as luciole's loop (prepare_read, flush, poll, read_events, dispatch).
  class MainLoop {
  public:
    MainLoop(WaylandConnection& wayland, std::vector<PollSource*> sources, std::function<bool()> running);
    void run();

  private:
    WaylandConnection& m_wayland;
    std::vector<PollSource*> m_sources;
    std::function<bool()> m_running;
  };

  // Runs callbacks posted with DeferredCall::callLater, including those from worker threads.
  class DeferredCallPollSource final : public PollSource {
  public:
    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
      if (startIdx < fds.size() && (fds[startIdx].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
        DeferredCall::drainWakeFd();
      }
      for (auto& fn : DeferredCall::takePending()) {
        if (fn) {
          fn();
        }
      }
    }

  private:
    void doAddPollFds(std::vector<pollfd>& fds) override {
      if (const int fd = DeferredCall::wakeFd(); fd >= 0) {
        fds.push_back({.fd = fd, .events = POLLIN, .revents = 0});
      }
    }
  };

  class TimerPollSource final : public PollSource {
  public:
    int pollTimeoutMs() const override { return TimerManager::instance().pollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
      TimerManager::instance().tick();
    }

  private:
    void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}
  };

} // namespace xdpu::picker
