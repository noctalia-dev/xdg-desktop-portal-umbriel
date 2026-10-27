#include "dbus/input_capture.h"

#include "dbus/request.h"
#include "loop/loop.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <umbriel-input-capture-v1-client-protocol.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>

namespace xdpu {

  namespace {
    constexpr char kInterface[] = "org.freedesktop.impl.portal.InputCapture";
    constexpr char kSessionInterface[] = "org.freedesktop.impl.portal.Session";
    constexpr char kFrontend[] = "org.freedesktop.portal.Desktop";
    constexpr uint32_t kCapabilities = 3;
    constexpr char kRestoreVendor[] = "umbriel-input-capture";
    constexpr uint32_t kRestoreVersion = 1;
    constexpr size_t kRestoreTokenLimit = 4096;
    constexpr size_t kSessionLimit = 64;
    constexpr size_t kBarrierLimit = 1024;
    using Zone = sdbus::Struct<uint32_t, uint32_t, int32_t, int32_t>;
    using Position = sdbus::Struct<int32_t, int32_t, int32_t, int32_t>;
    using CursorPosition = sdbus::Struct<double, double>;
    using RestoreData = sdbus::Struct<std::string, uint32_t, sdbus::Variant>;
    using FdResponse = sdbus::Result<sdbus::UnixFd>;

    template <typename T> std::optional<T> option(const PortalResults& options, const char* key) {
      const auto found = options.find(key);
      if (found == options.end() || !found->second.containsValueOfType<T>()) {
        return std::nullopt;
      }
      return found->second.get<T>();
    }

    sdbus::Error failure(const char* message) {
      return sdbus::Error{sdbus::Error::Name{"org.freedesktop.portal.Error.Failed"}, message};
    }

    std::string applicationInstance(std::string_view path) {
      constexpr std::string_view prefix = "/org/freedesktop/portal/desktop/session/";
      if (!path.starts_with(prefix)) {
        return {};
      }
      path.remove_prefix(prefix.size());
      const size_t separator = path.find('/');
      if (separator == std::string_view::npos || separator == 0 || separator > 255) {
        return {};
      }
      const std::string_view component = path.substr(0, separator);
      bool needDigit = true;
      bool hasSeparator = false;
      for (const char character : component) {
        if (character >= '0' && character <= '9') {
          needDigit = false;
        } else if (character == '_' && !needDigit) {
          hasSeparator = true;
          needDigit = true;
        } else {
          return {};
        }
      }
      if (needDigit || !hasSeparator) {
        return {};
      }
      std::string sender = ":" + std::string(component);
      std::ranges::replace(sender, '_', '.');
      return sender;
    }
  } // namespace

  struct InputCapturePortal::Impl {
    enum class OperationKind { Start, Zones, Barriers, Connect, Other };

    struct Operation {
      OperationKind kind = OperationKind::Other;
      std::unique_ptr<PortalResponse> response;
      std::unique_ptr<FdResponse> fdResponse;
      std::shared_ptr<sdbus::IObject> request;
      std::vector<Zone> zones;
      std::vector<uint32_t> failedBarriers;
      sdbus::UnixFd fd;
      uint32_t requestedPersistMode = 0;
      uint32_t grantedPersistMode = 0;
      std::string restoreToken;
      int timer = 0;
    };

    struct Session : std::enable_shared_from_this<Session> {
      Impl& portal;
      std::string path;
      std::string appId;
      std::string sender;
      std::string instance;
      std::unique_ptr<sdbus::IObject> object;
      umbriel_input_capture_session_v1* capture = nullptr;
      std::map<uint32_t, std::unique_ptr<Operation>> operations;
      uint32_t nextId = 0;
      bool starting = false;
      bool started = false;
      bool connected = false;
      bool closed = false;

      Session(Impl& portal, std::string path, std::string appId, std::string sender)
          : portal(portal), path(std::move(path)), appId(std::move(appId)), sender(std::move(sender)),
            instance(applicationInstance(this->path)) {}

