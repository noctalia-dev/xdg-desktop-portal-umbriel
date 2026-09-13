#include "dbus/session.h"

#include "loop/loop.h"
#include "pipewire/pipewire.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <pipewire/stream.h>
#include <sdbus-c++/sdbus-c++.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <utility>

namespace xdpu {

  namespace {

    constexpr char kSessionInterface[] = "org.freedesktop.impl.portal.Session";
    constexpr uint64_t kNsecPerSec = 1000000000ull;
    // Two capture sessions per source keep a copy request pending at every
    // compositor frame boundary; ext_image_copy_capture_v1 allows at most one
    // live frame per session, so the sessions hand off instead of pipelining.
    constexpr int kCaptureSlots = 2;

    uint32_t sourceType(Session::SourceKind kind) { return kind == Session::SourceKind::Window ? 2u : 1u; }

    std::string kindName(Session::SourceKind kind) {
      return kind == Session::SourceKind::Window ? "window" : "monitor";
    }

  } // namespace

  struct Session::Impl {
    struct StreamState : public std::enable_shared_from_this<StreamState> {
      Loop* loop = nullptr;
      WaylandContext* wayland = nullptr;
      std::unique_ptr<WaylandContext::CaptureSession> capture;
      std::unique_ptr<WaylandContext::CaptureSession> twinCapture;
      std::unique_ptr<PipeWireStream> stream;
      CaptureConstraints constraints;
      Selection selection;
      ClosedHandler backendClosedHandler;
      uint32_t maxFps = 0;
      int fpsTimer = 0;
      bool stopped = false;
      int framesInFlight = 0;
      bool constraintsDirty = false;
      bool waitingForConstraints = false;
      bool reconfiguring = false;
      CaptureConstraints reconfigureTarget;
      std::array<std::unique_ptr<WaylandContext::CaptureFrame>, kCaptureSlots> pendingFrame;
      uint64_t sequence = 0;
      std::chrono::steady_clock::time_point lastFrame{};

      ~StreamState() { stop(); }

      int maxInFlight() const { return twinCapture ? kCaptureSlots : 1; }

      StreamResult result() const {
        StreamResult value;
        value.nodeId = stream ? stream->nodeId() : 0;
        value.sourceType = sourceType(selection.kind);
        value.x = selection.x;
        value.y = selection.y;
        value.width = selection.width > 0 ? selection.width : static_cast<int32_t>(constraints.bufferWidth);
        value.height = selection.height > 0 ? selection.height : static_cast<int32_t>(constraints.bufferHeight);
        value.mappingId = selection.kind == SourceKind::Window ? selection.appId : selection.output;
        return value;
      }

      void stop() {
        if (stopped) {
          return;
        }
        stopped = true;
        if (fpsTimer != 0 && loop != nullptr) {
          const int timer = fpsTimer;
          fpsTimer = 0;
          loop->removeTimer(timer);
        }
        // Destroy the pending frame first — their proxies must be gone before
        // the capture session or stream buffers they references.
        for (auto& frame : pendingFrame) {
          frame.reset();
        }
        if (stream) {
          stream->onProcessRequest = nullptr;
          stream->onAddBuffer = nullptr;
          stream->onRemoveBuffer = nullptr;
          stream->disconnect();
        }
        framesInFlight = 0;
      }

      void captureStopped() {
        if (stopped) {
          return;
        }
        stop();
        if (backendClosedHandler) {
          auto handler = std::move(backendClosedHandler);
          handler();
        }
      }

      // The twin only paces frames; losing it degrades to single-session
      // pacing instead of ending the stream.  With a frame still live on it,
      // frameFailed finishes the teardown once that frame settles.
      void twinStopped() {
        if (stopped || !twinCapture || pendingFrame[1] != nullptr) {
          return;
        }
        twinCapture.reset();
        std::fprintf(stderr, "session: twin capture stopped early, single-session pacing\n");
        processRequest();
      }

      void scheduleProcess(int delayMs) {
        if (fpsTimer != 0 || loop == nullptr) {
          return;
        }
        std::weak_ptr<StreamState> weak = shared_from_this();
        fpsTimer = loop->addTimer(delayMs, [weak]() {
          if (auto self = weak.lock()) {
            self->fpsTimer = 0;
            self->processRequest();
          }
        });
      }

