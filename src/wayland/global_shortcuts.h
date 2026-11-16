#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xdpu {
  class Loop;
  class WaylandContext;

  struct GlobalShortcutRequest {
    std::string id;
    std::string description;
    std::string preferredTrigger;
  };

  struct GlobalShortcut {
    std::string id;
    std::string description;
    std::string triggerDescription;
    bool operator==(const GlobalShortcut&) const = default;
  };

  enum class GlobalShortcutStatus : uint8_t { Success, Cancelled, Failed };

  struct GlobalShortcutResult {
    GlobalShortcutStatus status = GlobalShortcutStatus::Failed;
    std::vector<GlobalShortcut> shortcuts;
    // Private transport data. Persist before completing a public portal request.
    std::string credential;
  };

  struct GlobalShortcutEvents {
    std::function<void(std::string id, uint64_t timeMsec)> activated;
    std::function<void(std::string id, uint64_t timeMsec)> deactivated;
    std::function<void(std::vector<GlobalShortcut>)> changed;
    std::function<void()> closed;
  };

  struct GlobalShortcutTimeouts {
    int operationMs = 10000;
    int approvalMs = 120000;
  };

  class GlobalShortcutsClient {
  public:
    struct Impl;
    using Completion = std::function<void(GlobalShortcutResult)>;

    class Session {
    public:
      struct State;
      explicit Session(std::shared_ptr<State> state);
      ~Session();
      Session(const Session&) = delete;
      Session& operator=(const Session&) = delete;

      // Rejected calls return no ID and invoke no callback.
      [[nodiscard]] std::optional<uint32_t> restore(std::string_view credential, Completion completion);
      [[nodiscard]] std::optional<uint32_t>
      bind(std::span<const GlobalShortcutRequest> shortcuts, Completion completion);
      [[nodiscard]] std::optional<uint32_t> list(Completion completion);
      // Abandoning restore closes the session because its credential may already
      // have rotated.
      void cancel(uint32_t request);
      void close();
      bool closed() const;

    private:
      std::shared_ptr<State> m_state;
    };

    // Loop and WaylandContext must outlive the client. Session handles may
    // outlive it and remain closed.
    GlobalShortcutsClient(Loop& loop, WaylandContext& wayland, GlobalShortcutTimeouts timeouts = {});
    ~GlobalShortcutsClient();
    GlobalShortcutsClient(const GlobalShortcutsClient&) = delete;
    GlobalShortcutsClient& operator=(const GlobalShortcutsClient&) = delete;

    bool available() const;
    [[nodiscard]] std::shared_ptr<Session>
    createSession(std::string_view application, GlobalShortcutEvents events = {});

  private:
    std::shared_ptr<Impl> m_impl;
  };
} // namespace xdpu