      ~Session() {
        if (capture != nullptr) {
          umbriel_input_capture_session_v1_destroy(capture);
        }
        for (auto& [id, operation] : operations) {
          (void)id;
          if (operation->timer != 0) {
            portal.loop.removeTimer(operation->timer);
          }
        }
      }

      void initialize() {
        object = sdbus::createObject(portal.connection, sdbus::ObjectPath{path});
        object
            ->addVTable(
                sdbus::registerMethod("Close").implementedAs([this]() {
                  portal.requireFrontend();
                  if (portal.caller() != sender) {
                    throw failure("Input capture session belongs to another caller");
                  }
                  close(false);
                }),
                sdbus::registerSignal("Closed").withParameters(),
                sdbus::registerProperty("version").withGetter([]() { return uint32_t{1}; })
            )
            .forInterface(kSessionInterface);
        capture = umbriel_input_capture_manager_v1_create_session(portal.manager);
        umbriel_input_capture_session_v1_add_listener(capture, &kListener, this);
        portal.wayland.flush();
      }

      uint32_t add(std::unique_ptr<Operation> operation, const std::string& handle = {}) {
        do {
          ++nextId;
        } while (nextId == 0 || operations.contains(nextId));
        const uint32_t id = nextId;
        const std::weak_ptr<Session> weak = shared_from_this();
        if (!handle.empty()) {
          operation->request = sdbus::createObject(portal.connection, sdbus::ObjectPath{handle});
          operation->request
              ->addVTable(sdbus::registerMethod("Close").implementedAs([weak, id]() {
                if (const auto session = weak.lock()) {
                  session->portal.requireFrontend();
                  const auto found = session->operations.find(id);
                  if (found == session->operations.end()) {
                    return;
                  }
                  const bool start = found->second->kind == OperationKind::Start;
                  session->finish(id, 1);
                  if (start) {
                    session->close(true);
                  }
                }
              }))
              .forInterface("org.freedesktop.impl.portal.Request");
        }
        operation->timer = portal.loop.addTimer(operation->kind == OperationKind::Start ? 120000 : 10000, [weak, id]() {
          if (const auto session = weak.lock()) {
            const auto found = session->operations.find(id);
            if (found != session->operations.end()) {
              found->second->timer = 0;
              session->finish(id, 2);
              session->close(true);
            }
          }
        });
        operations.emplace(id, std::move(operation));
        return id;
      }

      uint32_t addResponse(PortalResponse&& response, OperationKind kind, const std::string& handle = {}) {
        auto operation = std::make_unique<Operation>();
        operation->kind = kind;
        operation->response = std::make_unique<PortalResponse>(std::move(response));
        return add(std::move(operation), handle);
      }

      void finish(uint32_t id, uint32_t status, PortalResults results = {}) {
        const auto found = operations.find(id);
        if (found == operations.end()) {
          return;
        }
        auto operation = std::move(found->second);
        operations.erase(found);
        if (operation->timer != 0) {
          portal.loop.removeTimer(operation->timer);
        }
        if (operation->response) {
          operation->response->returnResults(status, results);
        } else if (operation->fdResponse) {
          if (status == 0 && operation->fd.get() >= 0) {
            connected = true;
            operation->fdResponse->returnResults(operation->fd);
          } else {
            operation->fdResponse->returnError(failure("Unable to connect the input capture EIS transport"));
          }
        }
        // The request Close callback may currently be executing. Keep its
        // D-Bus object alive until dispatch returns.
        if (operation->request) {
          portal.defer([request = std::move(operation->request)]() {});
        }
      }

      void close(bool backend) {
        if (std::exchange(closed, true)) {
          return;
        }
        if (capture != nullptr) {
          umbriel_input_capture_session_v1_destroy(capture);
          capture = nullptr;
          portal.wayland.flush();
        }
        while (!operations.empty()) {
          finish(operations.begin()->first, 2);
        }
        if (backend && object) {
          object->emitSignal("Closed").onInterface(kSessionInterface).withArguments();
        }
        const std::string sessionPath = path;
        portal.defer([owner = &portal, sessionPath]() { owner->sessions.erase(sessionPath); });
      }

