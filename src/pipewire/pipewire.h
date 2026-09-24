#pragma once

#include "wayland/wayland.h"

#include <cstdint>
#include <functional>
#include <memory>

struct pw_buffer;
struct pw_loop;

namespace xdpu {

  class Loop;
  class PipeWireStream;

  class PipeWireContext {
  public:
    explicit PipeWireContext(Loop& loop);
    ~PipeWireContext();

    PipeWireContext(const PipeWireContext&) = delete;
    PipeWireContext& operator=(const PipeWireContext&) = delete;

    struct pw_loop* pwLoop() const;

    std::unique_ptr<PipeWireStream> createStream(
        uint32_t width, uint32_t height, const CaptureConstraints& constraints, uint32_t maxFps, bool cursorMetadata
    );
    [[nodiscard]] bool supportsCursorMetadata() const;
    // Process any pending PipeWire events (e.g. to resolve node IDs after stream connect).
    void processPending();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

  class PipeWireStream {
  public:
    struct Impl;

    ~PipeWireStream();

    PipeWireStream(const PipeWireStream&) = delete;
    PipeWireStream& operator=(const PipeWireStream&) = delete;

    uint32_t nodeId() const;

    struct pw_buffer* dequeueBuffer();
    void queueBuffer(struct pw_buffer* buf);
    bool reconfigure(const CaptureConstraints& constraints);
    void disconnect();
    bool connected() const;
    // Unlike connected(), false while PAUSED (no user pulling frames).
    [[nodiscard]] bool streaming() const;
    void triggerProcess();

    CaptureBuffer* captureBuffer(struct pw_buffer* buffer) const;
    void setCursorMetadata(struct pw_buffer* buffer, const CursorMetadata* metadata);
    bool ownsBuffer(struct pw_buffer* buffer) const;
    Impl* implForCallbacks() const;

    std::function<void()> onProcessRequest;
    std::function<void(struct pw_buffer*)> onAddBuffer;
    std::function<void(struct pw_buffer*)> onRemoveBuffer;

  private:
    friend class PipeWireContext;
    explicit PipeWireStream(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
