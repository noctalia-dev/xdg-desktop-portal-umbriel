#include "dbus/global_shortcuts.h"
#include "config/config.h"
#include "dbus/request.h"
#include "dbus/screenshot.h"
#include "global_shortcuts_fixture.h"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <source_location>

namespace {
  using shortcut_test::Fixture;
  using shortcut_test::Json;
  using shortcut_test::require;
  using Dict = xdpu::PortalResults;
  using Shortcuts = std::vector<sdbus::Struct<std::string, Dict>>;
  using Cache = xdpu::GlobalShortcutCredentials;
  constexpr auto kInterface = "org.freedesktop.impl.portal.GlobalShortcuts";
  constexpr auto kBackend = "org.freedesktop.impl.portal.desktop.umbriel";
  constexpr auto kFrontend = "org.freedesktop.portal.Desktop";
  constexpr auto kDesktop = "/org/freedesktop/portal/desktop";
  constexpr auto kRequestInterface = "org.freedesktop.impl.portal.Request";
  constexpr auto kSessionInterface = "org.freedesktop.impl.portal.Session";
  bool failRename = false;
  bool renameFailed = false;

  struct Temporary {
    std::filesystem::path root;
    std::filesystem::path path;
    Temporary() {
      std::array<char, 48> name{};
      std::string_view("/tmp/xdpu-shortcut-portal-XXXXXX").copy(name.data(), name.size());
      const auto* created = mkdtemp(name.data());
      require(created != nullptr, "temporary state directory failed");
      root = created;
      path = root / "state/credentials.json";
    }
    ~Temporary() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
    Temporary(const Temporary&) = delete;
    Temporary& operator=(const Temporary&) = delete;
  };

  struct Reply {
    bool done = false;
    std::string error;
    uint32_t status = 99;
    Dict results;
    std::string handle;
  };

  struct BusFixture {
    Temporary files;
    Fixture transport;
    std::unique_ptr<sdbus::IConnection> frontend;
    std::unique_ptr<sdbus::IConnection> backend;
    std::unique_ptr<sdbus::IConnection> other;
    std::unique_ptr<sdbus::IObject> desktop;
    std::unique_ptr<xdpu::GlobalShortcutsPortal> portal;
    std::unique_ptr<sdbus::IProxy> proxy;
    std::unique_ptr<sdbus::IProxy> otherProxy;
    std::vector<std::unique_ptr<sdbus::IProxy>> objects;
    std::vector<std::string> closed;
    std::set<std::string> watched;
    std::vector<uint64_t> activated;
    std::vector<uint64_t> deactivated;
    std::vector<Shortcuts> changed;
    uint32_t nextHandle = 0;

    explicit BusFixture(bool advertise = true, xdpu::GlobalShortcutTimeouts timeouts = {5000, 10000})
        : transport(advertise) {
      frontend = sdbus::createSessionBusConnection(sdbus::ServiceName{kFrontend});
      backend = sdbus::createSessionBusConnection(sdbus::ServiceName{kBackend});
      other = sdbus::createSessionBusConnection();
      desktop = sdbus::createObject(*backend, sdbus::ObjectPath{kDesktop});
      portal = std::make_unique<xdpu::GlobalShortcutsPortal>(
          *transport.loop, *backend, *desktop, *transport.wayland, Cache(files.path), timeouts
      );
      proxy = makeProxy(*frontend, kDesktop);
      otherProxy = makeProxy(*other, kDesktop);
      proxy->uponSignal("Activated")
          .onInterface(kInterface)
          .call([this](const sdbus::ObjectPath&, const std::string& id, uint64_t time, const Dict& options) {
            require(id == "talk" && options.empty(), "activation exposed unexpected public data");
            activated.push_back(time);
          });
      proxy->uponSignal("Deactivated")
          .onInterface(kInterface)
          .call([this](const sdbus::ObjectPath&, const std::string& id, uint64_t time, const Dict& options) {
            require(id == "talk" && options.empty(), "deactivation exposed unexpected public data");
            deactivated.push_back(time);
          });
      proxy->uponSignal("ShortcutsChanged")
          .onInterface(kInterface)
          .call([this](const sdbus::ObjectPath&, Shortcuts shortcuts) { changed.push_back(std::move(shortcuts)); });
    }
    ~BusFixture() { portal.reset(); }
    BusFixture(const BusFixture&) = delete;
    BusFixture& operator=(const BusFixture&) = delete;