      void signal(const char* name, PortalResults options) {
        if (!closed) {
          portal.object.emitSignal(name).onInterface(kInterface).withArguments(sdbus::ObjectPath{path}, options);
        }
      }

      static void
      onStarted(void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t status, uint32_t capabilities) {
        auto& session = *static_cast<Session*>(data);
        session.starting = false;
        session.started = status == 0;
        PortalResults results;
        if (status == 0) {
          results.emplace("capabilities", sdbus::Variant{capabilities & kCapabilities});
          results.emplace("clipboard_enabled", sdbus::Variant{false});
          const auto found = session.operations.find(id);
          if (found != session.operations.end()
              && found->second->kind == OperationKind::Start
              && found->second->grantedPersistMode != 0
              && !found->second->restoreToken.empty()) {
            results.emplace("persist_mode", sdbus::Variant{found->second->grantedPersistMode});
            results.emplace(
                "restore_data",
                sdbus::Variant{
                    RestoreData{kRestoreVendor, kRestoreVersion, sdbus::Variant{found->second->restoreToken}}
                }
            );
          }
        }
        session.finish(id, status, std::move(results));
        if (status != 0) {
          session.close(true);
        }
      }

      static void onZone(
          void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t width, uint32_t height, int32_t x,
          int32_t y
      ) {
        auto& session = *static_cast<Session*>(data);
        const auto found = session.operations.find(id);
        if (found != session.operations.end() && found->second->kind == OperationKind::Zones) {
          found->second->zones.emplace_back(width, height, x, y);
        }
      }

      static void
      onZonesDone(void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t status, uint32_t zoneSet) {
        auto& session = *static_cast<Session*>(data);
        const auto found = session.operations.find(id);
        if (found == session.operations.end()) {
          return;
        }
        PortalResults results;
        if (status == 0) {
          results.emplace("zones", sdbus::Variant{found->second->zones});
          results.emplace("zone_set", sdbus::Variant{zoneSet});
        }
        session.finish(id, status, std::move(results));
      }

      static void onBarrierFailed(void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t barrier) {
        auto& session = *static_cast<Session*>(data);
        const auto found = session.operations.find(id);
        if (found != session.operations.end() && found->second->kind == OperationKind::Barriers) {
          found->second->failedBarriers.push_back(barrier);
        }
      }

      static void onBarriersDone(void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t status) {
        auto& session = *static_cast<Session*>(data);
        const auto found = session.operations.find(id);
        if (found == session.operations.end()) {
          return;
        }
        PortalResults results;
        results.emplace("failed_barriers", sdbus::Variant{found->second->failedBarriers});
        session.finish(id, status, std::move(results));
      }

      static void onEisFd(void* data, umbriel_input_capture_session_v1*, uint32_t id, int32_t fd) {
        auto& session = *static_cast<Session*>(data);
        const auto found = session.operations.find(id);
        if (found != session.operations.end() && found->second->kind == OperationKind::Connect) {
          found->second->fd.reset(fd, sdbus::adopt_fd);
        } else {
          ::close(fd);
        }
      }

      static void onResult(void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t status) {
        static_cast<Session*>(data)->finish(id, status);
      }

      static void onActivated(
          void* data, umbriel_input_capture_session_v1*, uint32_t activationId, wl_fixed_t x, wl_fixed_t y,
          uint32_t barrierId
      ) {
        static_cast<Session*>(data)->signal(
            "Activated",
            {
                {"activation_id", sdbus::Variant{activationId}},
                {"cursor_position", sdbus::Variant{CursorPosition{wl_fixed_to_double(x), wl_fixed_to_double(y)}}},
                {"barrier_id", sdbus::Variant{barrierId}},
            }
        );
      }

