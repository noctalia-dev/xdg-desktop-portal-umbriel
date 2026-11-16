#include "wayland/global_shortcuts.h"

#include "loop/loop.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <umbriel-global-shortcuts-v1-client-protocol.h>
#include <utility>
#include <wayland-client.h>

namespace xdpu {
  namespace {
    constexpr size_t kSessionLimit = 16;
    constexpr size_t kOperationLimit = 64;
    constexpr size_t kShortcutLimit = 128;

    bool validText(std::string_view text, size_t limit, bool allowEmpty = true) {
      if ((!allowEmpty && text.empty()) || text.size() > limit || text.contains('\0')) {
        return false;
      }
      try {
        nlohmann::json(text).dump();
      } catch (const nlohmann::json::exception&) {
        return false;
      }
      return true;
    }

    bool
    appendShortcut(std::vector<GlobalShortcut>& entries, const char* id, const char* description, const char* trigger) {
      if (entries.size() >= kShortcutLimit
          || !validText(id, 256, false)
          || !validText(description, 1024)
          || !validText(trigger, 256, false)
          || std::ranges::any_of(entries, [id](const auto& entry) { return entry.id == id; })) {
        return false;
      }
      entries.push_back({id, description, trigger});
      return true;
    }
  } // namespace

  struct GlobalShortcutsClient::Impl : std::enable_shared_from_this<Impl> {
    Loop& loop;
    WaylandContext& wayland;
    GlobalShortcutTimeouts timeouts;
    wl_registry* registry = nullptr;
    umbriel_global_shortcuts_manager_v1* manager = nullptr;
    uint32_t managerName = 0;
    uint64_t nextSession = 0;
    size_t operations = 0;
    bool stopping = false;
    std::map<uint64_t, std::shared_ptr<Session::State>> sessions;

    Impl(Loop& loop, WaylandContext& wayland, GlobalShortcutTimeouts timeouts)
        : loop(loop), wayland(wayland), timeouts(timeouts) {}
    void initialize();
    void closeAll();
    void shutdown();
    bool available() const { return !stopping && manager != nullptr && wayland.connected(); }
    static void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    static void onRemove(void* data, wl_registry*, uint32_t name);
  };

  struct GlobalShortcutsClient::Session::State : std::enable_shared_from_this<State> {
    enum class Kind : uint8_t { Restore, Bind, List };
    struct Operation {
      Kind kind;
      Completion completion;
      std::vector<GlobalShortcut> shortcuts;
      std::string credential;
      int timer = 0;
    };

    std::weak_ptr<GlobalShortcutsClient::Impl> owner;
    uint64_t id;
    umbriel_global_shortcuts_session_v1* proxy = nullptr;
    GlobalShortcutEvents events;
    std::map<uint32_t, Operation> operations;
    std::vector<GlobalShortcut> changes;
    uint32_t nextRequest = 0;
    bool closed = false;
    bool restoreAttempted = false;
    bool bindAttempted = false;
    bool listed = false;

    State(const std::shared_ptr<GlobalShortcutsClient::Impl>& owner, uint64_t id, GlobalShortcutEvents events)
        : owner(owner), id(id), events(std::move(events)) {}

    std::optional<uint32_t> add(Kind kind, Completion completion) {
      const auto client = owner.lock();
      if (closed || !client || !client->available() || !completion || client->operations >= kOperationLimit) {
        return std::nullopt;
      }
      do {
        ++nextRequest;
      } while (nextRequest == 0 || operations.contains(nextRequest));
      const auto request = nextRequest;
      const std::weak_ptr<State> weak = shared_from_this();
      const int timer = client->loop.addTimer(
          kind == Kind::Bind ? client->timeouts.approvalMs : client->timeouts.operationMs, [weak, request] {
            if (const auto state = weak.lock()) {
              const auto operation = state->operations.find(request);
              if (operation != state->operations.end()) {
                operation->second.timer = 0;
                state->close();
              }
            }
          }
      );
      if (timer == 0) {
        return std::nullopt;
      }
      operations.emplace(request, Operation{kind, std::move(completion), {}, {}, timer});
      ++client->operations;
      return request;
    }

    void finish(uint32_t request, Kind kind, uint32_t status) {
      const auto keepAlive = shared_from_this();
      const auto found = operations.find(request);
      if (found == operations.end()) {
        return;
      }
      if (found->second.kind != kind
          || status > 2
          || (kind == Kind::Restore && status == 0 && found->second.credential.empty())) {
        close();
        return;
      }
      auto operation = std::move(found->second);
      operations.erase(found);
      if (const auto client = owner.lock()) {
        client->loop.removeTimer(operation.timer);
        --client->operations;
      }
      GlobalShortcutResult result{static_cast<GlobalShortcutStatus>(status), {}, {}};
      if (status == 0) {
        result.shortcuts = std::move(operation.shortcuts);
        result.credential = std::move(operation.credential);
      }
      operation.completion(std::move(result));
    }

    void close() { close(std::nullopt); }