      void processRequest() {
        if (stopped || !stream || !stream->connected() || waitingForConstraints || reconfiguring) {
          return;
        }
        if (constraintsDirty) {
          // reconfigureStream() no-ops while frames are in flight; the last
          // frameReady re-enters here once they drain.
          reconfigureStream();
          return;
        }
        if (framesInFlight >= maxInFlight()) {
          return;
        }

        if (maxFps > 0) {
          const auto now = std::chrono::steady_clock::now();
          const auto minInterval = std::chrono::microseconds(1000000 / std::max<uint32_t>(maxFps, 1));
          if (lastFrame.time_since_epoch().count() != 0 && now - lastFrame < minInterval) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(minInterval - (now - lastFrame));
            scheduleProcess(static_cast<int>(std::max<int64_t>(1, remaining.count())));
            return;
          }
        }

        requestFrame();
      }

      void constraintsChanged(const CaptureConstraints& newConstraints) {
        if (stopped) {
          return;
        }
        // Every done event settles a pending constraints-wait — dropping a
        // redundant one here would leave the stream waiting forever.
        waitingForConstraints = false;
        if (newConstraints == constraints) {
          scheduleProcess(1);
          return;
        }
        constraints = newConstraints;
        constraintsDirty = true;
        if (framesInFlight == 0 && !reconfiguring) {
          reconfigureStream();
        }
      }

      void reconfigureStream() {
        if (stopped || !stream || framesInFlight > 0 || reconfiguring || !constraintsDirty) {
          return;
        }

        constraintsDirty = false;
        reconfiguring = true;
        reconfigureTarget = constraints;
        if (!stream->reconfigure(reconfigureTarget)) {
          std::fprintf(stderr, "session: unable to reconfigure capture stream %u\n", stream->nodeId());
          stop();
          if (backendClosedHandler) {
            backendClosedHandler();
          }
        }
      }

      void bufferAdded(pw_buffer* buffer) {
        if (stopped || !reconfiguring || !stream) {
          return;
        }
        const CaptureBuffer* added = stream->captureBuffer(buffer);
        if (added == nullptr
            || added->width != reconfigureTarget.bufferWidth
            || added->height != reconfigureTarget.bufferHeight) {
          return;
        }
        reconfiguring = false;
        if (constraintsDirty) {
          reconfigureStream();
        } else {
          scheduleProcess(1);
        }
      }

      [[nodiscard]] int freeSlots() const {
        for (int slot = 0; slot < maxInFlight(); ++slot) {
          if (pendingFrame[slot] == nullptr) {
            return slot;
          }
        }
        return -1;
      }

      void requestFrame() {
        if (stopped || !stream || !stream->connected() || !capture || wayland == nullptr) {
          return;
        }
        const int slot = freeSlots();
        if (slot < 0) {
          return;
        }

        pw_buffer* pwBuffer = stream->dequeueBuffer();
        if (pwBuffer == nullptr) {
          scheduleProcess(1);
          return;
        }

        CaptureBuffer* captureBuffer = stream->captureBuffer(pwBuffer);
        if (captureBuffer == nullptr || captureBuffer->wlBuffer == nullptr) {
          stream->queueBuffer(pwBuffer);
          scheduleProcess(1);
          return;
        }

        wayland->requestCursorFrame(*capture);

        ++framesInFlight;
        std::weak_ptr<StreamState> weak = shared_from_this();
        auto frame = wayland->captureFrame(
            slot == 1 && twinCapture ? *twinCapture : *capture, captureBuffer->wlBuffer,
            [weak, slot, pwBuffer](CaptureBuffer&, uint64_t sec, uint32_t nsec) {
              if (auto self = weak.lock()) {
                self->frameReady(slot, pwBuffer, sec, nsec);
              }
            },
            [weak, slot, pwBuffer](CaptureFailureReason reason) {
              if (auto self = weak.lock()) {
                self->frameFailed(slot, pwBuffer, reason);
              }
            }
        );
        // A synchronous failure has already run frameFailed, which balanced
        // framesInFlight; only take ownership of a real frame.
        if (frame) {
          pendingFrame[slot] = std::move(frame);
        }
      }