      static void onDeactivated(void* data, umbriel_input_capture_session_v1*, uint32_t activationId) {
        static_cast<Session*>(data)->signal("Deactivated", {{"activation_id", sdbus::Variant{activationId}}});
      }
      static void onDisabled(void* data, umbriel_input_capture_session_v1*) {
        static_cast<Session*>(data)->signal("Disabled", {});
      }
      static void onZonesChanged(void* data, umbriel_input_capture_session_v1*, uint32_t zoneSet) {
        static_cast<Session*>(data)->signal("ZonesChanged", {{"zone_set", sdbus::Variant{zoneSet}}});
      }
      static void onClosed(void* data, umbriel_input_capture_session_v1*) { static_cast<Session*>(data)->close(true); }

      static void
      onGrant(void* data, umbriel_input_capture_session_v1*, uint32_t id, uint32_t persistMode, const char* token) {
        auto& session = *static_cast<Session*>(data);
        const auto found = session.operations.find(id);
        if (found == session.operations.end()
            || found->second->kind != OperationKind::Start
            || persistMode == 0
            || persistMode > found->second->requestedPersistMode
            || (persistMode == 1 && session.instance.empty())
            || token == nullptr
            || *token == '\0'
            || std::string_view{token}.size() > kRestoreTokenLimit) {
          return;
        }
        found->second->grantedPersistMode = persistMode;
        found->second->restoreToken = token;
        if (persistMode == 1) {
          session.portal.transientScopes.insert(session.instance);
        }
      }

      static constexpr umbriel_input_capture_session_v1_listener kListener{
          .started = onStarted,
          .zone = onZone,
          .zones_done = onZonesDone,
          .barrier_failed = onBarrierFailed,
          .barriers_done = onBarriersDone,
          .eis_fd = onEisFd,
          .result = onResult,
          .activated = onActivated,
          .deactivated = onDeactivated,
          .disabled = onDisabled,
          .zones_changed = onZonesChanged,
          .closed = onClosed,
          .grant = onGrant,
      };
    };

    Loop& loop;
    sdbus::IConnection& connection;
    sdbus::IObject& object;
    WaylandContext& wayland;
    std::unique_ptr<sdbus::IProxy> bus;
    std::string frontendOwner;
    wl_registry* registry = nullptr;
    umbriel_input_capture_manager_v1* manager = nullptr;
    uint32_t managerName = 0;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::vector<int> deferred;
    std::set<std::string> transientScopes;

