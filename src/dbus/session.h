#pragma once

#include "dbus/request.h"
#include "wayland/wayland.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xdpu {

  class Loop;
  class PipeWireStream;

  class Session {
  public:
    enum class SourceKind : uint32_t {
      Monitor = 1,
      Window = 2,
    };

    struct Selection {
      SourceKind kind = SourceKind::Monitor;
      std::string output;
      std::string identifier;
      std::string appId;
      std::string title;
      int32_t x = 0;
      int32_t y = 0;
      int32_t width = 0;
      int32_t height = 0;
    };

    struct StreamResult {
      uint32_t nodeId = 0;
      uint32_t sourceType = 1;
      int32_t x = 0;
      int32_t y = 0;
      int32_t width = 0;
      int32_t height = 0;
      std::string mappingId;
    };

    using ClosedHandler = std::function<void()>;

    Session(sdbus::IConnection& connection, std::string path, ClosedHandler closedHandler);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    const std::string& path() const;
    bool closed() const;

    void setSelectionOptions(
        uint32_t sourceTypes, bool multiple, uint32_t cursorMode, uint32_t persistMode,
        std::vector<Selection> restoreSelections
    );
    uint32_t sourceTypes() const;
    bool multiple() const;
    uint32_t cursorMode() const;
    uint32_t persistMode() const;
    const std::vector<Selection>& restoreSelections() const;

    void setSelections(std::vector<Selection> selections);
    const std::vector<Selection>& selections() const;

    bool addStream(
        Loop& loop, WaylandContext& wayland, std::unique_ptr<WaylandContext::CaptureSession> capture,
        std::unique_ptr<PipeWireStream> stream, const CaptureConstraints& constraints, const Selection& selection,
        uint32_t maxFps, ClosedHandler backendClosedHandler
    );
    std::vector<StreamResult> streamResults() const;

    sdbus::Variant restoreDataVariant(const std::string& token) const;

    void clearStreams();
    void close();
    void closeByBackend();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