      void frameReady(int slot, pw_buffer* pwBuffer, uint64_t sec, uint32_t nsec) {
        pendingFrame[slot].reset(); // frame proxy already destroyed by the callback
        --framesInFlight;
        // A stop can race with the Wayland ready event.  Never return a
        // buffer to a PipeWire stream after it has been disconnected.
        if (stopped || !stream || !stream->connected() || !stream->ownsBuffer(pwBuffer)) {
          return;
        }

        if (pwBuffer != nullptr && pwBuffer->buffer != nullptr) {
          auto* header = static_cast<spa_meta_header*>(
              spa_buffer_find_meta_data(pwBuffer->buffer, SPA_META_Header, sizeof(spa_meta_header))
          );
          if (header != nullptr) {
            header->flags = 0;
            header->offset = 0;
            header->pts = static_cast<int64_t>(sec * kNsecPerSec + nsec);
            header->dts_offset = 0;
            header->seq = ++sequence;
          }
          stream->setCursorMetadata(pwBuffer, capture->cursorMetadata());
        }

        lastFrame = std::chrono::steady_clock::now();
        stream->queueBuffer(pwBuffer);
        processRequest();
      }

      void frameFailed(int slot, pw_buffer* pwBuffer, CaptureFailureReason reason) {
        pendingFrame[slot].reset(); // frame proxy already destroyed by the callback
        --framesInFlight;
        if (!stopped && stream && stream->connected() && stream->ownsBuffer(pwBuffer)) {
          stream->queueBuffer(pwBuffer);
        }
        if (reason == CaptureFailureReason::Stopped) {
          if (slot == 1 && twinCapture) {
            twinStopped();
          } else {
            captureStopped();
          }
        } else if (reason == CaptureFailureReason::ConstraintsChanged) {
          if (constraintsDirty) {
            reconfigureStream();
          } else {
            waitingForConstraints = true;
          }
        } else {
          processRequest();
        }
      }
    };

    std::unique_ptr<sdbus::IObject> object;
    std::string path;
    ClosedHandler closedHandler;
    bool isClosed = false;
    uint32_t selectedSourceTypes = 1;
    bool allowMultiple = false;
    uint32_t selectedCursorMode = 1;
    uint32_t selectedPersistMode = 0;
    std::vector<Selection> restore;
    std::vector<Selection> currentSelections;
    std::vector<std::shared_ptr<StreamState>> streams;

    Impl(sdbus::IConnection& connection, std::string path, ClosedHandler closedHandler)
        : path(std::move(path)), closedHandler(std::move(closedHandler)) {
      object = sdbus::createObject(connection, sdbus::ObjectPath{this->path});
      object
          ->addVTable(
              sdbus::registerMethod("Close").implementedAs([this]() { close(false); }),
              sdbus::registerSignal("Closed").withParameters(),
              sdbus::registerProperty("version").withGetter([]() { return uint32_t{1}; })
          )
          .forInterface(kSessionInterface);
    }

    void close(bool backend) {
      if (isClosed) {
        return;
      }
      isClosed = true;
      clearStreams();

      if (backend) {
        try {
          object->emitSignal("Closed").onInterface(kSessionInterface).withArguments();
        } catch (const std::exception& error) {
          std::fprintf(stderr, "session: failed to emit Closed for %s: %s\n", path.c_str(), error.what());
        }
      }

      auto handler = closedHandler;
      if (handler) {
        handler();
      }
    }