    Impl(Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, WaylandContext& wayland)
        : loop(loop), connection(connection), object(object), wayland(wayland) {
      registry = wl_display_get_registry(wayland.display());
      static constexpr wl_registry_listener listener{.global = onGlobal, .global_remove = onGlobalRemove};
      wl_registry_add_listener(registry, &listener, this);
      wayland.roundtrip();
      bus = sdbus::createProxy(
          connection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
      );
      bus->uponSignal("NameOwnerChanged")
          .onInterface("org.freedesktop.DBus")
          .call([this](const std::string& name, const std::string&, const std::string& owner) {
            if (name == kFrontend && owner != frontendOwner) {
              frontendOwner = owner;
              closeAll();
              expireScopes();
            } else if (owner.empty()) {
              expireScope(name);
            }
          });
      try {
        bus->callMethod("GetNameOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string{kFrontend})
            .storeResultsTo(frontendOwner);
      } catch (const sdbus::Error&) {
        frontendOwner.clear();
      }
      registerInterface();
    }

    ~Impl() {
      closeAll();
      expireScopes();
      for (const int timer : deferred) {
        loop.removeTimer(timer);
      }
      deferred.clear();
      sessions.clear();
      if (manager != nullptr) {
        umbriel_input_capture_manager_v1_destroy(manager);
      }
      if (registry != nullptr) {
        wl_registry_destroy(registry);
      }
      wayland.flush();
    }

    void defer(std::function<void()> callback) {
      const auto id = std::make_shared<int>(0);
      *id = loop.addTimer(0, [this, id, callback = std::move(callback)]() {
        std::erase(deferred, *id);
        callback();
      });
      if (*id != 0) {
        deferred.push_back(*id);
      }
    }

    std::string caller() const {
      const auto message = connection.getCurrentlyProcessedMessage();
      const char* sender = message.getSender();
      return sender != nullptr ? sender : "";
    }

    void requireFrontend() const {
      if (frontendOwner.empty() || caller() != frontendOwner) {
        throw sdbus::Error{
            sdbus::Error::Name{"org.freedesktop.portal.Error.NotAllowed"},
            "Only the desktop portal frontend may control input capture"
        };
      }
    }

    std::shared_ptr<Session>
    lookup(const sdbus::ObjectPath& path, const std::string& appId, bool requireStarted = true) {
      requireFrontend();
      const auto found = sessions.find(path);
      if (found == sessions.end()
          || found->second->closed
          || found->second->appId != appId
          || found->second->sender != caller()
          || (requireStarted && !found->second->started)) {
        throw failure("Invalid input capture session or state");
      }
      return found->second;
    }

    std::shared_ptr<Session> create(const sdbus::ObjectPath& path, const std::string& appId) {
      requireFrontend();
      if (manager == nullptr || !wayland.connected()) {
        throw failure("The compositor does not support input capture");
      }
      if (sessions.size() >= kSessionLimit
          || sessions.contains(path)
          || appId.size() > 256
          || !std::string(path).starts_with("/org/freedesktop/portal/desktop/session/")) {
        throw failure("Invalid input capture session");
      }
      auto session = std::make_shared<Session>(*this, std::string(path), appId, caller());
      sessions.emplace(session->path, session);
      try {
        session->initialize();
      } catch (...) {
        sessions.erase(session->path);
        throw;
      }
      return session;
    }

    void closeAll() {
      // close() only schedules erasure, so callbacks cannot invalidate this walk.
      for (const auto& [path, session] : sessions) {
        (void)path;
        session->close(true);
      }
    }

    void expireScope(const std::string& scope) {
      bool known = transientScopes.erase(scope) != 0;
      for (const auto& [path, session] : sessions) {
        (void)path;
        if (session->instance == scope) {
          known = true;
          session->close(true);
        }
      }
      if (!known || manager == nullptr) {
        return;
      }
      umbriel_input_capture_manager_v1_expire_scope(manager, scope.c_str());
      wayland.flush();
    }

    void expireScopes() {
      auto scopes = std::exchange(transientScopes, {});
      for (const auto& [path, session] : sessions) {
        (void)path;
        if (!session->instance.empty()) {
          scopes.insert(session->instance);
        }
      }
      if (manager != nullptr) {
        for (const auto& scope : scopes) {
          umbriel_input_capture_manager_v1_expire_scope(manager, scope.c_str());
        }
        wayland.flush();
      }
    }

    void start(
        PortalResponse&& result, const sdbus::ObjectPath& handle, const std::shared_ptr<Session>& session,
        const PortalResults& options
    ) {
      const uint32_t requested = option<uint32_t>(options, "capabilities").value_or(0);
      const uint32_t capabilities = requested & kCapabilities;
      const auto requestedPersistMode = option<uint32_t>(options, "persist_mode");
      if (session->starting
          || session->started
          || capabilities == 0
          || (options.contains("persist_mode") && !requestedPersistMode)
          || requestedPersistMode.value_or(0) > 2) {
        result.returnResults(uint32_t{2}, PortalResults{});
        return;
      }
      session->starting = true;
      const uint32_t id = session->addResponse(std::move(result), OperationKind::Start, std::string(handle));
      const bool unidentifiedInstance = requestedPersistMode.value_or(0) == 1 && session->instance.empty();
      const uint32_t persistMode = unidentifiedInstance ? 0 : requestedPersistMode.value_or(0);
      session->operations.at(id)->requestedPersistMode = persistMode;
      std::string restoreToken;
      if (const auto restore = option<RestoreData>(options, "restore_data"); restore
          && restore->get<0>() == kRestoreVendor
          && restore->get<1>() == kRestoreVersion
          && restore->get<2>().containsValueOfType<std::string>()) {
        restoreToken = restore->get<2>().get<std::string>();
        if (restoreToken.size() > kRestoreTokenLimit) {
          restoreToken.clear();
        }
      }
      if (unidentifiedInstance) {
        restoreToken.clear();
      }
      umbriel_input_capture_session_v1_set_persistence(
          session->capture, persistMode, restoreToken.c_str(), session->instance.c_str()
      );
      umbriel_input_capture_session_v1_start(session->capture, id, session->appId.c_str(), capabilities);
      wayland.flush();
    }

    static void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      auto& portal = *static_cast<Impl*>(data);
      if (std::string_view(interface) == umbriel_input_capture_manager_v1_interface.name && portal.manager == nullptr) {
        if (version < 2) {
          std::println(stderr, "input-capture: compositor bridge version 2 is required");
          return;
        }
        portal.manager = static_cast<umbriel_input_capture_manager_v1*>(
            wl_registry_bind(registry, name, &umbriel_input_capture_manager_v1_interface, 2)
        );
        portal.managerName = name;
      }
    }