    void close(std::optional<uint32_t> cancelled) {
      if (closed) {
        return;
      }
      const auto keepAlive = shared_from_this();
      closed = true;
      if (proxy != nullptr) {
        umbriel_global_shortcuts_session_v1_destroy(std::exchange(proxy, nullptr));
      }
      auto pending = std::exchange(operations, {});
      auto callbacks = std::exchange(events, {});
      changes.clear();
      if (const auto client = owner.lock()) {
        client->sessions.erase(id);
        client->operations -= pending.size();
        for (const auto& [request, operation] : pending) {
          (void)request;
          client->loop.removeTimer(operation.timer);
        }
        client->wayland.flush();
      }
      for (auto& [request, operation] : pending) {
        operation.completion(
            {cancelled == request ? GlobalShortcutStatus::Cancelled : GlobalShortcutStatus::Failed, {}, {}}
        );
      }
      if (callbacks.closed) {
        callbacks.closed();
      }
    }

    static std::shared_ptr<State> state(void* data) { return static_cast<State*>(data)->shared_from_this(); }

    static const umbriel_global_shortcuts_session_v1_listener kListener;
  };

  const umbriel_global_shortcuts_session_v1_listener GlobalShortcutsClient::Session::State::kListener{
      .grant =
          [](void* data, auto*, uint32_t request, const char* credential) {
            const auto self = state(data);
            const auto found = self->operations.find(request);
            if (found == self->operations.end()) {
              return;
            }
            if (found->second.kind == Kind::List
                || !found->second.credential.empty()
                || !validText(credential, 4096, false)) {
              self->close();
              return;
            }
            found->second.credential = credential;
          },
      .restored = [](void* data, auto*, uint32_t request,
                     uint32_t status) { state(data)->finish(request, Kind::Restore, status); },
      .shortcut =
          [](void* data, auto*, uint32_t request, const char* id, const char* description, const char* trigger) {
            const auto self = state(data);
            const auto found = self->operations.find(request);
            if (found == self->operations.end()) {
              return;
            }
            if (found->second.kind == Kind::Restore
                || !appendShortcut(found->second.shortcuts, id, description, trigger)) {
              self->close();
            }
          },
      .bound = [](void* data, auto*, uint32_t request,
                  uint32_t status) { state(data)->finish(request, Kind::Bind, status); },
      .listed = [](void* data, auto*, uint32_t request,
                   uint32_t status) { state(data)->finish(request, Kind::List, status); },
      .activated =
          [](void* data, auto*, const char* id, uint32_t time) {
            const auto self = state(data);
            if (!validText(id, 256, false)) {
              self->close();
              return;
            }
            const auto callback = self->events.activated;
            if (callback) {
              callback(id, time);
            }
          },
      .deactivated =
          [](void* data, auto*, const char* id, uint32_t time) {
            const auto self = state(data);
            if (!validText(id, 256, false)) {
              self->close();
              return;
            }
            const auto callback = self->events.deactivated;
            if (callback) {
              callback(id, time);
            }
          },
      .changed =
          [](void* data, auto*, const char* id, const char* description, const char* trigger) {
            const auto self = state(data);
            if (!appendShortcut(self->changes, id, description, trigger)) {
              self->close();
            }
          },
      .changes_done =
          [](void* data, auto*) {
            const auto self = state(data);
            auto entries = std::exchange(self->changes, {});
            const auto callback = self->events.changed;
            if (callback) {
              callback(std::move(entries));
            }
          },
      .closed = [](void* data, auto*) { state(data)->close(); },
  };

  void GlobalShortcutsClient::Impl::initialize() {
    if (!wayland.connected()) {
      return;
    }
    registry = wl_display_get_registry(wayland.display());
    if (registry == nullptr) {
      return;
    }
    static constexpr wl_registry_listener listener{.global = onGlobal, .global_remove = onRemove};
    wl_registry_add_listener(registry, &listener, this);
    wayland.roundtrip();
  }

  void GlobalShortcutsClient::Impl::closeAll() {
    const auto snapshot = sessions;
    for (const auto& [id, session] : snapshot) {
      (void)id;
      session->close();
    }
  }

  void GlobalShortcutsClient::Impl::shutdown() {
    if (stopping) {
      return;
    }
    stopping = true;
    closeAll();
    if (manager != nullptr) {
      umbriel_global_shortcuts_manager_v1_destroy(std::exchange(manager, nullptr));
    }
    if (registry != nullptr) {
      wl_registry_destroy(std::exchange(registry, nullptr));
    }
    wayland.flush();
  }

  void GlobalShortcutsClient::Impl::onGlobal(
      void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version
  ) {
    const auto self = static_cast<Impl*>(data)->shared_from_this();
    if (!self->stopping
        && self->manager == nullptr
        && version >= 1
        && std::string_view(interface) == umbriel_global_shortcuts_manager_v1_interface.name) {
      self->manager = static_cast<umbriel_global_shortcuts_manager_v1*>(
          wl_registry_bind(registry, name, &umbriel_global_shortcuts_manager_v1_interface, 1)
      );
      self->managerName = name;
    }
  }