    void clearStreams() {
      for (auto& stream : streams) {
        if (stream) {
          stream->stop();
        }
      }
      streams.clear();
    }
  };

  Session::Session(sdbus::IConnection& connection, std::string path, ClosedHandler closedHandler)
      : m_impl(std::make_unique<Impl>(connection, std::move(path), std::move(closedHandler))) {}

  Session::~Session() = default;

  const std::string& Session::path() const { return m_impl->path; }

  bool Session::closed() const { return m_impl->isClosed; }

  void Session::setSelectionOptions(
      uint32_t sourceTypes, bool multiple, uint32_t cursorMode, uint32_t persistMode,
      std::vector<Selection> restoreSelections
  ) {
    m_impl->selectedSourceTypes = (sourceTypes & 0x3u) == 0 ? 1u : (sourceTypes & 0x3u);
    m_impl->allowMultiple = multiple;
    m_impl->selectedCursorMode = cursorMode;
    m_impl->selectedPersistMode = std::min<uint32_t>(persistMode, 2);
    m_impl->restore = std::move(restoreSelections);
  }

  uint32_t Session::sourceTypes() const { return m_impl->selectedSourceTypes; }

  bool Session::multiple() const { return m_impl->allowMultiple; }

  uint32_t Session::cursorMode() const { return m_impl->selectedCursorMode; }

  uint32_t Session::persistMode() const { return m_impl->selectedPersistMode; }

  const std::vector<Session::Selection>& Session::restoreSelections() const { return m_impl->restore; }

  void Session::setSelections(std::vector<Selection> selections) { m_impl->currentSelections = std::move(selections); }

  const std::vector<Session::Selection>& Session::selections() const { return m_impl->currentSelections; }

  bool Session::addStream(
      Loop& loop, WaylandContext& wayland, std::unique_ptr<WaylandContext::CaptureSession> capture,
      CaptureCursorMode cursorMode, std::unique_ptr<PipeWireStream> stream, const CaptureConstraints& constraints,
      const Selection& selection, uint32_t maxFps, ClosedHandler backendClosedHandler
  ) {
    if (!capture || !stream || capture->stopped) {
      return false;
    }

    auto state = std::make_shared<Impl::StreamState>();
    state->loop = &loop;
    state->wayland = &wayland;
    state->capture = std::move(capture);
    state->stream = std::move(stream);
    state->constraints = constraints;
    state->selection = selection;
    state->maxFps = maxFps;
    state->backendClosedHandler = std::move(backendClosedHandler);

    // A second session for the same source keeps a copy request pending at
    // every compositor frame boundary (see kCaptureSlots).  Its constraints
    // are ignored — the stream is shaped by the primary's.
    if (selection.kind == Session::SourceKind::Monitor) {
      state->twinCapture = wayland.createOutputCapture(selection.output, cursorMode, nullptr);
    } else {
      state->twinCapture = wayland.createToplevelCapture(selection.identifier, cursorMode, nullptr);
    }
    if (!state->twinCapture || state->twinCapture->stopped) {
      state->twinCapture.reset();
      std::fprintf(stderr, "session: twin capture unavailable, single-session pacing\n");
    }

    std::weak_ptr<Impl::StreamState> weak = state;
    for (auto* session : {&state->capture, &state->twinCapture}) {
      if (*session == nullptr) {
        continue;
      }
      const bool isTwin = session == &state->twinCapture;
      (*session)->constraintsCb = [weak](const CaptureConstraints& newConstraints) {
        if (auto streamState = weak.lock()) {
          streamState->constraintsChanged(newConstraints);
        }
      };
      (*session)->stoppedCb = [weak, isTwin]() {
        if (auto streamState = weak.lock()) {
          if (isTwin) {
            streamState->twinStopped();
          } else {
            streamState->captureStopped();
          }
        }
      };
    }
    state->stream->onProcessRequest = [weak]() {
      if (auto streamState = weak.lock()) {
        streamState->processRequest();
      }
    };
    state->stream->onAddBuffer = [weak](pw_buffer* buffer) {
      if (auto streamState = weak.lock()) {
        streamState->bufferAdded(buffer);
      }
    };
    state->stream->onRemoveBuffer = [](pw_buffer*) {};
    // createStream() may reach STREAMING before these callbacks are installed.
    // Start capture directly so the first buffer exists before graph scheduling.
    state->processRequest();

    m_impl->streams.push_back(std::move(state));
    return true;
  }

  std::vector<Session::StreamResult> Session::streamResults() const {
    std::vector<StreamResult> results;
    results.reserve(m_impl->streams.size());
    for (const auto& stream : m_impl->streams) {
      if (stream) {
        results.push_back(stream->result());
      }
    }
    return results;
  }

  sdbus::Variant Session::restoreDataVariant(const std::string& token) const {
    std::vector<PortalResults> entries;
    entries.reserve(m_impl->currentSelections.size());
    for (const Selection& selection : m_impl->currentSelections) {
      PortalResults entry;
      entry.emplace("kind", sdbus::Variant{kindName(selection.kind)});
      entry.emplace("title", sdbus::Variant{selection.title});
      entry.emplace("token", sdbus::Variant{token});
      if (selection.kind == SourceKind::Monitor) {
        entry.emplace("output", sdbus::Variant{selection.output});
      } else {
        entry.emplace("app_id", sdbus::Variant{selection.appId});
      }
      entries.push_back(std::move(entry));
    }

    return sdbus::Variant{
        sdbus::Struct<std::string, uint32_t, sdbus::Variant>{"umbriel", uint32_t{1}, sdbus::Variant{entries}}
    };
  }

  void Session::clearStreams() { m_impl->clearStreams(); }

  void Session::close() { m_impl->close(false); }

  void Session::closeByBackend() { m_impl->close(true); }

} // namespace xdpu