    static void onGlobalRemove(void* data, wl_registry*, uint32_t name) {
      auto& portal = *static_cast<Impl*>(data);
      if (name == portal.managerName) {
        portal.closeAll();
        portal.expireScopes();
        umbriel_input_capture_manager_v1_destroy(portal.manager);
        portal.manager = nullptr;
        portal.managerName = 0;
      }
    }

    void registerInterface() {
      object
          .addVTable(
              sdbus::registerMethod("CreateSession2")
                  .implementedAs([this](const sdbus::ObjectPath& path, const std::string& appId, const PortalResults&) {
                    create(path, appId);
                    return PortalResults{};
                  })
                  .withInputParamNames("session_handle", "app_id", "options")
                  .withOutputParamNames("results"),
              sdbus::registerMethod("CreateSession")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& path, const std::string& appId, const std::string&,
                                     const PortalResults& options
                                 ) { start(std::move(response), handle, create(path, appId), options); })
                  .withInputParamNames("handle", "session_handle", "app_id", "parent_window", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("Start")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& path, const std::string& appId, const std::string&,
                                     const PortalResults& options
                                 ) { start(std::move(response), handle, lookup(path, appId, false), options); })
                  .withInputParamNames("handle", "session_handle", "app_id", "parent_window", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("GetZones")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& path, const std::string& appId, const PortalResults&
                                 ) {
                    const auto session = lookup(path, appId);
                    const uint32_t id =
                        session->addResponse(std::move(response), OperationKind::Zones, std::string(handle));
                    umbriel_input_capture_session_v1_get_zones(session->capture, id);
                    wayland.flush();
                  })
                  .withInputParamNames("handle", "session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("SetPointerBarriers")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& path, const std::string& appId, const PortalResults&,
                                     const std::vector<PortalResults>& barriers, uint32_t zoneSet
                                 ) {
                    const auto session = lookup(path, appId);
                    if (barriers.size() > kBarrierLimit) {
                      response.returnResults(uint32_t{2}, PortalResults{});
                      return;
                    }
                    struct Barrier {
                      uint32_t id;
                      Position position;
                    };
                    std::vector<Barrier> validated;
                    for (const auto& barrier : barriers) {
                      const auto id = option<uint32_t>(barrier, "barrier_id");
                      const auto position = option<Position>(barrier, "position");
                      if (!id || !position || *id == 0) {
                        response.returnResults(uint32_t{2}, PortalResults{});
                        return;
                      }
                      validated.push_back({*id, *position});
                    }
                    const uint32_t id =
                        session->addResponse(std::move(response), OperationKind::Barriers, std::string(handle));
                    umbriel_input_capture_session_v1_clear_barriers(session->capture);
                    for (const auto& barrier : validated) {
                      umbriel_input_capture_session_v1_add_barrier(
                          session->capture, barrier.id, std::get<0>(barrier.position), std::get<1>(barrier.position),
                          std::get<2>(barrier.position), std::get<3>(barrier.position)
                      );
                    }
                    umbriel_input_capture_session_v1_commit_barriers(session->capture, id, zoneSet);
                    wayland.flush();
                  })
                  .withInputParamNames("handle", "session_handle", "app_id", "options", "barriers", "zone_set")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("Enable")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& path, const std::string& appId,
                                     const PortalResults&
                                 ) {
                    const auto session = lookup(path, appId);
                    const uint32_t id = session->addResponse(std::move(response), OperationKind::Other);
                    umbriel_input_capture_session_v1_enable(session->capture, id);
                    wayland.flush();
                  })
                  .withInputParamNames("session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("Disable")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& path, const std::string& appId,
                                     const PortalResults&
                                 ) {
                    const auto session = lookup(path, appId);
                    const uint32_t id = session->addResponse(std::move(response), OperationKind::Other);
                    umbriel_input_capture_session_v1_disable(session->capture, id);
                    wayland.flush();
                  })
                  .withInputParamNames("session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("Release")
                  .implementedAs([this](
                                     PortalResponse&& response, const sdbus::ObjectPath& path, const std::string& appId,
                                     const PortalResults& options
                                 ) {
                    const auto session = lookup(path, appId);
                    const auto activation = option<uint32_t>(options, "activation_id");
                    const auto position = option<CursorPosition>(options, "cursor_position");
                    double x = 0;
                    double y = 0;
                    if (position) {
                      x = std::get<0>(*position);
                      y = std::get<1>(*position);
                      if (!std::isfinite(x)
                          || !std::isfinite(y)
                          || x < -8388608.0
                          || x >= 8388608.0
                          || y < -8388608.0
                          || y >= 8388608.0) {
                        response.returnResults(uint32_t{2}, PortalResults{});
                        return;
                      }
                    }
                    if (!activation) {
                      response.returnResults(uint32_t{2}, PortalResults{});
                      return;
                    }
                    const uint32_t id = session->addResponse(std::move(response), OperationKind::Other);
                    umbriel_input_capture_session_v1_release(
                        session->capture, id, *activation, position.has_value(), wl_fixed_from_double(x),
                        wl_fixed_from_double(y)
                    );
                    wayland.flush();
                  })
                  .withInputParamNames("session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("ConnectToEIS")
                  .implementedAs([this](
                                     FdResponse&& response, const sdbus::ObjectPath& path, const std::string& appId,
                                     const PortalResults&
                                 ) {
                    const auto session = lookup(path, appId);
                    if (session->connected || std::ranges::any_of(session->operations, [](const auto& entry) {
                          return entry.second->kind == OperationKind::Connect;
                        })) {
                      response.returnError(failure(
                          "The input capture session already has an "
                          "EIS connection"
                      ));
                      return;
                    }
                    auto operation = std::make_unique<Operation>();
                    operation->kind = OperationKind::Connect;
                    operation->fdResponse = std::make_unique<FdResponse>(std::move(response));
                    const uint32_t id = session->add(std::move(operation));
                    umbriel_input_capture_session_v1_connect_eis(session->capture, id);
                    wayland.flush();
                  })
                  .withInputParamNames("session_handle", "app_id", "options")
                  .withOutputParamNames("fd"),
              sdbus::registerSignal("Activated").withParameters<sdbus::ObjectPath, PortalResults>(),
              sdbus::registerSignal("Deactivated").withParameters<sdbus::ObjectPath, PortalResults>(),
              sdbus::registerSignal("Disabled").withParameters<sdbus::ObjectPath, PortalResults>(),
              sdbus::registerSignal("ZonesChanged").withParameters<sdbus::ObjectPath, PortalResults>(),
              sdbus::registerProperty("SupportedCapabilities").withGetter([]() { return kCapabilities; }),
              sdbus::registerProperty("version").withGetter([]() { return uint32_t{2}; })
          )
          .forInterface(kInterface);
    }
  };

  InputCapturePortal::InputCapturePortal(
      Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, WaylandContext& wayland
  )
      : m_impl(std::make_unique<Impl>(loop, connection, object, wayland)) {}

  InputCapturePortal::~InputCapturePortal() = default;

} // namespace xdpu
