#include "dbus/global_shortcuts.h"

#include "dbus/frontend.h"
#include "dbus/request.h"
#include "loop/loop.h"

#include <algorithm>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xdpu {
  namespace {
    constexpr auto kInterface = "org.freedesktop.impl.portal.GlobalShortcuts";
    constexpr auto kSessionInterface = "org.freedesktop.impl.portal.Session";
    constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";
    constexpr std::string_view kSessionPrefix = "/org/freedesktop/portal/desktop/session/";
    constexpr std::string_view kRequestPrefix = "/org/freedesktop/portal/desktop/request/";
    constexpr size_t kSessionLimit = 64;
    constexpr size_t kRequestLimit = 64;
    constexpr size_t kShortcutLimit = 128;
    constexpr int kCacheRetryMs = 25;
    using Shortcuts = std::vector<sdbus::Struct<std::string, PortalResults>>;
    using CacheStatus = GlobalShortcutCacheStatus;
    using BridgeStatus = GlobalShortcutStatus;

    sdbus::Error failure(const char* message) {
      return sdbus::Error{sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"}, message};
    }

    bool validText(std::string_view text, size_t limit, bool allowEmpty = true) {
      if ((!allowEmpty && text.empty()) || text.size() > limit || text.contains('\0')) {
        return false;
      }
      try {
        (void)nlohmann::json(text).dump();
        return true;
      } catch (const nlohmann::json::exception&) {
        return false;
      }
    }

    bool validPath(std::string_view path, std::string_view prefix) {
      return path.starts_with(prefix) && path.size() > prefix.size() && path.size() <= 4096;
    }

    std::optional<std::vector<GlobalShortcutRequest>> parseShortcuts(const Shortcuts& shortcuts) {
      if (shortcuts.size() > kShortcutLimit) {
        return std::nullopt;
      }
      std::set<std::string_view> ids;
      std::vector<GlobalShortcutRequest> parsed;
      parsed.reserve(shortcuts.size());
      for (const auto& [id, properties] : shortcuts) {
        const auto description = properties.find("description");
        const auto trigger = properties.find("preferred_trigger");
        if (!validText(id, 256, false)
            || !ids.insert(id).second
            || description == properties.end()
            || !description->second.containsValueOfType<std::string>()
            || (trigger != properties.end() && !trigger->second.containsValueOfType<std::string>())) {
          return std::nullopt;
        }
        auto label = description->second.get<std::string>();
        auto preferred = trigger != properties.end() ? trigger->second.get<std::string>() : std::string{};
        if (!validText(label, 1024) || !validText(preferred, 256)) {
          return std::nullopt;
        }
        parsed.push_back({id, std::move(label), std::move(preferred)});
      }
      return parsed;
    }

    Shortcuts publicShortcuts(const std::vector<GlobalShortcut>& shortcuts) {
      Shortcuts result;
      result.reserve(shortcuts.size());
      for (const auto& shortcut : shortcuts) {
        result.emplace_back(
            shortcut.id,
            PortalResults{
                {"description", sdbus::Variant{shortcut.description}},
                {"trigger_description", sdbus::Variant{shortcut.triggerDescription}},
            }
        );
      }
      return result;
    }
  } // namespace

  struct GlobalShortcutsPortal::Impl {
    enum class Kind : uint8_t { Create, Bind, List };
    enum class Phase : uint8_t { Acquire, Bridge, Persist };

    struct Operation {
      Kind kind;
      uint64_t id;
      Phase phase = Phase::Acquire;
      std::unique_ptr<PortalResponse> response;
      std::unique_ptr<sdbus::IObject> request;
      std::string handle;
      std::vector<GlobalShortcutRequest> shortcuts;
      std::unique_ptr<GlobalShortcutCredentials::Transaction> transaction;
      std::optional<uint32_t> bridgeRequest;
      std::optional<GlobalShortcutResult> result;
      int deadline = 0;
      int retry = 0;
    };

    struct Session : std::enable_shared_from_this<Session> {
      Impl& portal;
      std::string path;
      std::string application;
      std::string sender;
      std::unique_ptr<sdbus::IObject> object;
      std::shared_ptr<GlobalShortcutsClient::Session> bridge;
      std::unique_ptr<Operation> pending;
      uint64_t nextOperation = 0;
      bool ready = false;
      bool bound = false;
      std::set<std::string> active;
      std::optional<std::vector<GlobalShortcut>> deferredChanges;
      bool bindAttempted = false;
      bool closed = false;

      Session(Impl& portal, std::string path, std::string application, std::string sender)
          : portal(portal), path(std::move(path)), application(std::move(application)), sender(std::move(sender)) {}

      void initialize() {
        const std::weak_ptr<Session> weak = shared_from_this();
        object = sdbus::createObject(portal.connection, sdbus::ObjectPath{path});
        object
            ->addVTable(
                sdbus::registerMethod("Close").implementedAs([weak] {
                  if (const auto session = weak.lock()) {
                    session->requireOwner();
                    session->close(false);
                  }
                }),
                sdbus::registerSignal("Closed").withParameters(),
                sdbus::registerProperty("version").withGetter([] { return uint32_t{1}; })
            )
            .forInterface(kSessionInterface);
      }

      void requireOwner() const {
        portal.requireFrontend();
        if (portal.caller() != sender) {
          throw failure("Global shortcuts session belongs to another caller");
        }
      }

      bool begin(
          PortalResponse&& response, const sdbus::ObjectPath& handle, Kind kind,
          std::vector<GlobalShortcutRequest> shortcuts = {}
      ) {
        if (pending
            || closed
            || !validPath(handle, kRequestPrefix)
            || portal.handles.contains(handle)
            || portal.handles.size() >= kRequestLimit) {
          response.returnResults(uint32_t{2}, PortalResults{});
          return false;
        }
        auto operation = std::make_unique<Operation>();
        operation->kind = kind;
        operation->id = ++nextOperation;
        operation->handle = handle;
        operation->shortcuts = std::move(shortcuts);
        operation->response = std::make_unique<PortalResponse>(std::move(response));
        operation->request = sdbus::createObject(portal.connection, handle);
        const std::weak_ptr<Session> weak = shared_from_this();
        const auto id = operation->id;
        operation->request
            ->addVTable(sdbus::registerMethod("Close").implementedAs([weak, id] {
              if (const auto session = weak.lock()) {
                session->requireOwner();
                session->abandon(id, 1);
              }
            }))
            .forInterface(kRequestInterface);
        portal.handles.insert(handle);
        pending = std::move(operation);
        pending->deadline = portal.loop.addTimer(
            kind == Kind::Bind ? portal.timeouts.approvalMs : portal.timeouts.operationMs, [weak, id] {
              if (const auto session = weak.lock(); session && session->pending && session->pending->id == id) {
                session->pending->deadline = 0;
                session->abandon(id, 2);
              }
            }
        );
        if (pending->deadline == 0) {
          fail();
          return false;
        }
        advance();
        return true;
      }

      void finish(uint32_t status, const PortalResults& results = {}) {
        auto operation = std::move(pending);
        if (!operation) {
          return;
        }
        portal.loop.removeTimer(operation->deadline);
        portal.loop.removeTimer(operation->retry);
        // Release the application lock before the frontend can issue another call.
        operation->transaction.reset();
        if (status == 0 && operation->kind == Kind::Bind) {
          bound = true;
        }
        try {
          operation->response->returnResults(status, results);
        } catch (const sdbus::Error&) {
          // A disconnected caller still needs its request and locks retired.
        }
        if (bound && deferredChanges) {
          auto pendingChanges = std::exchange(deferredChanges, std::nullopt);
          changes(std::move(*pendingChanges));
        }
        const auto handle = operation->handle;
        portal.defer([owner = &portal, handle, request = std::shared_ptr(std::move(operation->request))] {
          owner->handles.erase(handle);
        });
      }

      void close(bool backend) {
        if (std::exchange(closed, true)) {
          return;
        }
        finish(2);
        if (bridge) {
          bridge->close();
        }
        if (backend && object) {
          try {
            object->emitSignal("Closed").onInterface(kSessionInterface).withArguments();
          } catch (const sdbus::Error&) {
          }
        }
        const auto sessionPath = path;
        portal.defer([owner = &portal, sessionPath] { owner->sessions.erase(sessionPath); });
      }

      void fail() {
        finish(2);
        close(true);
      }

      void abandon(uint64_t id, uint32_t status) {
        if (!pending || pending->id != id) {
          return;
        }
        const auto kind = pending->kind;
        const auto request = pending->bridgeRequest;
        finish(status);
        if (bridge && request) {
          bridge->cancel(*request);
        }
        // An abandoned bind may already have registered shortcuts before its
        // cancellation reaches the compositor. Destroy that registration too.
        if (kind != Kind::List || status == 2) {
          close(true);
        }
      }

      void retry() {
        const std::weak_ptr<Session> weak = shared_from_this();
        const auto id = pending->id;
        pending->retry = portal.loop.addTimer(kCacheRetryMs, [weak, id] {
          if (const auto session = weak.lock(); session && session->pending && session->pending->id == id) {
            session->pending->retry = 0;
            session->advance();
          }
        });
        if (pending->retry == 0) {
          fail();
        }
      }

      void signal(const char* name, const std::string& id, uint64_t time) {
        if (closed || !bound) {
          return;
        }
        if (std::string_view(name) == "Activated") {
          if (active.contains(id)) {
            return;
          }
          if (active.size() >= kShortcutLimit) {
            close(true);
            return;
          }
          active.insert(id);
        } else if (active.erase(id) == 0) {
          return;
        }
        portal.object.emitSignal(name)
            .onInterface(kInterface)
            .withArguments(sdbus::ObjectPath{path}, id, time, PortalResults{});
      }

      void changes(std::vector<GlobalShortcut> shortcuts) {
        if (closed) {
          return;
        }
        if (bound) {
          portal.object.emitSignal("ShortcutsChanged")
              .onInterface(kInterface)
              .withArguments(sdbus::ObjectPath{path}, publicShortcuts(shortcuts));
        } else if (pending && pending->kind == Kind::Bind) {
          deferredChanges = std::move(shortcuts);
        }
      }

      bool createBridge() {
        const std::weak_ptr<Session> weak = shared_from_this();
        bridge = portal.client.createSession(
            application,
            {
                .activated =
                    [weak](const std::string& id, uint64_t time) {
                      if (const auto session = weak.lock()) {
                        session->signal("Activated", id, time);
                      }
                    },
                .deactivated =
                    [weak](const std::string& id, uint64_t time) {
                      if (const auto session = weak.lock()) {
                        session->signal("Deactivated", id, time);
                      }
                    },
                .changed =
                    [weak](std::vector<GlobalShortcut> shortcuts) {
                      if (const auto session = weak.lock()) {
                        session->changes(std::move(shortcuts));
                      }
                    },
                .closed =
                    [weak] {
                      if (const auto session = weak.lock()) {
                        session->close(true);
                      }
                    },
            }
        );
        return bridge != nullptr;
      }

      void complete(uint64_t id, GlobalShortcutResult result) {
        if (!pending || pending->id != id || closed) {
          return;
        }
        pending->bridgeRequest.reset();
        if (result.status != BridgeStatus::Success) {
          if (pending->kind != Kind::Create || result.status != BridgeStatus::Failed || bridge->closed()) {
            finish(static_cast<uint32_t>(result.status));
            if (bridge->closed()) {
              close(true);
            }
            return;
          }
          // A rejected cached credential grants no approval. Remove it before
          // allowing the session to request fresh consent.
        } else if (pending->kind != Kind::Create && result.credential.empty()) {
          finish(0, {{"shortcuts", sdbus::Variant{publicShortcuts(result.shortcuts)}}});
          return;
        }
        pending->result = std::move(result);
        pending->phase = Phase::Persist;
        advance();
      }

      void advance() {
        if (closed || !pending) {
          return;
        }
        if (pending->phase == Phase::Persist) {
          const auto& result = *pending->result;
          const auto status = result.status == BridgeStatus::Success ? pending->transaction->save(result.credential)
                                                                     : pending->transaction->clear();
          if (status == CacheStatus::Busy) {
            retry();
          } else if (status != CacheStatus::Ready) {
            fail();
          } else if (pending->kind == Kind::Create) {
            ready = true;
            finish(0);
          } else {
            finish(0, {{"shortcuts", sdbus::Variant{publicShortcuts(result.shortcuts)}}});
          }
          return;
        }
        if (pending->phase != Phase::Acquire) {
          return;
        }
        if (pending->kind != Kind::List) {
          auto acquired = portal.credentials.acquire(application);
          if (acquired.status == CacheStatus::Busy) {
            retry();
            return;
          }
          if (acquired.status != CacheStatus::Ready) {
            fail();
            return;
          }
          pending->transaction = std::move(acquired.transaction);
        }
        if (pending->kind == Kind::Create) {
          if (!createBridge()) {
            fail();
            return;
          }
          if (!pending->transaction->credential()) {
            ready = true;
            finish(0);
            return;
          }
        }
        pending->phase = Phase::Bridge;
        const auto id = pending->id;
        const std::weak_ptr<Session> weak = shared_from_this();
        auto completion = [weak, id](GlobalShortcutResult result) {
          if (const auto session = weak.lock()) {
            session->complete(id, std::move(result));
          }
        };
        switch (pending->kind) {
        case Kind::Create:
          pending->bridgeRequest = bridge->restore(*pending->transaction->credential(), std::move(completion));
          break;
        case Kind::Bind:
          pending->bridgeRequest = bridge->bind(pending->shortcuts, std::move(completion));
          break;
        case Kind::List:
          pending->bridgeRequest = bridge->list(std::move(completion));
          break;
        }
        if (!pending->bridgeRequest) {
          fail();
        }
      }
    };

    Loop& loop;
    sdbus::IConnection& connection;
    sdbus::IObject& object;
    GlobalShortcutCredentials credentials;
    GlobalShortcutsClient client;
    GlobalShortcutTimeouts timeouts;
    Frontend frontend;
    sdbus::Slot registration;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::set<std::string> handles;
    std::vector<std::function<void()>> deferred;
    int deferredTimer = 0;

    Impl(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, WaylandContext& wayland,
        GlobalShortcutCredentials credentials, GlobalShortcutTimeouts timeouts
    )
        : loop(loop), connection(connection), object(object), credentials(std::move(credentials)),
          client(loop, wayland, timeouts), timeouts(timeouts),
          frontend(connection, [this](const std::string&, const std::string&, bool changed) {
            if (changed) {
              closeAll();
            }
          }) {
      registerInterface();
    }

    ~Impl() {
      registration.reset();
      closeAll();
      loop.removeTimer(deferredTimer);
      deferred.clear();
      sessions.clear();
    }

    void defer(std::function<void()> callback) {
      deferred.push_back(std::move(callback));
      if (deferredTimer == 0) {
        deferredTimer = loop.addTimer(0, [this] {
          deferredTimer = 0;
          auto callbacks = std::exchange(deferred, {});
          for (const auto& callback : callbacks) {
            callback();
          }
        });
      }
    }

    void closeAll() {
      for (const auto& [path, session] : sessions) {
        (void)path;
        session->close(true);
      }
    }

    std::string caller() const { return frontend.caller(); }

    void requireFrontend() const { frontend.require("global shortcuts"); }

    std::shared_ptr<Session> lookup(const sdbus::ObjectPath& path) const {
      requireFrontend();
      const auto found = sessions.find(path);
      if (found == sessions.end()
          || found->second->closed
          || !found->second->ready
          || found->second->sender != caller()) {
        throw failure("Invalid global shortcuts session or state");
      }
      return found->second;
    }

    void registerInterface() {
      registration =
          object
              .addVTable(
                  sdbus::registerMethod("CreateSession")
                      .implementedAs([this](
                                         PortalResponse&& response, const sdbus::ObjectPath& handle,
                                         const sdbus::ObjectPath& path, const std::string& application,
                                         const PortalResults&
                                     ) {
                        requireFrontend();
                        if (!client.available()) {
                          throw failure("The compositor does not support global shortcuts");
                        }
                        if (!validText(application, 256, false)
                            || !validPath(path, kSessionPrefix)
                            || sessions.contains(path)
                            || sessions.size() >= kSessionLimit) {
                          response.returnResults(uint32_t{2}, PortalResults{});
                          return;
                        }
                        auto session = std::make_shared<Session>(*this, path, application, caller());
                        sessions.emplace(path, session);
                        try {
                          session->initialize();
                          if (!session->begin(std::move(response), handle, Kind::Create)) {
                            session->close(false);
                          }
                        } catch (...) {
                          session->close(false);
                          throw;
                        }
                      })
                      .withInputParamNames("handle", "session_handle", "app_id", "options")
                      .withOutputParamNames("response", "results"),
                  sdbus::registerMethod("BindShortcuts")
                      .implementedAs([this](
                                         PortalResponse&& response, const sdbus::ObjectPath& handle,
                                         const sdbus::ObjectPath& path, const Shortcuts& shortcuts, const std::string&,
                                         const PortalResults&
                                     ) {
                        const auto session = lookup(path);
                        if (session->pending || std::exchange(session->bindAttempted, true)) {
                          response.returnResults(uint32_t{2}, PortalResults{});
                          return;
                        }
                        auto parsed = parseShortcuts(shortcuts);
                        if (!parsed) {
                          response.returnResults(uint32_t{2}, PortalResults{});
                          return;
                        }
                        (void)session->begin(std::move(response), handle, Kind::Bind, std::move(*parsed));
                      })
                      .withInputParamNames("handle", "session_handle", "shortcuts", "parent_window", "options")
                      .withOutputParamNames("response", "results"),
                  sdbus::registerMethod("ListShortcuts")
                      .implementedAs([this](
                                         PortalResponse&& response, const sdbus::ObjectPath& handle,
                                         const sdbus::ObjectPath& path
                                     ) {
                        const auto session = lookup(path);
                        (void)session->begin(std::move(response), handle, Kind::List);
                      })
                      .withInputParamNames("handle", "session_handle")
                      .withOutputParamNames("response", "results"),
                  sdbus::registerSignal("Activated")
                      .withParameters<sdbus::ObjectPath, std::string, uint64_t, PortalResults>(),
                  sdbus::registerSignal("Deactivated")
                      .withParameters<sdbus::ObjectPath, std::string, uint64_t, PortalResults>(),
                  sdbus::registerSignal("ShortcutsChanged").withParameters<sdbus::ObjectPath, Shortcuts>(),
                  sdbus::registerProperty("version").withGetter([] { return uint32_t{1}; })
              )
              .forInterface(kInterface, sdbus::return_slot);
    }
  };

  GlobalShortcutsPortal::GlobalShortcutsPortal(
      Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, WaylandContext& wayland,
      GlobalShortcutCredentials credentials, GlobalShortcutTimeouts timeouts
  )
      : m_impl(std::make_unique<Impl>(loop, connection, object, wayland, std::move(credentials), timeouts)) {}
  GlobalShortcutsPortal::~GlobalShortcutsPortal() = default;
} // namespace xdpu
