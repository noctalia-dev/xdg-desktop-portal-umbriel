// Drives the real portal frontend and keeps its D-Bus session and EI receiver
// alive together. Stdin commands let an isolated compositor supply physical input.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <libei.h>
#include <map>
#include <memory>
#include <poll.h>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace {
  constexpr char kInterface[] = "org.freedesktop.portal.InputCapture";
  using Dict = std::map<std::string, sdbus::Variant>;
  using Zone = sdbus::Struct<uint32_t, uint32_t, int32_t, int32_t>;

  class Client {
  public:
    Client() {
      m_bus = sdbus::createSessionBusConnection();
      m_portal = sdbus::createProxy(
          *m_bus, sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
          sdbus::ObjectPath{"/org/freedesktop/portal/desktop"}
      );
      m_portal->uponSignal("Activated").onInterface(kInterface).call([this](sdbus::ObjectPath session, Dict options) {
        if (session != m_session)
          return;
        m_activation = options.at("activation_id").get<uint32_t>();
        const uint32_t barrier = options.contains("barrier_id") ? options.at("barrier_id").get<uint32_t>() : 0;
        std::println("activated id={} barrier={}", m_activation, barrier);
      });
      m_portal->uponSignal("Deactivated").onInterface(kInterface).call([this](sdbus::ObjectPath session, Dict options) {
        if (session == m_session)
          std::println("deactivated id={}", options.at("activation_id").get<uint32_t>());
      });
      m_portal->uponSignal("Disabled").onInterface(kInterface).call([this](sdbus::ObjectPath session, Dict) {
        if (session == m_session)
          std::puts("disabled");
      });
      m_portal->uponSignal("ZonesChanged")
          .onInterface(kInterface)
          .call([this](sdbus::ObjectPath session, Dict options) {
            if (session == m_session)
              std::println("zones-changed invalidated={}", options.at("zone_set").get<uint32_t>());
          });
      sdbus::Variant version;
      m_portal->callMethod("Get")
          .onInterface("org.freedesktop.DBus.Properties")
          .withArguments(std::string(kInterface), std::string("version"))
          .storeResultsTo(version);
      sdbus::Variant capabilities;
      m_portal->callMethod("Get")
          .onInterface("org.freedesktop.DBus.Properties")
          .withArguments(std::string(kInterface), std::string("SupportedCapabilities"))
          .storeResultsTo(capabilities);
      if (version.get<uint32_t>() != 2 || capabilities.get<uint32_t>() != 3) {
        throw std::runtime_error("portal must advertise InputCapture v2 with keyboard and pointer only");
      }
      std::puts("properties version=2 capabilities=3");
      Dict results;
      const Dict options{{"session_handle_token", sdbus::Variant(std::string("input_capture_test"))}};
      m_portal->callMethod("CreateSession2").onInterface(kInterface).withArguments(options).storeResultsTo(results);
      m_session = results.at("session_handle").get<sdbus::ObjectPath>();
      m_sessionProxy = sdbus::createProxy(*m_bus, sdbus::ServiceName{"org.freedesktop.portal.Desktop"}, m_session);
      m_sessionProxy->uponSignal("Closed").onInterface("org.freedesktop.portal.Session").call([](Dict) {
        std::puts("session-closed");
      });
      std::string sender = m_bus->getUniqueName();
      if (!sender.empty() && sender.front() == ':')
        sender.erase(0, 1);
      std::ranges::replace(sender, '.', '_');
      m_requestPrefix = "/org/freedesktop/portal/desktop/request/" + sender + "/";
      std::println("ready session={}", m_session.c_str());
    }

    ~Client() {
      if (m_receiver != nullptr)
        ei_unref(m_receiver);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void run() {
      const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
      if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("cannot make command input nonblocking");
      }
      while (!m_done) {
        while (m_bus->processPendingEvent()) {
        }
        drainEi();
        const auto busPoll = m_bus->getEventLoopPollData();
        pollfd fds[]{
            {.fd = busPoll.fd, .events = busPoll.events, .revents = 0},
            {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
            {.fd = m_receiver != nullptr ? ei_get_fd(m_receiver) : -1, .events = POLLIN, .revents = 0}
        };
        if (poll(fds, 3, 100) < 0 && errno != EINTR)
          throw std::runtime_error("poll failed");
        if ((fds[1].revents & POLLIN) != 0) {
          char bytes[2048];
          const ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
          if (count > 0)
            m_input.append(bytes, static_cast<size_t>(count));
          else if (count == 0)
            m_done = true;
          size_t newline = 0;
          while ((newline = m_input.find('\n')) != std::string::npos) {
            const std::string line = m_input.substr(0, newline);
            m_input.erase(0, newline + 1);
            command(line);
          }
        }
      }
    }

  private:
    void request(const std::string& method, const std::function<void(const Dict&, sdbus::ObjectPath&)>& invoke) {
      const std::string token = "capture_request_" + std::to_string(++m_serial);
      const sdbus::ObjectPath expected{m_requestPrefix + token};
      auto proxy = sdbus::createProxy(*m_bus, sdbus::ServiceName{"org.freedesktop.portal.Desktop"}, expected);
      proxy->uponSignal("Response")
          .onInterface("org.freedesktop.portal.Request")
          .call([this, method](uint32_t status, Dict results) {
            std::println("response {} status={}", method, status);
            if (status != 0)
              return;
            if (method == "Start") {
              const auto capabilities = results.at("capabilities").get<uint32_t>();
              std::println(
                  "started capabilities={} restore={} clipboard={}", capabilities,
                  results.contains("restore_token") ? 1U : 0U,
                  results.contains("clipboard_enabled") && results.at("clipboard_enabled").get<bool>() ? 1U : 0U
              );
            } else if (method == "GetZones") {
              m_zoneSet = results.at("zone_set").get<uint32_t>();
              const auto zones = results.at("zones").get<std::vector<Zone>>();
              std::println("zones zone_set={} count={}", m_zoneSet, zones.size());
              for (const auto& zone : zones) {
                std::println(
                    "zone x={} y={} width={} height={}", zone.get<2>(), zone.get<3>(), zone.get<0>(), zone.get<1>()
                );
              }
            } else if (method == "SetPointerBarriers") {
              const auto failed = results.at("failed_barriers").get<std::vector<uint32_t>>();
              std::println("barriers failed={}", failed.size());
              for (const auto id : failed)
                std::println("failed-barrier id={}", id);
            }
          });
      m_requests.push_back(std::move(proxy));
      const Dict options{{"handle_token", sdbus::Variant(token)}};
      sdbus::ObjectPath returned;
      invoke(options, returned);
      if (returned != expected)
        throw std::runtime_error("frontend ignored request handle token");
      std::println("requested {}", method);
    }

    void command(const std::string& line) {
      std::istringstream input(line);
      std::string action;
      input >> action;
      if (action == "start") {
        uint32_t capabilities = 3;
        input >> capabilities;
        request("Start", [&](const Dict& base, sdbus::ObjectPath& handle) {
          Dict options = base;
          options.emplace("capabilities", sdbus::Variant(capabilities));
          options.emplace("persist_mode", sdbus::Variant(uint32_t{2}));
          m_portal->callMethod("Start")
              .onInterface(kInterface)
              .withArguments(m_session, std::string{}, options)
              .storeResultsTo(handle);
        });
      } else if (action == "zones") {
        request("GetZones", [&](const Dict& options, sdbus::ObjectPath& handle) {
          m_portal->callMethod("GetZones")
              .onInterface(kInterface)
              .withArguments(m_session, options)
              .storeResultsTo(handle);
        });
      } else if (action == "barrier") {
        uint32_t id = 1;
        int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!(input >> id >> x1 >> y1 >> x2 >> y2))
          throw std::runtime_error("barrier requires id and four coordinates");
        uint32_t zoneSet = m_zoneSet;
        input >> zoneSet;
        request("SetPointerBarriers", [&](const Dict& options, sdbus::ObjectPath& handle) {
          const std::vector<Dict> barriers{
              {{"barrier_id", sdbus::Variant(id)},
               {"position", sdbus::Variant(sdbus::Struct<int32_t, int32_t, int32_t, int32_t>{x1, y1, x2, y2})}}
          };
          m_portal->callMethod("SetPointerBarriers")
              .onInterface(kInterface)
              .withArguments(m_session, options, barriers, zoneSet)
              .storeResultsTo(handle);
        });
      } else if (action == "connect") {
        sdbus::UnixFd fd;
        m_portal->callMethod("ConnectToEIS")
            .onInterface(kInterface)
            .withArguments(m_session, Dict{})
            .storeResultsTo(fd);
        if (m_receiver != nullptr)
          ei_unref(m_receiver);
        m_receiver = ei_new_receiver(nullptr);
        ei_configure_name(m_receiver, "Umbriel portal input capture test");
        const int ownedFd = dup(fd.get());
        if (ownedFd < 0 || ei_setup_backend_fd(m_receiver, ownedFd) != 0)
          throw std::runtime_error("EI fd setup failed");
        std::puts("eis-connected-fd");
      } else if (action == "enable" || action == "disable") {
        m_portal->callMethod(action == "enable" ? "Enable" : "Disable")
            .onInterface(kInterface)
            .withArguments(m_session, Dict{});
        std::println("requested {}", action);
      } else if (action == "release") {
        uint32_t activation = m_activation;
        double x = 400, y = 300;
        input >> activation >> x >> y;
        const Dict options{
            {"activation_id", sdbus::Variant(activation)},
            {"cursor_position", sdbus::Variant(sdbus::Struct<double, double>{x, y})}
        };
        m_portal->callMethod("Release").onInterface(kInterface).withArguments(m_session, options);
        std::println("requested release id={}", activation);
      } else if (action == "close") {
        m_sessionProxy->callMethod("Close").onInterface("org.freedesktop.portal.Session");
        std::puts("requested close");
      } else if (action == "cancel") {
        if (m_requests.empty())
          throw std::runtime_error("no request to cancel");
        m_requests.back()->callMethod("Close").onInterface("org.freedesktop.portal.Request");
        std::puts("requested cancel");
      } else if (action == "attack-cancel") {
        if (m_requests.empty())
          throw std::runtime_error("no request to attack");
        auto other = sdbus::createSessionBusConnection();
        auto requestProxy = sdbus::createProxy(
            *other, sdbus::ServiceName{"org.freedesktop.impl.portal.desktop.umbriel"},
            m_requests.back()->getObjectPath()
        );
        bool rejected = false;
        try {
          requestProxy->callMethod("Close").onInterface("org.freedesktop.impl.portal.Request");
        } catch (const sdbus::Error& error) {
          rejected = error.getName() == "org.freedesktop.portal.Error.NotAllowed";
          if (!rejected)
            throw;
        }
        if (!rejected)
          throw std::runtime_error("untrusted client cancelled a pending capture request");
        std::puts("untrusted-cancel rejected");
      } else if (action == "mark") {
        std::string marker;
        input >> marker;
        std::println("mark {}", marker);
      } else if (action == "quit") {
        m_done = true;
      } else {
        throw std::runtime_error("unknown command: " + line);
      }
    }

    void drainEi() {
      if (m_receiver == nullptr)
        return;
      ei_dispatch(m_receiver);
      while (ei_event* event = ei_get_event(m_receiver)) {
        switch (ei_event_get_type(event)) {
        case EI_EVENT_CONNECT:
          std::puts("ei-connected");
          break;
        case EI_EVENT_DISCONNECT:
          std::puts("ei-disconnected");
          break;
        case EI_EVENT_SEAT_ADDED:
          ei_seat_bind_capabilities(
              ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON,
              EI_DEVICE_CAP_SCROLL, nullptr
          );
          break;
        case EI_EVENT_DEVICE_RESUMED:
          std::puts("ei-device-ready");
          break;
        case EI_EVENT_DEVICE_START_EMULATING:
          std::println("ei-start id={}", ei_event_emulating_get_sequence(event));
          break;
        case EI_EVENT_DEVICE_STOP_EMULATING:
          std::puts("ei-stop");
          break;
        case EI_EVENT_KEYBOARD_KEY:
          std::println(
              "ei-key code={} state={}", ei_event_keyboard_get_key(event),
              ei_event_keyboard_get_key_is_press(event) ? "pressed" : "released"
          );
          break;
        case EI_EVENT_POINTER_MOTION:
          std::println("ei-motion dx={:.3f} dy={:.3f}", ei_event_pointer_get_dx(event), ei_event_pointer_get_dy(event));
          break;
        case EI_EVENT_BUTTON_BUTTON:
          std::println(
              "ei-button code={} state={}", ei_event_button_get_button(event),
              ei_event_button_get_is_press(event) ? "pressed" : "released"
          );
          break;
        case EI_EVENT_SCROLL_DELTA:
          std::println("ei-scroll x={:.3f} y={:.3f}", ei_event_scroll_get_dx(event), ei_event_scroll_get_dy(event));
          break;
        case EI_EVENT_SCROLL_DISCRETE:
          std::println(
              "ei-discrete x={} y={}", ei_event_scroll_get_discrete_dx(event), ei_event_scroll_get_discrete_dy(event)
          );
          break;
        case EI_EVENT_FRAME:
          std::puts("ei-frame");
          break;
        default:
          break;
        }
        ei_event_unref(event);
      }
    }

    std::unique_ptr<sdbus::IConnection> m_bus;
    std::unique_ptr<sdbus::IProxy> m_portal;
    std::unique_ptr<sdbus::IProxy> m_sessionProxy;
    std::vector<std::unique_ptr<sdbus::IProxy>> m_requests;
    sdbus::ObjectPath m_session;
    std::string m_requestPrefix;
    std::string m_input;
    ei* m_receiver = nullptr;
    uint32_t m_serial = 0;
    uint32_t m_zoneSet = 0;
    uint32_t m_activation = 0;
    bool m_done = false;
  };
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  try {
    Client client;
    client.run();
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "input-capture-test-client: {}", error.what());
    return 1;
  }
}