    static std::unique_ptr<sdbus::IProxy> makeProxy(sdbus::IConnection& connection, const std::string& path) {
      return sdbus::createProxy(connection, sdbus::ServiceName{kBackend}, sdbus::ObjectPath{path});
    }
    void pump() const {
      for (auto* connection : {backend.get(), frontend.get(), other.get()}) {
        while (connection->processPendingEvent()) {
        }
      }
      transport.sync();
      require(transport.loop->addTimer(0, [this] { transport.loop->quit(); }) != 0, "test loop timer failed");
      transport.loop->run();
    }
    void until(
        const std::function<bool()>& ready, const std::source_location& location = std::source_location::current()
    ) const {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error(
              std::format("private D-Bus operation timed out at {}:{}", location.function_name(), location.line())
          );
        }
        pump();
      }
    }
    std::string handle() { return "/org/freedesktop/portal/desktop/request/1_1/r" + std::to_string(++nextHandle); }
    static std::string path(size_t id) { return "/org/freedesktop/portal/desktop/session/1_1/s" + std::to_string(id); }
    template <typename... Args>
    std::shared_ptr<Reply> call(sdbus::IProxy& target, const char* method, const std::string& request, Args&&... args) {
      auto reply = std::make_shared<Reply>();
      reply->handle = request;
      target.callMethodAsync(method)
          .onInterface(kInterface)
          .withArguments(sdbus::ObjectPath{request}, std::forward<Args>(args)...)
          .uponReplyInvoke([reply](std::optional<sdbus::Error> error, uint32_t status, Dict results) {
            reply->error = error ? std::string(error->getName()) : std::string{};
            reply->status = status;
            reply->results = std::move(results);
            reply->done = true;
          });
      return reply;
    }
    std::shared_ptr<Reply> create(size_t id, const std::string& application = "org.test.App") {
      const auto session = path(id);
      if (watched.insert(session).second) {
        auto watch = makeProxy(*frontend, session);
        watch->uponSignal("Closed").onInterface(kSessionInterface).call([this, session] { closed.push_back(session); });
        objects.push_back(std::move(watch));
      }
      return call(*proxy, "CreateSession", handle(), sdbus::ObjectPath{session}, application, Dict{});
    }
    std::shared_ptr<Reply> bind(
        size_t id,
        const Shortcuts& shortcuts = {
            {"talk",
             {{"description", sdbus::Variant{std::string("Talk")}},
              {"preferred_trigger", sdbus::Variant{std::string("CTRL+m")}}}}
        }
    ) {
      return call(*proxy, "BindShortcuts", handle(), sdbus::ObjectPath{path(id)}, shortcuts, std::string{}, Dict{});
    }
    std::shared_ptr<Reply> list(size_t id, const std::string& request = {}) {
      return call(*proxy, "ListShortcuts", request.empty() ? handle() : request, sdbus::ObjectPath{path(id)});
    }
    void expect(
        const std::shared_ptr<Reply>& reply, uint32_t status,
        const std::source_location& location = std::source_location::current()
    ) const {
      until([&] { return reply->done; }, location);
      require(reply->error.empty() && reply->status == status, "unexpected public portal response");
    }
    void expectError(const std::shared_ptr<Reply>& reply, const char* error) const {
      until([&] { return reply->done; });
      require(reply->error == error, "wrong D-Bus ownership or state error");
    }
    std::shared_ptr<Reply> close(const std::string& path, const char* interface, bool intruder = false) {
      auto target = makeProxy(intruder ? *other : *frontend, path);
      auto reply = std::make_shared<Reply>();
      target->callMethodAsync("Close").onInterface(interface).uponReplyInvoke(
          [reply](std::optional<sdbus::Error> error) {
            reply->error = error ? std::string(error->getName()) : std::string{};
            reply->done = true;
          }
      );
      objects.push_back(std::move(target));
      return reply;
    }
    void barrier() const {
      bool done = false;
      proxy->callMethodAsync("Get")
          .onInterface("org.freedesktop.DBus.Properties")
          .withArguments(std::string{kInterface}, std::string{"version"})
          // sdbus-c++ requires its optional error argument by value.
          // NOLINTNEXTLINE(performance-unnecessary-value-param)
          .uponReplyInvoke([&](std::optional<sdbus::Error> error, const sdbus::Variant& version) {
            require(!error && version.get<uint32_t>() == 1, "backend version property changed");
            done = true;
          });
      until([&] { return done; });
    }
    Json seen(size_t session) const {
      if (transport.send({{"op", "sessions"}}).get<size_t>() <= session) {
        return Json::array();
      }
      return transport.send({{"op", "seen"}, {"session", session}});
    }
    uint32_t request(size_t session, const std::string& kind) const {
      std::optional<uint32_t> id;
      until([&] {
        for (const auto& entry : seen(session)) {
          if (entry["kind"] == kind) {
            id = entry["id"].get<uint32_t>();
          }
        }
        return id.has_value();
      });
      return *id;
    }
    void send(const char* op, size_t session, uint32_t request, const std::string& credential = {}) const {
      Json command = {{"op", op}, {"session", session}, {"request", request}};
      if (!credential.empty()) {
        command["credential"] = credential;
      }
      (void)transport.send(command);
    }
    std::optional<std::string> credential(const std::string& app = "org.test.App") const {
      auto acquired = Cache(files.path).acquire(app);
      require(
          acquired.status == xdpu::GlobalShortcutCacheStatus::Ready && acquired.transaction != nullptr,
          "application transaction was not released before public completion"
      );
      return acquired.transaction->credential();
    }
  };

  void ordinaryCallsAndSignals() {
    BusFixture fixture;
    fixture.barrier();
    auto created = fixture.create(0);
    fixture.expect(created, 0);
    require(created->results.empty(), "create returned private credential data");
    auto listed = fixture.list(0);
    auto request = fixture.request(0, "list");
    fixture.send("listed", 0, request);
    fixture.expect(listed, 0);
    require(
        listed->results.size() == 1 && listed->results.at("shortcuts").get<Shortcuts>().empty(),
        "unapproved session did not return an empty shortcut list"
    );
    auto bound = fixture.bind(0);
    request = fixture.request(0, "bind");
    const auto seen = fixture.seen(0);
    require(seen.back()["shortcuts"][0]["trigger"] == "CTRL+m", "preferred trigger was not forwarded");
    fixture.expectError(
        fixture.close(bound->handle, kRequestInterface, true), "org.freedesktop.portal.Error.NotAllowed"
    );
    fixture.expect(fixture.bind(0), 2);
    fixture.send("grant", 0, request, "private-bind-credential");
    fixture.send("shortcut", 0, request);
    fixture.send("bound", 0, request);
    fixture.expect(bound, 0);
    require(fixture.credential() == "private-bind-credential", "bind succeeded before persisting its credential");
    fixture.expect(fixture.bind(0), 2);
    fixture.barrier();
    require(
        fixture.transport.send({{"op", "alive"}}).get<bool>(), "repeated bind disrupted an established registration"
    );
    const auto shortcuts = bound->results.at("shortcuts").get<Shortcuts>();
    require(
        bound->results.size() == 1
            && shortcuts.size() == 1
            && std::get<0>(shortcuts[0]) == "talk"
            && std::get<1>(shortcuts[0]).size() == 2
            && std::get<1>(shortcuts[0]).at("trigger_description").get<std::string>() == "Ctrl+M",
        "bound public shortcut data was incomplete or exposed transport data"
    );
    fixture.transport.send({{"op", "activated"}, {"time", std::numeric_limits<uint32_t>::max()}});
    fixture.transport.send({{"op", "deactivated"}, {"time", 7}});
    fixture.send("changed", 0, 0);
    fixture.send("changes-done", 0, 0);
    fixture.send("changes-done", 0, 0);
    fixture.until([&] {
      return fixture.activated.size() == 1 && fixture.deactivated.size() == 1 && fixture.changed.size() == 2;
    });
    require(
        fixture.activated[0] == 4294967295ULL
            && fixture.deactivated[0] == 7
            && fixture.changed[0].size() == 1
            && fixture.changed[1].empty(),
        "public signals lost timestamps or snapshots"
    );
    const auto beforeList = fixture.seen(0).size();
    listed = fixture.list(0);
    fixture.until([&] { return fixture.seen(0).size() > beforeList; });
    const auto newRequest = fixture.seen(0).back()["id"].get<uint32_t>();
    fixture.send("shortcut", 0, newRequest);
    fixture.send("listed", 0, newRequest);
    fixture.expect(listed, 0);
    require(listed->results.at("shortcuts").get<Shortcuts>().size() == 1, "bound shortcut list was lost");
    fixture.expectError(
        fixture.close(BusFixture::path(0), kSessionInterface, true), "org.freedesktop.portal.Error.NotAllowed"
    );
    auto closed = fixture.close(BusFixture::path(0), kSessionInterface);
    fixture.until([&] { return closed->done; });
    require(closed->error.empty(), "owner could not close its session");
    fixture.expectError(fixture.list(0), "org.freedesktop.portal.Error.Failed");
    fixture.transport.sync();
    require(!fixture.transport.send({{"op", "alive"}}).get<bool>(), "session close retained its bridge registration");
  }

  void restorationAndSerialization() {
    BusFixture fixture;
    {
      auto acquired = Cache(fixture.files.path).acquire("org.test.App");
      require(
          acquired.transaction && acquired.transaction->save("original") == xdpu::GlobalShortcutCacheStatus::Ready,
          "restore fixture cache setup failed"
      );
    }
    auto first = fixture.create(0);
    auto request = fixture.request(0, "restore");
    require(fixture.seen(0)[0]["credential"] == "original", "cached credential was not restored");
    auto second = fixture.create(1);
    fixture.barrier();
    require(
        !first->done && !second->done && fixture.transport.send({{"op", "sessions"}}) == 1,
        "same-application creates did not serialize credential rotation"
    );
    fixture.send("grant", 0, request, "rotated");
    fixture.send("restored", 0, request);
    fixture.expect(first, 0);
    request = fixture.request(1, "restore");
    require(fixture.seen(1)[0]["credential"] == "rotated", "next session restored a stale credential");
    fixture.transport.send({{"op", "restored"}, {"session", 1}, {"request", request}, {"status", 2}});
    fixture.expect(second, 0);
    require(!fixture.credential(), "invalid cached credential was not cleared");
    auto bound = fixture.bind(1, {});
    request = fixture.request(1, "bind");
    fixture.send("grant", 1, request, "fresh-approval");
    fixture.send("bound", 1, request);
    fixture.expect(bound, 0);
    require(
        bound->results.at("shortcuts").get<Shortcuts>().empty() && fixture.credential() == "fresh-approval",
        "empty successful bind did not save its new approval credential"
    );
  }

  void ownershipAndCancellation() {
    BusFixture fixture;
    const auto denied = fixture.call(
        *fixture.otherProxy, "CreateSession", fixture.handle(), sdbus::ObjectPath{BusFixture::path(0)},
        std::string{"app"}, Dict{}
    );
    fixture.expectError(denied, "org.freedesktop.portal.Error.NotAllowed");
    fixture.expect(fixture.create(0), 0);
    auto bound = fixture.bind(0);
    const auto request = fixture.request(0, "bind");
    auto cancelled = fixture.close(bound->handle, kRequestInterface);
    fixture.until([&] { return cancelled->done; });
    require(cancelled->error.empty(), "owner could not cancel bind");
    fixture.expect(bound, 1);
    fixture.until([&] { return fixture.closed.size() == 1; });
    fixture.transport.sync();
    require(
        fixture.seen(0).back()["kind"] == "cancel" && !fixture.transport.send({{"op", "alive"}}).get<bool>(),
        "cancelled bind retained a registration"
    );
    require(request != 0 && !fixture.credential(), "cancelled bind saved a credential");

    fixture.expect(fixture.create(1), 0);
    auto listed = fixture.list(1);
    (void)fixture.request(1, "list");
    fixture.expect(fixture.create(2, "org.test.Other"), 0);
    fixture.expect(fixture.list(2, listed->handle), 2);
    require(fixture.seen(2).empty(), "a reused request handle reached another session");
    auto duplicate = fixture.list(1, listed->handle);
    fixture.expect(duplicate, 2);
    auto unauthorized =
        fixture.call(*fixture.otherProxy, "ListShortcuts", fixture.handle(), sdbus::ObjectPath{BusFixture::path(1)});
    fixture.expectError(unauthorized, "org.freedesktop.portal.Error.NotAllowed");
    fixture.frontend->releaseName(sdbus::ServiceName{kFrontend});
    fixture.barrier();
    require(
        listed->done && listed->status == 2 && fixture.closed.size() == 3,
        "frontend owner loss did not retire all sessions and pending requests"
    );
    fixture.expect(listed, 2);
    fixture.until([&] { return fixture.closed.size() == 3; });
    fixture.other->requestName(sdbus::ServiceName{kFrontend});
    auto takeover =
        fixture.call(*fixture.otherProxy, "ListShortcuts", fixture.handle(), sdbus::ObjectPath{BusFixture::path(1)});
    fixture.expectError(takeover, "org.freedesktop.portal.Error.Failed");
  }

  void persistenceFailureAndDelayedSignals() {
    BusFixture fixture;
    fixture.expect(fixture.create(0), 0);
    auto bound = fixture.bind(0);
    auto request = fixture.request(0, "bind");
    const int lock =
        open((fixture.files.path.parent_path() / ".global-shortcuts-credentials.lock").c_str(), O_RDWR | O_CLOEXEC);
    require(lock >= 0, "cache file lock setup failed");
    struct flock range{};
    range.l_type = F_WRLCK;
    range.l_whence = SEEK_SET;
    range.l_len = 1;
    require(fcntl(lock, F_OFD_SETLK, &range) == 0, "cache file lock failed");
    fixture.send("grant", 0, request, "persisted-after-contention");
    fixture.send("shortcut", 0, request);
    fixture.send("bound", 0, request);
    fixture.send("activated", 0, 0);
    fixture.send("changed", 0, 0);
    fixture.send("changes-done", 0, 0);
    fixture.barrier();
    require(
        !bound->done && fixture.activated.empty() && fixture.changed.empty(),
        "pending persistence exposed a public success or shortcut signal"
    );
    require(close(lock) == 0, "cache file unlock failed");
    fixture.expect(bound, 0);
    require(fixture.credential() == "persisted-after-contention", "contended persistence lost credential");
    fixture.barrier();
    require(fixture.changed.size() == 1, "snapshot received during persistence was not delivered after success");
    fixture.send("deactivated", 0, 0);
    fixture.barrier();
    require(fixture.deactivated.empty(), "release was forwarded without its suppressed activation");
    fixture.send("activated", 0, 0);
    fixture.until([&] { return fixture.activated.size() == 1; });

    fixture.expect(fixture.create(1, "org.test.Other"), 0);
    bound = fixture.bind(1);
    request = fixture.request(1, "bind");
    const auto before = [&] {
      std::ifstream file(fixture.files.path);
      return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }();
    failRename = true;
    renameFailed = false;
    fixture.send("grant", 1, request, "must-not-publish");
    fixture.send("bound", 1, request);
    fixture.expect(bound, 2);
    failRename = false;
    fixture.barrier();
    require(
        renameFailed
            && fixture.closed.size() == 1
            && fixture.closed.back() == BusFixture::path(1)
            && !fixture.credential("org.test.Other"),
        "failed credential save did not close its session"
    );
    std::ifstream file(fixture.files.path);
    require(
        std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()) == before,
        "failed credential save changed the previous cache"
    );
    fixture.transport.sync();
    require(
        !fixture.transport.send({{"op", "alive"}, {"session", 1}}).get<bool>(),
        "failed credential save left an active registration"
    );
  }

  void validationMissingBridgeAndDeadlines() {
    {
      BusFixture fixture;
      fixture.expect(fixture.create(0, ""), 2);
      require(fixture.transport.send({{"op", "sessions"}}) == 0, "invalid app created a bridge session");
      fixture.expect(fixture.create(0), 0);
      const Shortcuts malformed{{"talk", {{"description", sdbus::Variant{uint32_t{1}}}}}};
      fixture.expect(fixture.bind(0, malformed), 2);
      fixture.expect(fixture.bind(0), 2);
      require(fixture.seen(0).empty(), "invalid or repeated bind reached the compositor");
      auto first = fixture.list(0);
      const auto request = fixture.request(0, "list");
      auto cancelled = fixture.close(first->handle, kRequestInterface);
      fixture.until([&] { return cancelled->done; });
      fixture.expect(first, 1);
      fixture.expect(fixture.create(1, "org.test.Other"), 0);
      auto second = fixture.list(1);
      const auto secondRequest = fixture.request(1, "list");
      fixture.send("listed", 0, request);
      fixture.send("listed", 1, secondRequest);
      fixture.expect(second, 0);
      fixture.send("closed", 0, 0);
      fixture.until([&] { return fixture.closed.size() == 1; });
      fixture.expectError(fixture.list(0), "org.freedesktop.portal.Error.Failed");
      fixture.transport.send({{"op", "remove"}});
      fixture.until([&] { return fixture.closed.size() == 2; });
      fixture.expectError(fixture.create(2), "org.freedesktop.portal.Error.Failed");
    }
    {
      BusFixture fixture(false);
      fixture.expectError(fixture.create(0), "org.freedesktop.portal.Error.Failed");
      fixture.barrier();
    }
    {
      BusFixture fixture(true, {80, 160});
      auto acquired = Cache(fixture.files.path).acquire("org.test.App");
      require(acquired.transaction != nullptr, "timeout fixture application lock failed");
      auto waiting = fixture.create(0);
      fixture.expect(waiting, 2);
      require(fixture.transport.send({{"op", "sessions"}}) == 0, "expired cache wait created a bridge session");
      acquired.transaction.reset();
      fixture.expect(fixture.create(1), 0);
      auto unanswered = fixture.list(1);
      fixture.expect(unanswered, 2);
      fixture.until([&] { return fixture.closed.size() == 2; });
    }
  }

  void screenshotRequiresFrontend() {
    BusFixture fixture;
    xdpu::ScreenshotPortal screenshot(
        *fixture.transport.loop, *fixture.backend, *fixture.desktop, xdpu::Config{}, *fixture.transport.wayland
    );
    auto reply = std::make_shared<Reply>();
    fixture.otherProxy->callMethodAsync("Screenshot")
        .onInterface("org.freedesktop.impl.portal.Screenshot")
        .withArguments(sdbus::ObjectPath{fixture.handle()}, std::string{"org.test.App"}, std::string{}, Dict{})
        .uponReplyInvoke([reply](std::optional<sdbus::Error> error, uint32_t, const Dict&) {
          reply->error = error ? std::string(error->getName()) : std::string{};
          reply->done = true;
        });
    fixture.expectError(reply, "org.freedesktop.portal.Error.NotAllowed");
  }
} // namespace

extern "C" {
int __real_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int __wrap_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
  if (failRename) {
    renameFailed = true;
    errno = EIO;
    return -1;
  }
  return __real_renameat(olddirfd, oldpath, newdirfd, newpath);
}
}

int main() {
  try {
    ordinaryCallsAndSignals();
    restorationAndSerialization();
    ownershipAndCancellation();
    persistenceFailureAndDelayedSignals();
    validationMissingBridgeAndDeadlines();
    screenshotRequiresFrontend();
    std::println("global shortcuts portal methods, ownership, persistence, cancellation, and deadlines passed");
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::println(stderr, "global shortcuts portal: {}", error.what());
    return EXIT_FAILURE;
  }
}
