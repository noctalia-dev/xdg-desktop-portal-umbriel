#pragma once

#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>

namespace xdpu {

  // Tracks the xdg-desktop-portal frontend's unique name; impl.portal methods accept calls only from it.
  class Frontend {
  public:
    // Called for every NameOwnerChanged after the tracked owner is updated; frontendChanged is true when the
    // frontend itself changed owner.
    using Handler = std::function<void(const std::string& name, const std::string& owner, bool frontendChanged)>;

    explicit Frontend(sdbus::IConnection& connection, Handler handler = {}) : m_connection(connection) {
      m_bus = sdbus::createProxy(
          connection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
      );
      m_bus->uponSignal("NameOwnerChanged")
          .onInterface("org.freedesktop.DBus")
          .call([this, handler = std::move(handler)](
                    const std::string& name, const std::string&, const std::string& owner
                ) {
            const bool changed = name == kName && owner != m_owner;
            if (changed) {
              m_owner = owner;
            }
            if (handler) {
              handler(name, owner, changed);
            }
          });
      try {
        m_bus->callMethod("GetNameOwner")
            .onInterface("org.freedesktop.DBus")
            .withArguments(std::string{kName})
            .storeResultsTo(m_owner);
      } catch (const sdbus::Error&) {
        m_owner.clear();
      }
    }

    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;

    std::string caller() const {
      const auto message = m_connection.getCurrentlyProcessedMessage();
      const char* sender = message.getSender();
      return sender != nullptr ? sender : "";
    }

    void require(const char* what) const {
      if (m_owner.empty() || caller() != m_owner) {
        throw sdbus::Error{
            sdbus::Error::Name{"org.freedesktop.portal.Error.NotAllowed"},
            std::string{"Only the desktop portal frontend may control "} + what
        };
      }
    }

  private:
    static constexpr char kName[] = "org.freedesktop.portal.Desktop";

    sdbus::IConnection& m_connection;
    std::string m_owner;
    std::unique_ptr<sdbus::IProxy> m_bus;
  };

} // namespace xdpu
