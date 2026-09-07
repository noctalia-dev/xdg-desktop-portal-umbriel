#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

struct wl_buffer;
struct wl_display;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;

struct ext_image_copy_capture_cursor_session_v1;
namespace xdpu {

  class Loop;

  struct OutputInfo {
    std::string name;
    std::string description;
    int32_t width = 0;
    int32_t height = 0;
    int32_t x = 0;
    int32_t y = 0;
  };

  struct ToplevelInfo {
    std::string identifier;
    std::string appId;
    std::string title;
  };

  struct CaptureBuffer {
    struct wl_buffer* wlBuffer = nullptr;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t transform = 0;
    void* data = nullptr;
    size_t size = 0;
  };
  struct DmabufPlane {
    int fd = -1;
    uint32_t stride = 0;
    uint32_t offset = 0;
  };

  struct CaptureConstraints {
    uint32_t bufferWidth = 0;
    uint32_t bufferHeight = 0;
    std::vector<uint32_t> shmFormats; // DRM fourcc, converted from wl_shm.format on receipt
    dev_t dmabufDevice = 0;

    struct DmabufFormat {
      uint32_t format = 0;
      uint64_t modifier = 0;
    };

    std::vector<DmabufFormat> dmabufFormats;
  };

  enum class CaptureCursorMode {
    Hidden,
    Embedded,
    Metadata,
  };

  struct CursorMetadata {
    bool visible = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t hotspotX = 0;
    int32_t hotspotY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    std::vector<uint8_t> pixels;
  };

  using ConstraintsCallback = std::function<void(const CaptureConstraints&)>;
  using FrameReadyCallback =
      std::function<void(CaptureBuffer& buf, uint64_t presentationTimeSec, uint32_t presentationTimeNsec)>;
  enum class CaptureFailureReason {
    Retry,
    ConstraintsChanged,
    Stopped,
  };

  using FrameFailedCallback = std::function<void(CaptureFailureReason reason)>;
  using CaptureStoppedCallback = std::function<void()>;

  class WaylandContext {
  public:
    explicit WaylandContext(Loop& loop);
    ~WaylandContext();

    WaylandContext(const WaylandContext&) = delete;
    WaylandContext& operator=(const WaylandContext&) = delete;

    wl_display* display() const;
    bool connected() const;
    const std::vector<OutputInfo>& outputs() const;
    const std::vector<ToplevelInfo>& toplevels() const;

    std::string buildChooserJson(uint32_t sourceTypes, bool multiple) const;

    struct Impl;
    struct CursorCapture;
    struct CaptureSession {
      CaptureSession(
          Impl& impl, ext_image_capture_source_v1* source, ext_image_copy_capture_session_v1* session,
          ConstraintsCallback constraintsCb
      );
      ~CaptureSession();

      [[nodiscard]] bool hasCursorMetadata() const;
      [[nodiscard]] const CursorMetadata* cursorMetadata() const;

      Impl& impl;
      ext_image_capture_source_v1* source = nullptr;
      ext_image_copy_capture_session_v1* session = nullptr;
      std::unique_ptr<CursorCapture> cursor;
      CaptureConstraints constraints;
      CaptureConstraints pendingConstraints;
      ConstraintsCallback constraintsCb;
      CaptureStoppedCallback stoppedCb;
      bool stopped = false;
    };
    std::unique_ptr<CaptureSession>
    createOutputCapture(const std::string& outputName, CaptureCursorMode cursorMode, ConstraintsCallback constraintsCb);
    std::unique_ptr<CaptureSession> createToplevelCapture(
        const std::string& identifier, CaptureCursorMode cursorMode, ConstraintsCallback constraintsCb
    );
    struct CaptureFrame {
      ext_image_copy_capture_frame_v1* frame = nullptr;
      CaptureBuffer buffer;
      FrameReadyCallback onReady;
      FrameFailedCallback onFailed;
      uint64_t presentationSec = 0;
      uint32_t presentationNsec = 0;

      CaptureFrame() = default;
      ~CaptureFrame();
      CaptureFrame(const CaptureFrame&) = delete;
      CaptureFrame& operator=(const CaptureFrame&) = delete;
    };

    // Returns an owning frame handle.  Destroy it to cancel a pending capture.
    // Callbacks fire at most once; after that the frame proxy is already destroyed.
    std::unique_ptr<CaptureFrame> captureFrame(
        CaptureSession& session, struct wl_buffer* buffer, FrameReadyCallback onReady, FrameFailedCallback onFailed
    );
    void requestCursorFrame(CaptureSession& session);

    struct ScreenshotResult {
      std::vector<uint8_t> pixels;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t stride = 0;
      uint32_t format = 0;
    };

    struct wl_buffer* createDmabufBuffer(
        uint32_t width, uint32_t height, uint32_t format, uint64_t modifier, const std::vector<DmabufPlane>& planes
    );
    // `format` is a DRM fourcc; the wl_shm.format value is derived internally.
    struct wl_buffer*
    createShmBuffer(uint32_t width, uint32_t height, uint32_t format, uint32_t stride, int fd, size_t size);

    void roundtrip();
    void flush();

    static WaylandContext* defaultContext();
    static uint32_t preferredShmFormat(const std::vector<uint32_t>& formats);

  private:
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
