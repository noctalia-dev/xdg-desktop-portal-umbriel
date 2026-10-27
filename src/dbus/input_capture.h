#pragma once

#include <memory>

namespace sdbus {
  class IConnection;
  class IObject;
} // namespace sdbus

namespace xdpu {

  class Loop;
  class WaylandContext;

  // InputCapture v2 control and consent. Umbriel owns the EIS server and the
  // permission decision; this backend forwards the approved connection FD.
  class InputCapturePortal {
  public:
    InputCapturePortal(Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, WaylandContext& wayland);
    ~InputCapturePortal();

    InputCapturePortal(const InputCapturePortal&) = delete;
    InputCapturePortal& operator=(const InputCapturePortal&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