  void GlobalShortcutsClient::Impl::onRemove(void* data, wl_registry*, uint32_t name) {
    const auto self = static_cast<Impl*>(data)->shared_from_this();
    if (self->manager != nullptr && name == self->managerName) {
      umbriel_global_shortcuts_manager_v1_destroy(std::exchange(self->manager, nullptr));
      self->managerName = 0;
      self->closeAll();
    }
  }

  GlobalShortcutsClient::GlobalShortcutsClient(Loop& loop, WaylandContext& wayland, GlobalShortcutTimeouts timeouts)
      : m_impl(std::make_shared<Impl>(loop, wayland, timeouts)) {
    if (timeouts.operationMs <= 0 || timeouts.approvalMs <= 0) {
      throw std::invalid_argument("Global shortcut deadlines must be positive");
    }
    m_impl->initialize();
  }
  GlobalShortcutsClient::~GlobalShortcutsClient() { m_impl->shutdown(); }
  bool GlobalShortcutsClient::available() const { return m_impl->available(); }

  std::shared_ptr<GlobalShortcutsClient::Session>
  GlobalShortcutsClient::createSession(std::string_view application, GlobalShortcutEvents events) {
    if (!available() || m_impl->sessions.size() >= kSessionLimit || !validText(application, 256, false)) {
      return nullptr;
    }
    do {
      ++m_impl->nextSession;
    } while (m_impl->nextSession == 0 || m_impl->sessions.contains(m_impl->nextSession));
    auto state = std::make_shared<Session::State>(m_impl, m_impl->nextSession, std::move(events));
    state->proxy =
        umbriel_global_shortcuts_manager_v1_create_session(m_impl->manager, std::string(application).c_str());
    if (state->proxy == nullptr) {
      return nullptr;
    }
    umbriel_global_shortcuts_session_v1_add_listener(state->proxy, &Session::State::kListener, state.get());
    m_impl->sessions.emplace(state->id, state);
    m_impl->wayland.flush();
    return std::make_shared<Session>(std::move(state));
  }

  GlobalShortcutsClient::Session::Session(std::shared_ptr<State> state) : m_state(std::move(state)) {
    if (!m_state) {
      throw std::invalid_argument("Global shortcut session state is required");
    }
  }
  GlobalShortcutsClient::Session::~Session() { m_state->close(); }
  void GlobalShortcutsClient::Session::close() { m_state->close(); }
  bool GlobalShortcutsClient::Session::closed() const { return m_state->closed; }

  std::optional<uint32_t> GlobalShortcutsClient::Session::restore(std::string_view credential, Completion completion) {
    const auto state = m_state;
    if (state->restoreAttempted || state->bindAttempted || state->listed || !validText(credential, 4096, false)) {
      return std::nullopt;
    }
    const auto request = state->add(State::Kind::Restore, std::move(completion));
    if (request) {
      state->restoreAttempted = true;
      umbriel_global_shortcuts_session_v1_restore(state->proxy, *request, std::string(credential).c_str());
      state->owner.lock()->wayland.flush();
    }
    return request;
  }

  std::optional<uint32_t>
  GlobalShortcutsClient::Session::bind(std::span<const GlobalShortcutRequest> shortcuts, Completion completion) {
    const auto state = m_state;
    if (state->bindAttempted || shortcuts.size() > kShortcutLimit) {
      return std::nullopt;
    }
    std::set<std::string_view> ids;
    for (const auto& shortcut : shortcuts) {
      if (!validText(shortcut.id, 256, false)
          || !validText(shortcut.description, 1024)
          || !validText(shortcut.preferredTrigger, 256)
          || !ids.insert(shortcut.id).second) {
        return std::nullopt;
      }
    }
    const auto request = state->add(State::Kind::Bind, std::move(completion));
    if (request) {
      state->bindAttempted = true;
      for (const auto& shortcut : shortcuts) {
        umbriel_global_shortcuts_session_v1_add_shortcut(
            state->proxy, shortcut.id.c_str(), shortcut.description.c_str(), shortcut.preferredTrigger.c_str()
        );
      }
      umbriel_global_shortcuts_session_v1_bind(state->proxy, *request);
      state->owner.lock()->wayland.flush();
    }
    return request;
  }

  std::optional<uint32_t> GlobalShortcutsClient::Session::list(Completion completion) {
    const auto state = m_state;
    const auto request = state->add(State::Kind::List, std::move(completion));
    if (request) {
      state->listed = true;
      umbriel_global_shortcuts_session_v1_list(state->proxy, *request);
      state->owner.lock()->wayland.flush();
    }
    return request;
  }

  void GlobalShortcutsClient::Session::cancel(uint32_t request) {
    const auto state = m_state;
    const auto found = state->operations.find(request);
    if (found == state->operations.end()) {
      return;
    }
    const auto kind = found->second.kind;
    if (kind == State::Kind::Restore) {
      state->close(request);
      return;
    }
    if (kind == State::Kind::Bind) {
      umbriel_global_shortcuts_session_v1_cancel(state->proxy, request);
      state->owner.lock()->wayland.flush();
    }
    state->finish(request, kind, 1);
  }
} // namespace xdpu
