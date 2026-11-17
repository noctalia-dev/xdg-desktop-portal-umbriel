// Drives the public GlobalShortcuts frontend. JSON commands on stdin and JSON
// events on stdout let an isolated test control approval and input separately.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  using Json = nlohmann::json;
  using Dict = std::map<std::string, sdbus::Variant>;
  using Shortcuts = std::vector<sdbus::Struct<std::string, Dict>>;
  constexpr auto kService = "org.freedesktop.portal.Desktop";
  constexpr auto kInterface = "org.freedesktop.portal.GlobalShortcuts";
  constexpr auto kRequestInterface = "org.freedesktop.portal.Request";
  constexpr auto kSessionInterface = "org.freedesktop.portal.Session";
  constexpr size_t kObjectLimit = 64;
  constexpr size_t kInputLimit = 2 * 1024 * 1024;

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  void emit(const Json& event) { std::println("{}", event.dump()); }

  Json shortcutsJson(const Shortcuts& shortcuts) {
    Json result = Json::array();
    for (const auto& [id, properties] : shortcuts) {
      require(
          properties.size() == 2 && properties.contains("description") && properties.contains("trigger_description"),
          "unexpected public shortcut fields"
      );
      result.push_back(
          {{"id", id},
           {"description", properties.at("description").get<std::string>()},
           {"trigger_description", properties.at("trigger_description").get<std::string>()}}
      );
    }
    return result;
  }

  class Client {
  public:
    explicit Client(const std::string& application) {
      m_bus = sdbus::createSessionBusConnection();
      m_portal = sdbus::createProxy(
          *m_bus, sdbus::ServiceName{kService}, sdbus::ObjectPath{"/org/freedesktop/portal/desktop"}
      );
      m_portal->callMethod("Register")
          .onInterface("org.freedesktop.host.portal.Registry")
          .withArguments(application, Dict{});
      sdbus::Variant version;
      m_portal->callMethod("Get")
          .onInterface("org.freedesktop.DBus.Properties")
          .withArguments(std::string{kInterface}, std::string{"version"})
          .storeResultsTo(version);
      require(version.get<uint32_t>() == 1, "expected GlobalShortcuts version 1");
      std::string sender = m_bus->getUniqueName();
      require(sender.starts_with(':'), "unexpected unique D-Bus name");
      sender.erase(0, 1);
      std::ranges::replace(sender, '.', '_');
      m_requestPrefix = "/org/freedesktop/portal/desktop/request/" + sender + "/";
      m_sessionPrefix = "/org/freedesktop/portal/desktop/session/" + sender + "/";
      m_portal->uponSignal("Activated")
          .onInterface(kInterface)
          .call([this](const sdbus::ObjectPath& path, const std::string& id, uint64_t time, const Dict& options) {
            activation("activated", path, id, time, options);
          });
      m_portal->uponSignal("Deactivated")
          .onInterface(kInterface)
          .call([this](const sdbus::ObjectPath& path, const std::string& id, uint64_t time, const Dict& options) {
            activation("deactivated", path, id, time, options);
          });
      m_portal->uponSignal("ShortcutsChanged")
          .onInterface(kInterface)
          .call([this](const sdbus::ObjectPath& path, const Shortcuts& shortcuts) {
            emit(
                {{"event", "changed"},
                 {"session", label(path)},
                 {"session_handle", std::string(path)},
                 {"shortcuts", shortcutsJson(shortcuts)}}
            );
          });
      emit(
          {{"event", "ready"},
           {"application", application},
           {"version", version.get<uint32_t>()},
           {"sender", std::string(m_bus->getUniqueName())}}
      );
    }

    void run() {
      while (!m_done) {
        while (m_bus->processPendingEvent()) {
        }
        collect();
        const auto data = m_bus->getEventLoopPollData();
        std::array<pollfd, 3> fds{{{data.fd, data.events, 0}, {data.eventFd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}}};
        const auto ready = poll(fds.data(), fds.size(), data.getPollTimeout());
        require(ready >= 0 || errno == EINTR, "client poll failed");
        if (ready > 0 && (fds[2].revents & (POLLIN | POLLHUP)) != 0) {
          readInput();
        }
      }
    }

  private:
    struct Session {
      std::string name;
      sdbus::ObjectPath path;
      std::unique_ptr<sdbus::IProxy> proxy;
      bool ready = false;
      bool closed = false;
    };

    struct Request {
      uint64_t id = 0;
      std::string method;
      std::string tag;
      std::shared_ptr<Session> session;
      std::unique_ptr<sdbus::IProxy> proxy;
      bool done = false;
      bool cancelled = false;
    };
    using Invoke = std::function<void(const Dict&, sdbus::ObjectPath&)>;

    void collect() {
      std::erase_if(m_requests, [](const auto& entry) { return entry.second->done; });
      std::erase_if(m_sessions, [](const auto& entry) { return entry.second->closed; });
    }

    std::unique_ptr<sdbus::IProxy> proxy(const sdbus::ObjectPath& path) {
      return sdbus::createProxy(*m_bus, sdbus::ServiceName{kService}, path);
    }

    Json label(const sdbus::ObjectPath& path) const {
      for (const auto& [name, session] : m_sessions) {
        if (session->path == path) {
          return name;
        }
      }
      return nullptr;
    }

    void activation(
        const char* event, const sdbus::ObjectPath& path, const std::string& id, uint64_t time, const Dict& options
    ) const {
      require(options.empty(), "unexpected public activation options");
      emit(
          {{"event", event},
           {"session", label(path)},
           {"session_handle", std::string(path)},
           {"id", id},
           {"time", time}}
      );
    }

    void complete(const std::shared_ptr<Request>& request, uint32_t status, const Dict& results) {
      require(!request->done && status <= 2, "duplicate or invalid public response");
      request->done = true;
      const bool create = request->method == "CreateSession";
      const char* field = create ? "session_handle" : "shortcuts";
      require(results.size() <= 1 && (results.empty() || results.contains(field)), "unexpected public response fields");
      Json event = {{"event", "response"}, {"method", request->method},         {"request", request->id},
                    {"tag", request->tag}, {"session", request->session->name}, {"status", status}};
      if (create) {
        if (status == 0) {
          require(
              results.contains(field) && results.at(field).containsValueOfType<std::string>(),
              "frontend session handle must be a string"
          );
          const auto path = results.at(field).get<std::string>();
          require(path == request->session->path && !request->session->closed, "unexpected frontend session handle");
          request->session->ready = true;
          event["session_handle"] = path;
        } else {
          request->session->closed = true;
        }
      } else if (results.contains(field)) {
        event["shortcuts"] = shortcutsJson(results.at(field).get<Shortcuts>());
      } else {
        require(status != 0, "successful bind or list omitted shortcuts");
      }
      emit(event);
    }

    void request(
        const std::string& method, const Json& command, const std::shared_ptr<Session>& session, const Invoke& invoke
    ) {
      require(m_requests.size() < kObjectLimit, "too many pending client requests");
      auto pending = std::make_shared<Request>();
      pending->id = ++m_serial;
      pending->method = method;
      pending->tag = command.value("tag", std::string{});
      pending->session = session;
      const std::string token = "shortcut_request_" + std::to_string(pending->id);
      const sdbus::ObjectPath expected{m_requestPrefix + token};
      pending->proxy = proxy(expected);
      const std::weak_ptr<Request> weak = pending;
      pending->proxy->uponSignal("Response")
          .onInterface(kRequestInterface)
          .call([this, weak](uint32_t status, const Dict& results) {
            if (const auto current = weak.lock(); current && !current->cancelled) {
              complete(current, status, results);
            }
          });
      m_requests.emplace(pending->id, pending);
      emit(
          {{"event", "requested"},
           {"method", method},
           {"request", pending->id},
           {"tag", pending->tag},
           {"session", session->name}}
      );
      try {
        sdbus::ObjectPath returned;
        invoke({{"handle_token", sdbus::Variant{token}}}, returned);
        require(returned == expected, "frontend ignored the request handle token");
      } catch (const sdbus::Error& error) {
        pending->done = true;
        if (method == "CreateSession") {
          session->closed = true;
        }
        emit(
            {{"event", "error"},
             {"method", method},
             {"request", pending->id},
             {"tag", pending->tag},
             {"session", session->name},
             {"error", std::string(error.getName())}}
        );
      }
    }

    void create(const Json& command, const std::string& name) {
      require(
          !name.empty() && !m_sessions.contains(name) && m_sessions.size() < kObjectLimit,
          "invalid, duplicate, or excessive client session"
      );
      auto session = std::make_shared<Session>();
      session->name = name;
      const std::string token = "shortcut_session_" + std::to_string(++m_nextSession);
      session->path = sdbus::ObjectPath{m_sessionPrefix + token};
      session->proxy = proxy(session->path);
      const std::weak_ptr<Session> weak = session;
      session->proxy->uponSignal("Closed").onInterface(kSessionInterface).call([weak](const Dict& options) {
        require(options.empty(), "unexpected public session-close options");
        if (const auto current = weak.lock()) {
          current->closed = true;
          emit({{"event", "closed"}, {"session", current->name}, {"session_handle", std::string(current->path)}});
        }
      });
      m_sessions.emplace(name, session);
      request("CreateSession", command, session, [&](const Dict& base, sdbus::ObjectPath& handle) {
        Dict options = base;
        options.emplace("session_handle_token", sdbus::Variant{token});
        m_portal->callMethod("CreateSession").onInterface(kInterface).withArguments(options).storeResultsTo(handle);
      });
    }

    void cancel(const Json& command) {
      require(!m_requests.empty(), "no client request to cancel");
      const auto id = command.value("request", m_requests.rbegin()->first);
      const auto found = m_requests.find(id);
      require(found != m_requests.end() && !found->second->done, "request is not pending");
      const auto pending = found->second;
      pending->proxy->callMethod("Close").onInterface(kRequestInterface);
      pending->cancelled = pending->done = true;
      if (pending->method == "CreateSession") {
        pending->session->closed = true;
      }
      // Closing a public request can suppress its Response signal.
      emit({{"event", "cancelled"}, {"request", id}, {"session", pending->session->name}, {"tag", pending->tag}});
    }

    void command(const Json& value) {
      const auto op = value.at("op").get<std::string>();
      const auto name = value.value("session", std::string{"main"});
      if (op == "quit") {
        m_done = true;
        return;
      }
      if (op == "cancel") {
        cancel(value);
        return;
      }
      if (op == "create") {
        create(value, name);
        return;
      }
      const auto found = m_sessions.find(name);
      require(
          found != m_sessions.end() && found->second->ready && !found->second->closed, "client session is not ready"
      );
      const auto session = found->second;
      if (op == "bind") {
        Shortcuts shortcuts;
        for (const auto& entry : value.value("shortcuts", Json::array())) {
          Dict properties{{"description", sdbus::Variant{entry.at("description").get<std::string>()}}};
          if (entry.contains("preferred_trigger")) {
            properties.emplace("preferred_trigger", sdbus::Variant{entry.at("preferred_trigger").get<std::string>()});
          }
          shortcuts.emplace_back(entry.at("id").get<std::string>(), std::move(properties));
        }
        request("BindShortcuts", value, session, [&](const Dict& options, sdbus::ObjectPath& handle) {
          m_portal->callMethod("BindShortcuts")
              .onInterface(kInterface)
              .withArguments(session->path, shortcuts, value.value("parent_window", std::string{}), options)
              .storeResultsTo(handle);
        });
      } else if (op == "list") {
        request("ListShortcuts", value, session, [&](const Dict& options, sdbus::ObjectPath& handle) {
          m_portal->callMethod("ListShortcuts")
              .onInterface(kInterface)
              .withArguments(session->path, options)
              .storeResultsTo(handle);
        });
      } else if (op == "close") {
        session->proxy->callMethod("Close").onInterface(kSessionInterface);
        session->closed = true;
        emit({{"event", "requested"}, {"method", "Close"}, {"session", name}});
      } else if (op == "configure") {
        try {
          m_portal->callMethod("ConfigureShortcuts")
              .onInterface(kInterface)
              .withArguments(session->path, std::string{}, Dict{});
          emit({{"event", "configured"}, {"session", name}});
        } catch (const sdbus::Error& error) {
          emit(
              {{"event", "error"},
               {"method", "ConfigureShortcuts"},
               {"session", name},
               {"error", std::string(error.getName())}}
          );
        }
      } else {
        throw std::runtime_error("unknown client operation");
      }
    }

    void readInput() {
      std::array<char, 4096> bytes{};
      const auto count = read(STDIN_FILENO, bytes.data(), bytes.size());
      if (count < 0 && errno == EINTR) {
        return;
      }
      require(count >= 0, "client input read failed");
      if (count == 0) {
        require(m_input.empty(), "unterminated JSON command");
        m_done = true;
        return;
      }
      m_input.append(bytes.data(), static_cast<size_t>(count));
      require(m_input.size() <= kInputLimit, "client command is too large");
      size_t end = 0;
      while (!m_done && (end = m_input.find('\n')) != std::string::npos) {
        const auto line = m_input.substr(0, end);
        m_input.erase(0, end + 1);
        command(Json::parse(line));
        collect();
      }
    }

    std::unique_ptr<sdbus::IConnection> m_bus;
    std::unique_ptr<sdbus::IProxy> m_portal;
    std::map<std::string, std::shared_ptr<Session>> m_sessions;
    std::map<uint64_t, std::shared_ptr<Request>> m_requests;
    std::string m_requestPrefix;
    std::string m_sessionPrefix;
    std::string m_input;
    uint64_t m_serial = 0;
    uint64_t m_nextSession = 0;
    bool m_done = false;
  };
} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::println("usage: global-shortcuts-test-client [APPLICATION_ID]");
    std::println("Use a private portal frontend with a matching desktop entry. One JSON command per line:");
    std::println("{}", R"({"op":"create","session":"main","tag":"create"})");
    std::println(
        "{}",
        R"({"op":"bind","session":"main","tag":"bind","shortcuts":[{"id":"talk","description":"Talk","preferred_trigger":"CTRL+m"}]})"
    );
    std::println("{}", R"({"op":"list","session":"main","tag":"list"})");
    std::println("{}", R"({"op":"cancel"})");
    std::println("{}", R"({"op":"configure","session":"main"})");
    std::println("{}", R"({"op":"close","session":"main"})");
    std::println("{}", R"({"op":"quit"})");
    return EXIT_SUCCESS;
  }
  try {
    require(argc <= 2, "expected at most one application ID");
    Client client(argc == 2 ? argv[1] : "org.umbriel.GlobalShortcutsTest");
    client.run();
    return EXIT_SUCCESS;
  } catch (const Json::exception&) {
    std::println(stderr, "global-shortcuts-test-client: invalid JSON command or public result");
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::println(stderr, "global-shortcuts-test-client: {}", error.what());
    return EXIT_FAILURE;
  }
}
