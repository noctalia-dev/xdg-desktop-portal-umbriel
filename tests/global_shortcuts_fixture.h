#pragma once

#include "loop/loop.h"
#include "wayland/wayland.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <global-shortcuts-server-protocol.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace shortcut_test {
  using Json = nlohmann::json;

  inline void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  inline void writeLine(int fd, const Json& value) {
    const auto text = value.dump() + "\n";
    size_t offset = 0;
    while (offset < text.size()) {
      const auto count = send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      require(count > 0, "private control write failed");
      offset += static_cast<size_t>(count);
    }
  }

  struct Server {
    struct Session {
      wl_resource* resource;
      Json requests;
      Json staged;
      Session() : resource(nullptr), requests(Json::array()), staged(Json::array()) {}
    };

    wl_display* display = wl_display_create();
    wl_global* global = nullptr;
    wl_interface interface = umbriel_global_shortcuts_manager_v1_interface;
    std::vector<std::unique_ptr<Session>> sessions;
    uint32_t boundVersion = 0;
    int control;
    std::string input;

    Server(int wayland, int control, bool advertise) : control(control) {
      require(display != nullptr && wl_client_create(display, wayland) != nullptr, "private server setup failed");
      interface.version = 2;
      if (advertise) {
        announce();
      }
      require(
          wl_event_loop_add_fd(wl_display_get_event_loop(display), control, WL_EVENT_READABLE, readControl, this)
              != nullptr,
          "private control registration failed"
      );
    }
    ~Server() {
      wl_display_destroy_clients(display);
      if (global != nullptr) {
        wl_global_destroy(global);
      }
      wl_display_destroy(display);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    static Session& session(wl_resource* resource) {
      return *static_cast<Session*>(wl_resource_get_user_data(resource));
    }
    static void destroy(wl_client*, wl_resource* resource) { wl_resource_destroy(resource); }

    static const struct umbriel_global_shortcuts_session_v1_interface kSession;

    static const struct umbriel_global_shortcuts_manager_v1_interface kManager;

    void announce() {
      global = wl_global_create(
          display, &interface, 2, this, [](wl_client* client, void* data, uint32_t version, uint32_t id) {
            auto& server = *static_cast<Server*>(data);
            server.boundVersion = version;
            auto* resource = wl_resource_create(
                client, &umbriel_global_shortcuts_manager_v1_interface, static_cast<int>(version), id
            );
            require(resource != nullptr, "private manager allocation failed");
            wl_resource_set_implementation(resource, &kManager, &server, nullptr);
          }
      );
      require(global != nullptr, "private global allocation failed");
    }

    Json command(const Json& command) {
      const auto op = command.at("op").get<std::string>();
      if (op == "quit") {
        wl_display_terminate(display);
        return true;
      }
      if (op == "remove") {
        wl_global_destroy(std::exchange(global, nullptr));
        return true;
      }
      if (op == "announce") {
        announce();
        return true;
      }
      if (op == "version") {
        return boundVersion;
      }
      if (op == "sessions") {
        return sessions.size();
      }
      auto& state = *sessions.at(command.value("session", size_t{0}));
      if (op == "seen") {
        return state.requests;
      }
      if (op == "alive") {
        return state.resource != nullptr;
      }
      require(state.resource != nullptr, "server command targets a destroyed session");
      const uint32_t request = command.value("request", uint32_t{1});
      const uint32_t status = command.value("status", uint32_t{0});
      if (op == "grant") {
        umbriel_global_shortcuts_session_v1_send_grant(
            state.resource, request, command.at("credential").get_ref<const std::string&>().c_str()
        );
      } else if (op == "restored") {
        umbriel_global_shortcuts_session_v1_send_restored(state.resource, request, status);
      } else if (op == "bound") {
        umbriel_global_shortcuts_session_v1_send_bound(state.resource, request, status);
      } else if (op == "listed") {
        umbriel_global_shortcuts_session_v1_send_listed(state.resource, request, status);
      } else if (op == "closed") {
        umbriel_global_shortcuts_session_v1_send_closed(state.resource);
      } else if (op == "activated" || op == "deactivated") {
        const auto id = command.value("id", std::string("talk"));
        const auto time = command.value("time", uint32_t{0});
        if (op == "activated") {
          umbriel_global_shortcuts_session_v1_send_activated(state.resource, id.c_str(), time);
        } else {
          umbriel_global_shortcuts_session_v1_send_deactivated(state.resource, id.c_str(), time);
        }
      } else if (op == "changes-done") {
        umbriel_global_shortcuts_session_v1_send_changes_done(state.resource);
      } else if (op == "shortcut" || op == "changed") {
        const auto id = command.value("id", std::string("talk"));
        const auto description = command.value("description", std::string("Action"));
        const auto trigger = command.value("trigger", std::string("Ctrl+M"));
        if (op == "shortcut") {
          umbriel_global_shortcuts_session_v1_send_shortcut(
              state.resource, request, id.c_str(), description.c_str(), trigger.c_str()
          );
        } else {
          umbriel_global_shortcuts_session_v1_send_changed(
              state.resource, id.c_str(), description.c_str(), trigger.c_str()
          );
        }
      } else {
        require(false, "unknown private server command");
      }
      return true;
    }

    static int readControl(int fd, uint32_t, void* data) {
      auto& server = *static_cast<Server*>(data);
      std::array<char, 4096> buffer{};
      const auto count = read(fd, buffer.data(), buffer.size());
      if (count <= 0) {
        wl_display_terminate(server.display);
        return 0;
      }
      server.input.append(buffer.data(), static_cast<size_t>(count));
      while (server.input.contains('\n')) {
        const auto end = server.input.find('\n');
        auto line = server.input.substr(0, end);
        server.input.erase(0, end + 1);
        auto result = server.command(Json::parse(line));
        wl_display_flush_clients(server.display);
        writeLine(server.control, result);
      }
      return 0;
    }
  };

  inline const struct umbriel_global_shortcuts_session_v1_interface Server::kSession{
      .destroy = destroy,
      .restore = [](
                     wl_client*, wl_resource* resource, uint32_t id, const char* token
                 ) { session(resource).requests.push_back({{"kind", "restore"}, {"id", id}, {"credential", token}}); },
      .add_shortcut =
          [](wl_client*, wl_resource* resource, const char* id, const char* description, const char* trigger) {
            session(resource).staged.push_back({{"id", id}, {"description", description}, {"trigger", trigger}});
          },
      .bind =
          [](wl_client*, wl_resource* resource, uint32_t id) {
            auto& state = session(resource);
            state.requests.push_back(
                {{"kind", "bind"}, {"id", id}, {"shortcuts", std::exchange(state.staged, Json::array())}}
            );
          },
      .list = [](wl_client*, wl_resource* resource,
                 uint32_t id) { session(resource).requests.push_back({{"kind", "list"}, {"id", id}}); },
      .cancel = [](wl_client*, wl_resource* resource,
                   uint32_t id) { session(resource).requests.push_back({{"kind", "cancel"}, {"id", id}}); },
  };

  inline const struct umbriel_global_shortcuts_manager_v1_interface Server::kManager{
      .destroy = destroy,
      .create_session = [](wl_client* client, wl_resource* manager, uint32_t id, const char*) {
        auto& server = *static_cast<Server*>(wl_resource_get_user_data(manager));
        auto state = std::make_unique<Session>();
        state->resource = wl_resource_create(client, &umbriel_global_shortcuts_session_v1_interface, 1, id);
        require(state->resource != nullptr, "private session allocation failed");
        wl_resource_set_implementation(state->resource, &kSession, state.get(), [](wl_resource* resource) {
          session(resource).resource = nullptr;
        });
        server.sessions.push_back(std::move(state));
      },
  };

  class Fixture {
  public:
    std::unique_ptr<xdpu::Loop> loop;
    std::unique_ptr<xdpu::WaylandContext> wayland;

    explicit Fixture(bool advertise = true) {
      std::array<int, 2> displaySockets{}, controlSockets{};
      require(
          socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, displaySockets.data()) == 0, "display socketpair failed"
      );
      require(
          socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, controlSockets.data()) == 0, "control socketpair failed"
      );
      m_child = fork();
      require(m_child >= 0, "private server fork failed");
      if (m_child == 0) {
        close(displaySockets[0]);
        close(controlSockets[0]);
        try {
          {
            Server server(displaySockets[1], controlSockets[1], advertise);
            wl_display_run(server.display);
          }
          _exit(EXIT_SUCCESS);
        } catch (...) {
          _exit(EXIT_FAILURE);
        }
      }
      close(displaySockets[1]);
      close(controlSockets[1]);
      m_control = controlSockets[0];
      const char* previous = std::getenv("WAYLAND_SOCKET");
      const auto saved = previous != nullptr ? std::optional<std::string>{previous} : std::nullopt;
      setenv("WAYLAND_SOCKET", std::to_string(displaySockets[0]).c_str(), 1);
      loop = std::make_unique<xdpu::Loop>();
      wayland = std::make_unique<xdpu::WaylandContext>(*loop);
      if (saved) {
        setenv("WAYLAND_SOCKET", saved->c_str(), 1);
      } else {
        unsetenv("WAYLAND_SOCKET");
      }
      require(wayland->connected(), "private Wayland connection failed");
    }

    ~Fixture() {
      wayland.reset();
      loop.reset();
      try {
        (void)send({{"op", "quit"}});
      } catch (...) {
      }
      close(m_control);
      int status = 0;
      while (waitpid(m_child, &status, 0) < 0 && errno == EINTR) {
      }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Json send(const Json& command) const {
      writeLine(m_control, command);
      std::string line;
      while (true) {
        pollfd ready{m_control, POLLIN, 0};
        require(poll(&ready, 1, 2000) > 0, "private server control timed out");
        char character = 0;
        require(read(m_control, &character, 1) == 1, "private server exited unexpectedly");
        if (character == '\n') {
          return Json::parse(line);
        }
        line += character;
      }
    }
    void sync() const {
      wayland->roundtrip();
      wayland->roundtrip();
    }

  private:
    int m_control = -1;
    pid_t m_child = -1;
  };

} // namespace shortcut_test
