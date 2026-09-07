#include "wayland/wayland.h"

#include "loop/loop.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <drm_fourcc.h>
#include <ext-foreign-toplevel-list-v1-client-protocol.h>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <fcntl.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <wayland-client.h>

namespace xdpu {

  namespace {

    constexpr uint32_t kSourceMonitor = 1;
    constexpr uint32_t kSourceWindow = 2;

    WaylandContext* g_defaultContext = nullptr;

  } // namespace

  struct WaylandContext::Impl {
    struct OutputState {
      wl_output* output = nullptr;
      OutputInfo info;
      uint32_t registryName = 0;
      bool hasName = false;
    };

    struct ToplevelState {
      ext_foreign_toplevel_handle_v1* handle = nullptr;
      ToplevelInfo info;
      ToplevelInfo pending;
      bool closed = false;
    };

    explicit Impl(WaylandContext& owner, Loop& loop) : owner(owner), loop(loop) {}

    WaylandContext& owner;
    Loop& loop;
    wl_display* display = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    uint32_t seatRegistryName = 0;
    wl_registry* registry = nullptr;
    wl_shm* shm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    ext_output_image_capture_source_manager_v1* outputSourceManager = nullptr;
    ext_foreign_toplevel_image_capture_source_manager_v1* toplevelSourceManager = nullptr;
    ext_image_copy_capture_manager_v1* captureManager = nullptr;
    ext_foreign_toplevel_list_v1* toplevelList = nullptr;

    int displayFdId = 0;
    bool preparedRead = false;
    bool wantsWrite = false;
    bool disconnected = false;
    uint32_t nextSyntheticOutput = 1;

    std::vector<OutputInfo> outputs;
    std::vector<ToplevelInfo> toplevels;
    std::unordered_map<wl_output*, std::unique_ptr<OutputState>> outputStates;
    std::unordered_map<ext_foreign_toplevel_handle_v1*, std::unique_ptr<ToplevelState>> toplevelStates;

    void rebuildOutputs() {
      outputs.clear();
      outputs.reserve(outputStates.size());
      for (const auto& [object, state] : outputStates) {
        (void)object;
        OutputInfo info = state->info;
        if (info.name.empty()) {
          info.name = "output-" + std::to_string(state->registryName == 0 ? nextSyntheticOutput : state->registryName);
        }
        outputs.push_back(std::move(info));
      }
      std::ranges::sort(outputs, {}, &OutputInfo::name);
    }

    void rebuildToplevels() {
      toplevels.clear();
      toplevels.reserve(toplevelStates.size());
      for (const auto& [handle, state] : toplevelStates) {
        (void)handle;
        if (!state->closed && !state->info.identifier.empty()) {
          toplevels.push_back(state->info);
        }
      }
      std::ranges::sort(toplevels, {}, &ToplevelInfo::title);
    }

    void cancelPreparedRead() {
      if (preparedRead) {
        wl_display_cancel_read(display);
        preparedRead = false;
      }
    }

    void markDisconnected() {
      if (disconnected) {
        return;
      }
      disconnected = true;
      cancelPreparedRead();
      fprintf(stderr, "wayland: compositor connection closed\n");
      loop.quit();
    }

    void updateDisplayFdEvents() {
      if (displayFdId == 0 || disconnected) {
        return;
      }
      uint32_t events = EPOLLIN | EPOLLERR | EPOLLHUP;
      if (wantsWrite) {
        events |= EPOLLOUT;
      }
      loop.updateFd(displayFdId, events);
    }

    void flushDisplay() {
      while (true) {
        const int rc = wl_display_flush(display);
        if (rc >= 0) {
          wantsWrite = false;
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          wantsWrite = true;
          break;
        }
        markDisconnected();
        break;
      }
      updateDisplayFdEvents();
    }

    void prepareRead() {
      if (disconnected || preparedRead) {
        return;
      }

      while (wl_display_prepare_read(display) < 0) {
        if (wl_display_dispatch_pending(display) < 0) {
          markDisconnected();
          return;
        }
      }
      preparedRead = true;
      flushDisplay();
    }

    void handleDisplayEvents(uint32_t events) {
      if (disconnected) {
        return;
      }

      if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
        markDisconnected();
        return;
      }

      if ((events & EPOLLOUT) != 0) {
        flushDisplay();
      }

      if ((events & EPOLLIN) != 0) {
        if (preparedRead) {
          if (wl_display_read_events(display) < 0) {
            preparedRead = false;
            markDisconnected();
            return;
          }
          preparedRead = false;
        }
        if (wl_display_dispatch_pending(display) < 0) {
          markDisconnected();
          return;
        }
      }

      prepareRead();
    }

    OutputState* findOutput(wl_output* output) const {
      const auto iter = outputStates.find(output);
      return iter == outputStates.end() ? nullptr : iter->second.get();
    }

    OutputState* findOutput(const std::string& name) const {
      for (const auto& [object, state] : outputStates) {
        (void)object;
        const std::string& stateName = state->info.name;
        if (stateName == name || (stateName.empty() && ("output-" + std::to_string(state->registryName)) == name)) {
          return state.get();
        }
      }
      return nullptr;
    }

    ToplevelState* findToplevel(const std::string& identifier) const {
      for (const auto& [handle, state] : toplevelStates) {
        (void)handle;
        if (!state->closed && state->info.identifier == identifier) {
          return state.get();
        }
      }
      return nullptr;
    }

    void destroyObjects() {
      for (auto& [handle, state] : toplevelStates) {
        (void)state;
        ext_foreign_toplevel_handle_v1_destroy(handle);
      }
      toplevelStates.clear();

      if (toplevelList != nullptr) {
        ext_foreign_toplevel_list_v1_destroy(toplevelList);
        toplevelList = nullptr;
      }
      if (pointer != nullptr) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
          wl_pointer_release(pointer);
        } else {
          wl_pointer_destroy(pointer);
        }
        pointer = nullptr;
      }
      if (seat != nullptr) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(seat)) >= WL_SEAT_RELEASE_SINCE_VERSION) {
          wl_seat_release(seat);
        } else {
          wl_seat_destroy(seat);
        }
        seat = nullptr;
      }
      seatRegistryName = 0;
      if (captureManager != nullptr) {
        ext_image_copy_capture_manager_v1_destroy(captureManager);
        captureManager = nullptr;
      }
      if (toplevelSourceManager != nullptr) {
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(toplevelSourceManager);
        toplevelSourceManager = nullptr;
      }
      if (outputSourceManager != nullptr) {
        ext_output_image_capture_source_manager_v1_destroy(outputSourceManager);
        outputSourceManager = nullptr;
      }
      if (dmabuf != nullptr) {
        zwp_linux_dmabuf_v1_destroy(dmabuf);
        dmabuf = nullptr;
      }
      if (shm != nullptr) {
        wl_shm_destroy(shm);
        shm = nullptr;
      }
      for (auto& [output, state] : outputStates) {
        (void)state;
        wl_output_destroy(output);
      }
      outputStates.clear();
      if (registry != nullptr) {
        wl_registry_destroy(registry);
        registry = nullptr;
      }
    }
  };

  struct WaylandContext::CursorCapture {
    explicit CursorCapture(CaptureSession& owner) : owner(owner) {}
    ~CursorCapture() {
      pendingFrame.reset();
      imageCapture.reset();
      if (session != nullptr) {
        ext_image_copy_capture_cursor_session_v1_destroy(session);
      }
      if (buffer != nullptr) {
        wl_buffer_destroy(buffer);
      }
      if (mapping != nullptr && mapping != MAP_FAILED) {
        munmap(mapping, mapSize);
      }
      if (fd >= 0) {
        close(fd);
      }
    }

    CaptureSession& owner;
    ext_image_copy_capture_cursor_session_v1* session = nullptr;
    std::unique_ptr<CaptureSession> imageCapture;
    std::unique_ptr<CaptureFrame> pendingFrame;
    wl_buffer* buffer = nullptr;
    int fd = -1;
    void* mapping = nullptr;
    size_t mapSize = 0;
    uint32_t format = 0;
    uint32_t stride = 0;
    bool requestPending = false;
    CursorMetadata metadata;
  };

  WaylandContext::CaptureSession::CaptureSession(
      WaylandContext::Impl& impl, ext_image_capture_source_v1* source, ext_image_copy_capture_session_v1* session,
      ConstraintsCallback constraintsCb
  )
      : impl(impl), source(source), session(session), constraintsCb(std::move(constraintsCb)) {}

  WaylandContext::CaptureSession::~CaptureSession() {
    cursor.reset();
    if (session != nullptr) {
      ext_image_copy_capture_session_v1_destroy(session);
    }
    if (source != nullptr) {
      ext_image_capture_source_v1_destroy(source);
    }
  }

  bool WaylandContext::CaptureSession::hasCursorMetadata() const { return cursor != nullptr; }

  const CursorMetadata* WaylandContext::CaptureSession::cursorMetadata() const {
    return cursor != nullptr ? &cursor->metadata : nullptr;
  }

  WaylandContext::CaptureFrame::~CaptureFrame() {
    if (frame != nullptr) {
      ext_image_copy_capture_frame_v1_destroy(frame);
    }
  }

  namespace {

    void onOutputGeometry(
        void* data, wl_output* output, int32_t x, int32_t y, int32_t physicalWidth, int32_t physicalHeight,
        int32_t subpixel, const char* make, const char* model, int32_t transform
    ) {
      (void)physicalWidth;
      (void)physicalHeight;
      (void)subpixel;
      (void)make;
      (void)model;
      (void)transform;
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.x = x;
      state->info.y = y;
    }

    void onOutputMode(void* data, wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
      (void)refresh;
      if ((flags & WL_OUTPUT_MODE_CURRENT) == 0) {
        return;
      }
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.width = width;
      state->info.height = height;
    }

    void onOutputDone(void* data, wl_output* output) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.name = state->info.name.empty() ? "output-" + std::to_string(state->registryName) : state->info.name;
      impl->rebuildOutputs();
    }

    void onOutputScale(void* data, wl_output* output, int32_t factor) {
      (void)data;
      (void)output;
      (void)factor;
    }

    void onOutputName(void* data, wl_output* output, const char* name) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.name = name == nullptr ? std::string{} : name;
      state->hasName = true;
    }

    void onOutputDescription(void* data, wl_output* output, const char* description) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto* state = impl->findOutput(output);
      if (state == nullptr) {
        return;
      }
      state->info.description = description == nullptr ? std::string{} : description;
    }

    constexpr wl_output_listener kOutputListener = {
        .geometry = onOutputGeometry,
        .mode = onOutputMode,
        .done = onOutputDone,
        .scale = onOutputScale,
        .name = onOutputName,
        .description = onOutputDescription,
    };

    // ext_image_copy_capture_session_v1.shm_format carries a wl_shm.format value,
    // not a DRM fourcc. The two agree for every format except the two wl_shm
    // defines itself: ARGB8888 is 0 and XRGB8888 is 1, where the fourcc would be
    // 'AR24' and 'XR24'. Everything past the listener speaks DRM fourcc (the
    // SPA table, bytes-per-pixel, the dmabuf constraints), so the enum is
    // converted away here and converted back only where wl_shm itself is
    // spoken to, in createShmBuffer().
    uint32_t drmFormatFromWlShm(uint32_t wlShmFormat) {
      switch (wlShmFormat) {
      case WL_SHM_FORMAT_ARGB8888:
        return DRM_FORMAT_ARGB8888;
      case WL_SHM_FORMAT_XRGB8888:
        return DRM_FORMAT_XRGB8888;
      default:
        return wlShmFormat;
      }
    }

    uint32_t wlShmFormatFromDrm(uint32_t drmFormat) {
      switch (drmFormat) {
      case DRM_FORMAT_ARGB8888:
        return WL_SHM_FORMAT_ARGB8888;
      case DRM_FORMAT_XRGB8888:
        return WL_SHM_FORMAT_XRGB8888;
      default:
        return drmFormat;
      }
    }

    void onSessionBufferSize(void* data, ext_image_copy_capture_session_v1* session, uint32_t width, uint32_t height) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->pendingConstraints.bufferWidth = width;
      capture->pendingConstraints.bufferHeight = height;
    }

    void onSessionShmFormat(void* data, ext_image_copy_capture_session_v1* session, uint32_t format) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->pendingConstraints.shmFormats.push_back(drmFormatFromWlShm(format));
    }

    void onSessionDmabufDevice(void* data, ext_image_copy_capture_session_v1* session, wl_array* device) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      if (device != nullptr && device->size >= sizeof(dev_t)) {
        dev_t dev = 0;
        std::memcpy(&dev, device->data, sizeof(dev));
        capture->pendingConstraints.dmabufDevice = dev;
      }
    }

    void onSessionDmabufFormat(
        void* data, ext_image_copy_capture_session_v1* session, uint32_t format, wl_array* modifiers
    ) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      if (modifiers == nullptr || modifiers->size == 0) {
        capture->pendingConstraints.dmabufFormats.push_back({format, DRM_FORMAT_MOD_INVALID});
        return;
      }

      const auto* begin = static_cast<const uint64_t*>(modifiers->data);
      const auto* end = reinterpret_cast<const uint64_t*>(static_cast<const char*>(modifiers->data) + modifiers->size);
      for (const uint64_t* modifier = begin; modifier < end; ++modifier) {
        capture->pendingConstraints.dmabufFormats.push_back({format, *modifier});
      }
    }

    void onSessionDone(void* data, ext_image_copy_capture_session_v1* session) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->constraints = std::move(capture->pendingConstraints);
      capture->pendingConstraints = {};
      if (capture->constraintsCb) {
        capture->constraintsCb(capture->constraints);
      }
    }

    void onSessionStopped(void* data, ext_image_copy_capture_session_v1* session) {
      (void)session;
      auto* capture = static_cast<WaylandContext::CaptureSession*>(data);
      capture->stopped = true;
      auto callback = std::move(capture->stoppedCb);
      if (callback) {
        callback();
      }
    }

    constexpr ext_image_copy_capture_session_v1_listener kSessionListener = {
        .buffer_size = onSessionBufferSize,
        .shm_format = onSessionShmFormat,
        .dmabuf_device = onSessionDmabufDevice,
        .dmabuf_format = onSessionDmabufFormat,
        .done = onSessionDone,
        .stopped = onSessionStopped,
    };

    void onFrameTransform(void* data, ext_image_copy_capture_frame_v1* /*frame*/, uint32_t transform) {
      static_cast<WaylandContext::CaptureFrame*>(data)->buffer.transform = transform;
    }

    void onFrameDamage(
        void* /*data*/, ext_image_copy_capture_frame_v1* /*frame*/, int32_t /*x*/, int32_t /*y*/, int32_t /*width*/,
        int32_t /*height*/
    ) {}

    void onFramePresentationTime(
        void* data, ext_image_copy_capture_frame_v1* /*frame*/, uint32_t tvSecHi, uint32_t tvSecLo, uint32_t tvNsec
    ) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      state->presentationSec = (static_cast<uint64_t>(tvSecHi) << 32U) | tvSecLo;
      state->presentationNsec = tvNsec;
    }

    void onFrameReady(void* data, ext_image_copy_capture_frame_v1* /*frame*/) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      // Grab everything we need before touching state — the callback may delete it.
      auto* proxy = state->frame;
      auto cb = std::move(state->onReady);
      auto buf = state->buffer;
      const uint64_t ptsSec = state->presentationSec;
      const uint32_t ptsNsec = state->presentationNsec;
      state->frame = nullptr;
      // Destroy the Wayland proxy (frees the protocol object).
      ext_image_copy_capture_frame_v1_destroy(proxy);
      if (cb) {
        cb(buf, ptsSec, ptsNsec);
      }
    }

    void onFrameFailed(void* data, ext_image_copy_capture_frame_v1* /*frame*/, uint32_t reason) {
      auto* state = static_cast<WaylandContext::CaptureFrame*>(data);
      auto* proxy = state->frame;
      auto cb = std::move(state->onFailed);
      CaptureFailureReason failureReason = CaptureFailureReason::Retry;
      if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
        failureReason = CaptureFailureReason::ConstraintsChanged;
      } else if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
        failureReason = CaptureFailureReason::Stopped;
      }
      state->frame = nullptr;
      ext_image_copy_capture_frame_v1_destroy(proxy);
      if (cb) {
        cb(failureReason);
      }
    }

    constexpr ext_image_copy_capture_frame_v1_listener kFrameListener = {
        .transform = onFrameTransform,
        .damage = onFrameDamage,
        .presentation_time = onFramePresentationTime,
        .ready = onFrameReady,
        .failed = onFrameFailed,
    };

    uint32_t preferredCursorFormat(const std::vector<uint32_t>& formats) {
      const auto has = [&](uint32_t format) { return std::ranges::find(formats, format) != formats.end(); };
      if (has(DRM_FORMAT_ARGB8888)) {
        return DRM_FORMAT_ARGB8888;
      }
      if (has(DRM_FORMAT_ABGR8888)) {
        return DRM_FORMAT_ABGR8888;
      }
      if (has(DRM_FORMAT_XRGB8888)) {
        return DRM_FORMAT_XRGB8888;
      }
      if (has(DRM_FORMAT_XBGR8888)) {
        return DRM_FORMAT_XBGR8888;
      }
      return 0;
    }

    void resetCursorBuffer(WaylandContext::CursorCapture& cursor) {
      cursor.pendingFrame.reset();
      if (cursor.buffer != nullptr) {
        wl_buffer_destroy(cursor.buffer);
        cursor.buffer = nullptr;
      }
      if (cursor.mapping != nullptr && cursor.mapping != MAP_FAILED) {
        munmap(cursor.mapping, cursor.mapSize);
        cursor.mapping = nullptr;
      }
      if (cursor.fd >= 0) {
        close(cursor.fd);
        cursor.fd = -1;
      }
      cursor.mapSize = 0;
      cursor.format = 0;
      cursor.stride = 0;
    }

    void requestCursorFrame(WaylandContext::CursorCapture& cursor);

    bool configureCursorBuffer(WaylandContext::CursorCapture& cursor, const CaptureConstraints& constraints) {
      resetCursorBuffer(cursor);
      const uint32_t format = preferredCursorFormat(constraints.shmFormats);
      if (format == 0 || constraints.bufferWidth == 0 || constraints.bufferHeight == 0) {
        std::fprintf(stderr, "wayland: cursor capture has no supported SHM format\n");
        return false;
      }

      const uint64_t stride = static_cast<uint64_t>(constraints.bufferWidth) * 4U;
      const uint64_t size = stride * constraints.bufferHeight;
      if (stride > INT32_MAX || size > INT32_MAX) {
        std::fprintf(stderr, "wayland: cursor capture buffer is too large\n");
        return false;
      }

      const int fd = memfd_create("umbriel-cursor", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        if (fd >= 0) {
          close(fd);
        }
        std::fprintf(stderr, "wayland: unable to allocate cursor capture buffer: %s\n", std::strerror(errno));
        return false;
      }

      void* mapping = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mapping == MAP_FAILED) {
        close(fd);
        std::fprintf(stderr, "wayland: unable to map cursor capture buffer: %s\n", std::strerror(errno));
        return false;
      }

      wl_buffer* buffer = cursor.owner.impl.owner.createShmBuffer(
          constraints.bufferWidth, constraints.bufferHeight, format, static_cast<uint32_t>(stride), fd,
          static_cast<size_t>(size)
      );
      if (buffer == nullptr) {
        munmap(mapping, static_cast<size_t>(size));
        close(fd);
        return false;
      }

      cursor.fd = fd;
      cursor.mapping = mapping;
      cursor.mapSize = static_cast<size_t>(size);
      cursor.buffer = buffer;
      cursor.format = format;
      cursor.stride = static_cast<uint32_t>(stride);
      requestCursorFrame(cursor);
      return true;
    }

    void cursorFrameReady(WaylandContext::CursorCapture& cursor) {
      cursor.pendingFrame.reset();
      CursorMetadata& metadata = cursor.metadata;
      const CaptureConstraints& constraints = cursor.imageCapture->constraints;
      metadata.width = constraints.bufferWidth;
      metadata.height = constraints.bufferHeight;
      metadata.stride = cursor.stride;
      metadata.format = cursor.format;
      metadata.pixels.resize(cursor.mapSize);
      std::memcpy(metadata.pixels.data(), cursor.mapping, cursor.mapSize);

      const bool requestAgain = cursor.requestPending;
      cursor.requestPending = false;
      if (requestAgain) {
        requestCursorFrame(cursor);
      }
    }

    void cursorFrameFailed(WaylandContext::CursorCapture& cursor, CaptureFailureReason reason) {
      cursor.pendingFrame.reset();
      if (reason == CaptureFailureReason::Stopped) {
        cursor.metadata.visible = false;
        return;
      }
      if (reason == CaptureFailureReason::ConstraintsChanged) {
        configureCursorBuffer(cursor, cursor.imageCapture->constraints);
        return;
      }
      if (cursor.requestPending) {
        cursor.requestPending = false;
        requestCursorFrame(cursor);
      }
    }

    void requestCursorFrame(WaylandContext::CursorCapture& cursor) {
      if (cursor.imageCapture == nullptr || cursor.imageCapture->stopped || cursor.buffer == nullptr) {
        cursor.requestPending = true;
        return;
      }
      if (cursor.pendingFrame != nullptr) {
        cursor.requestPending = true;
        return;
      }

      cursor.requestPending = false;
      cursor.pendingFrame = cursor.owner.impl.owner.captureFrame(
          *cursor.imageCapture, cursor.buffer,
          [&cursor](CaptureBuffer&, uint64_t, uint32_t) { cursorFrameReady(cursor); },
          [&cursor](CaptureFailureReason reason) { cursorFrameFailed(cursor, reason); }
      );
    }

    void onCursorEnter(void* data, ext_image_copy_capture_cursor_session_v1*) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.visible = true;
      requestCursorFrame(*cursor);
    }

    void onCursorLeave(void* data, ext_image_copy_capture_cursor_session_v1*) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.visible = false;
    }

    void onCursorPosition(void* data, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.x = x;
      cursor->metadata.y = y;
      requestCursorFrame(*cursor);
    }

    void onCursorHotspot(void* data, ext_image_copy_capture_cursor_session_v1*, int32_t x, int32_t y) {
      auto* cursor = static_cast<WaylandContext::CursorCapture*>(data);
      cursor->metadata.hotspotX = x;
      cursor->metadata.hotspotY = y;
      requestCursorFrame(*cursor);
    }

    constexpr ext_image_copy_capture_cursor_session_v1_listener kCursorSessionListener = {
        .enter = onCursorEnter,
        .leave = onCursorLeave,
        .position = onCursorPosition,
        .hotspot = onCursorHotspot,
    };

    bool setupCursorCapture(WaylandContext::CaptureSession& capture) {
      WaylandContext::Impl& impl = capture.impl;
      if (impl.captureManager == nullptr || impl.pointer == nullptr) {
        return false;
      }

      auto cursor = std::make_unique<WaylandContext::CursorCapture>(capture);
      cursor->session = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
          impl.captureManager, capture.source, impl.pointer
      );
      if (cursor->session == nullptr) {
        return false;
      }

      auto* imageSession = ext_image_copy_capture_cursor_session_v1_get_capture_session(cursor->session);
      if (imageSession == nullptr) {
        return false;
      }
      cursor->imageCapture =
          std::make_unique<WaylandContext::CaptureSession>(impl, nullptr, imageSession, ConstraintsCallback{});

      auto* state = cursor.get();
      state->imageCapture->constraintsCb = [state](const CaptureConstraints& constraints) {
        configureCursorBuffer(*state, constraints);
      };
      state->imageCapture->stoppedCb = [state]() { state->metadata.visible = false; };
      ext_image_copy_capture_cursor_session_v1_add_listener(state->session, &kCursorSessionListener, state);
      ext_image_copy_capture_session_v1_add_listener(imageSession, &kSessionListener, state->imageCapture.get());
      capture.cursor = std::move(cursor);
      return true;
    }

    void onToplevelClosed(void* data, ext_foreign_toplevel_handle_v1* handle) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter == impl->toplevelStates.end()) {
        return;
      }
      ext_foreign_toplevel_handle_v1_destroy(handle);
      impl->toplevelStates.erase(iter);
      impl->rebuildToplevels();
    }

    void onToplevelDone(void* data, ext_foreign_toplevel_handle_v1* handle) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter == impl->toplevelStates.end()) {
        return;
      }
      iter->second->info = iter->second->pending;
      impl->rebuildToplevels();
    }

    void onToplevelTitle(void* data, ext_foreign_toplevel_handle_v1* handle, const char* title) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter != impl->toplevelStates.end()) {
        iter->second->pending.title = title == nullptr ? std::string{} : title;
      }
    }

    void onToplevelAppId(void* data, ext_foreign_toplevel_handle_v1* handle, const char* appId) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter != impl->toplevelStates.end()) {
        iter->second->pending.appId = appId == nullptr ? std::string{} : appId;
      }
    }

    void onToplevelIdentifier(void* data, ext_foreign_toplevel_handle_v1* handle, const char* identifier) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const auto iter = impl->toplevelStates.find(handle);
      if (iter != impl->toplevelStates.end()) {
        iter->second->pending.identifier = identifier == nullptr ? std::string{} : identifier;
      }
    }

    constexpr ext_foreign_toplevel_handle_v1_listener kToplevelHandleListener = {
        .closed = onToplevelClosed,
        .done = onToplevelDone,
        .title = onToplevelTitle,
        .app_id = onToplevelAppId,
        .identifier = onToplevelIdentifier,
    };

    void
    onToplevelListToplevel(void* data, ext_foreign_toplevel_list_v1* list, ext_foreign_toplevel_handle_v1* handle) {
      (void)list;
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      auto state = std::make_unique<WaylandContext::Impl::ToplevelState>();
      state->handle = handle;
      const auto [iter, inserted] = impl->toplevelStates.emplace(handle, std::move(state));
      (void)iter;
      (void)inserted;
      ext_foreign_toplevel_handle_v1_add_listener(handle, &kToplevelHandleListener, impl);
    }

    void onToplevelListFinished(void* data, ext_foreign_toplevel_list_v1* list) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      if (impl->toplevelList == list) {
        ext_foreign_toplevel_list_v1_destroy(list);
        impl->toplevelList = nullptr;
      }
    }

    constexpr ext_foreign_toplevel_list_v1_listener kToplevelListListener = {
        .toplevel = onToplevelListToplevel,
        .finished = onToplevelListFinished,
    };

    void onSeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
      if (hasPointer && impl->pointer == nullptr) {
        impl->pointer = wl_seat_get_pointer(seat);
      } else if (!hasPointer && impl->pointer != nullptr) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(impl->pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
          wl_pointer_release(impl->pointer);
        } else {
          wl_pointer_destroy(impl->pointer);
        }
        impl->pointer = nullptr;
      }
    }

    void onSeatName(void*, wl_seat*, const char*) {}

    constexpr wl_seat_listener kSeatListener = {
        .capabilities = onSeatCapabilities,
        .name = onSeatName,
    };

    void onRegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      const std::string_view iface = interface == nullptr ? std::string_view{} : std::string_view(interface);
      if (iface == wl_seat_interface.name && impl->seat == nullptr) {
        impl->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 9U)));
        impl->seatRegistryName = name;
        wl_seat_add_listener(impl->seat, &kSeatListener, impl);
        return;
      }

      if (iface == wl_shm_interface.name) {
        impl->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1U)));
        return;
      }
      if (iface == zwp_linux_dmabuf_v1_interface.name) {
        impl->dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, std::min(version, 4U))
        );
        return;
      }
      if (iface == wl_output_interface.name) {
        auto* output =
            static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
        auto state = std::make_unique<WaylandContext::Impl::OutputState>();
        state->output = output;
        state->registryName = name;
        impl->outputStates.emplace(output, std::move(state));
        wl_output_add_listener(output, &kOutputListener, impl);
        return;
      }
      if (iface == ext_output_image_capture_source_manager_v1_interface.name) {
        impl->outputSourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(
            registry, name, &ext_output_image_capture_source_manager_v1_interface, std::min(version, 1U)
        ));
        return;
      }
      if (iface == ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) {
        impl->toplevelSourceManager =
            static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(wl_registry_bind(
                registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, std::min(version, 1U)
            ));
        return;
      }
      if (iface == ext_image_copy_capture_manager_v1_interface.name) {
        impl->captureManager = static_cast<ext_image_copy_capture_manager_v1*>(
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, std::min(version, 1U))
        );
        return;
      }
      if (iface == ext_foreign_toplevel_list_v1_interface.name) {
        impl->toplevelList = static_cast<ext_foreign_toplevel_list_v1*>(
            wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, std::min(version, 1U))
        );
        ext_foreign_toplevel_list_v1_add_listener(impl->toplevelList, &kToplevelListListener, impl);
      }
    }

    void onRegistryRemove(void* data, wl_registry* registry, uint32_t name) {
      (void)registry;
      auto* impl = static_cast<WaylandContext::Impl*>(data);
      if (name == impl->seatRegistryName) {
        if (impl->pointer != nullptr) {
          if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(impl->pointer)) >= WL_POINTER_RELEASE_SINCE_VERSION) {
            wl_pointer_release(impl->pointer);
          } else {
            wl_pointer_destroy(impl->pointer);
          }
          impl->pointer = nullptr;
        }
        if (impl->seat != nullptr) {
          if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(impl->seat)) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(impl->seat);
          } else {
            wl_seat_destroy(impl->seat);
          }
          impl->seat = nullptr;
        }
        impl->seatRegistryName = 0;
        return;
      }
      for (auto iter = impl->outputStates.begin(); iter != impl->outputStates.end(); ++iter) {
        if (iter->second->registryName == name) {
          wl_output_destroy(iter->first);
          impl->outputStates.erase(iter);
          impl->rebuildOutputs();
          return;
        }
      }
    }

    constexpr wl_registry_listener kRegistryListener = {
        .global = onRegistryGlobal,
        .global_remove = onRegistryRemove,
    };

    std::unique_ptr<WaylandContext::CaptureSession> makeCaptureSession(
        WaylandContext::Impl& impl, ext_image_capture_source_v1* source, CaptureCursorMode cursorMode,
        ConstraintsCallback constraintsCb
    ) {
      if (impl.captureManager == nullptr || source == nullptr) {
        if (source != nullptr) {
          ext_image_capture_source_v1_destroy(source);
        }
        return nullptr;
      }

      const bool metadata = cursorMode == CaptureCursorMode::Metadata && impl.pointer != nullptr;
      const bool paintCursors =
          cursorMode == CaptureCursorMode::Embedded || (cursorMode == CaptureCursorMode::Metadata && !metadata);
      const uint32_t options = paintCursors ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
      auto* session = ext_image_copy_capture_manager_v1_create_session(impl.captureManager, source, options);
      if (session == nullptr) {
        ext_image_capture_source_v1_destroy(source);
        return nullptr;
      }

      auto capture = std::make_unique<WaylandContext::CaptureSession>(impl, source, session, std::move(constraintsCb));
      ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, capture.get());
      if (metadata && !setupCursorCapture(*capture)) {
        ConstraintsCallback callback = std::move(capture->constraintsCb);
        capture->source = nullptr;
        capture.reset();
        session = ext_image_copy_capture_manager_v1_create_session(
            impl.captureManager, source, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS
        );
        if (session == nullptr) {
          ext_image_capture_source_v1_destroy(source);
          return nullptr;
        }
        capture = std::make_unique<WaylandContext::CaptureSession>(impl, source, session, std::move(callback));
        ext_image_copy_capture_session_v1_add_listener(session, &kSessionListener, capture.get());
      }
      impl.flushDisplay();
      return capture;
    }

  } // namespace

  WaylandContext::WaylandContext(Loop& loop) : m_impl(std::make_unique<Impl>(*this, loop)) {
    m_impl->display = wl_display_connect(nullptr);
    if (m_impl->display == nullptr) {
      fprintf(stderr, "wayland: unable to connect to compositor\n");
      return;
    }

    m_impl->registry = wl_display_get_registry(m_impl->display);
    wl_registry_add_listener(m_impl->registry, &kRegistryListener, m_impl.get());
    // First roundtrip: discover and bind globals (wl_output, managers, etc.).
    roundtrip();
    // Second roundtrip: receive initial events from newly-bound globals
    // (wl_output.name/geometry/mode/done, toplevel handle events, etc.).
    roundtrip();

    const int fd = wl_display_get_fd(m_impl->display);
    m_impl->displayFdId = loop.addFd(fd, EPOLLIN | EPOLLERR | EPOLLHUP, [impl = m_impl.get()](uint32_t events) {
      impl->handleDisplayEvents(events);
    });
    m_impl->prepareRead();

    g_defaultContext = this;
  }

  WaylandContext::~WaylandContext() {
    if (g_defaultContext == this) {
      g_defaultContext = nullptr;
    }
    if (m_impl->displayFdId != 0) {
      m_impl->loop.removeFd(m_impl->displayFdId);
    }
    if (m_impl->display != nullptr) {
      m_impl->cancelPreparedRead();
      m_impl->destroyObjects();
      wl_display_disconnect(m_impl->display);
      m_impl->display = nullptr;
    }
  }

  wl_display* WaylandContext::display() const { return m_impl->display; }

  bool WaylandContext::connected() const { return m_impl->display != nullptr && !m_impl->disconnected; }

  const std::vector<OutputInfo>& WaylandContext::outputs() const { return m_impl->outputs; }

  const std::vector<ToplevelInfo>& WaylandContext::toplevels() const { return m_impl->toplevels; }

  std::string WaylandContext::buildChooserJson(uint32_t sourceTypes, bool multiple) const {
    nlohmann::json request;
    request["multiple"] = multiple;
    request["types"] = nlohmann::json::array();
    request["outputs"] = nlohmann::json::array();
    request["windows"] = nlohmann::json::array();

    if ((sourceTypes & kSourceMonitor) != 0) {
      request["types"].push_back("monitor");
      for (const OutputInfo& output : m_impl->outputs) {
        request["outputs"].push_back({
            {"name", output.name},
            {"description", output.description},
            {"width", output.width},
            {"height", output.height},
        });
      }
    }

    if ((sourceTypes & kSourceWindow) != 0) {
      request["types"].push_back("window");
      for (const ToplevelInfo& toplevel : m_impl->toplevels) {
        request["windows"].push_back({
            {"identifier", toplevel.identifier},
            {"app_id", toplevel.appId},
            {"title", toplevel.title},
        });
      }
    }

    return request.dump();
  }

  std::unique_ptr<WaylandContext::CaptureSession> WaylandContext::createOutputCapture(
      const std::string& outputName, CaptureCursorMode cursorMode, ConstraintsCallback constraintsCb
  ) {
    if (m_impl->outputSourceManager == nullptr) {
      fprintf(stderr, "wayland: output capture source manager is unavailable\n");
      return nullptr;
    }
    Impl::OutputState* output = m_impl->findOutput(outputName);
    if (output == nullptr) {
      fprintf(stderr, "wayland: output '%s' is unavailable\n", outputName.c_str());
      return nullptr;
    }

    auto* source =
        ext_output_image_capture_source_manager_v1_create_source(m_impl->outputSourceManager, output->output);
    return makeCaptureSession(*m_impl, source, cursorMode, std::move(constraintsCb));
  }

  std::unique_ptr<WaylandContext::CaptureSession> WaylandContext::createToplevelCapture(
      const std::string& identifier, CaptureCursorMode cursorMode, ConstraintsCallback constraintsCb
  ) {
    if (m_impl->toplevelSourceManager == nullptr) {
      fprintf(stderr, "wayland: toplevel capture source manager is unavailable\n");
      return nullptr;
    }
    Impl::ToplevelState* toplevel = m_impl->findToplevel(identifier);
    if (toplevel == nullptr) {
      fprintf(stderr, "wayland: toplevel '%s' is unavailable\n", identifier.c_str());
      return nullptr;
    }

    auto* source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        m_impl->toplevelSourceManager, toplevel->handle
    );
    return makeCaptureSession(*m_impl, source, cursorMode, std::move(constraintsCb));
  }

  std::unique_ptr<WaylandContext::CaptureFrame> WaylandContext::captureFrame(
      CaptureSession& session, wl_buffer* buffer, FrameReadyCallback onReady, FrameFailedCallback onFailed
  ) {
    if (session.session == nullptr || session.stopped) {
      if (onFailed) {
        onFailed(CaptureFailureReason::Stopped);
      }
      return nullptr;
    }
    if (buffer == nullptr) {
      if (onFailed) {
        onFailed(CaptureFailureReason::Retry);
      }
      return nullptr;
    }

    auto* frame = ext_image_copy_capture_session_v1_create_frame(session.session);
    if (frame == nullptr) {
      if (onFailed) {
        onFailed(CaptureFailureReason::Retry);
      }
      return nullptr;
    }

    auto state = std::make_unique<CaptureFrame>();
    state->frame = frame;
    state->buffer.wlBuffer = buffer;
    state->buffer.width = session.constraints.bufferWidth;
    state->buffer.height = session.constraints.bufferHeight;
    state->onReady = std::move(onReady);
    state->onFailed = std::move(onFailed);

    ext_image_copy_capture_frame_v1_add_listener(frame, &kFrameListener, state.get());
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(
        frame, 0, 0, static_cast<int32_t>(session.constraints.bufferWidth),
        static_cast<int32_t>(session.constraints.bufferHeight)
    );
    ext_image_copy_capture_frame_v1_capture(frame);
    m_impl->flushDisplay();
    return state;
  }

  void WaylandContext::requestCursorFrame(CaptureSession& session) {
    if (session.cursor != nullptr) {
      xdpu::requestCursorFrame(*session.cursor);
    }
  }

  wl_buffer* WaylandContext::createDmabufBuffer(
      uint32_t width, uint32_t height, uint32_t format, uint64_t modifier, const std::vector<DmabufPlane>& planes
  ) {
    if (m_impl->dmabuf == nullptr || planes.empty() || width == 0 || height == 0) {
      return nullptr;
    }

    zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(m_impl->dmabuf);
    if (params == nullptr) {
      return nullptr;
    }
    for (size_t index = 0; index < planes.size(); ++index) {
      const DmabufPlane& plane = planes[index];
      if (plane.fd < 0) {
        zwp_linux_buffer_params_v1_destroy(params);
        return nullptr;
      }
      zwp_linux_buffer_params_v1_add(
          params, plane.fd, static_cast<uint32_t>(index), plane.offset, plane.stride,
          static_cast<uint32_t>(modifier >> 32U), static_cast<uint32_t>(modifier & 0xffffffffU)
      );
    }
    wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(
        params, static_cast<int32_t>(width), static_cast<int32_t>(height), format, 0
    );
    zwp_linux_buffer_params_v1_destroy(params);
    m_impl->flushDisplay();
    return buffer;
  }

  wl_buffer* WaylandContext::createShmBuffer(
      uint32_t width, uint32_t height, uint32_t format, uint32_t stride, int fd, size_t size
  ) {
    if (m_impl->shm == nullptr || fd < 0 || width == 0 || height == 0 || size == 0) {
      return nullptr;
    }

    wl_shm_pool* pool = wl_shm_create_pool(m_impl->shm, fd, static_cast<int32_t>(size));
    if (pool == nullptr) {
      return nullptr;
    }
    wl_buffer* buffer = wl_shm_pool_create_buffer(
        pool, 0, static_cast<int32_t>(width), static_cast<int32_t>(height), static_cast<int32_t>(stride),
        wlShmFormatFromDrm(format)
    );
    wl_shm_pool_destroy(pool);
    m_impl->flushDisplay();
    return buffer;
  }

  void WaylandContext::flush() {
    if (connected()) {
      m_impl->flushDisplay();
    }
  }

  void WaylandContext::roundtrip() {
    if (m_impl->display == nullptr || m_impl->disconnected) {
      return;
    }
    m_impl->cancelPreparedRead();
    if (wl_display_roundtrip(m_impl->display) < 0) {
      m_impl->markDisconnected();
      return;
    }
    m_impl->rebuildOutputs();
    m_impl->rebuildToplevels();
    m_impl->prepareRead();
  }

  WaylandContext* WaylandContext::defaultContext() { return g_defaultContext; }

  uint32_t WaylandContext::preferredShmFormat(const std::vector<uint32_t>& formats) {
    // Constraints hold DRM fourcc values; the session listener has already
    // converted the wl_shm.format the protocol event carries.
    const auto has = [&](uint32_t fmt) { return std::ranges::find(formats, fmt) != formats.end(); };
    if (has(DRM_FORMAT_XRGB8888)) {
      return DRM_FORMAT_XRGB8888;
    }
    if (has(DRM_FORMAT_ARGB8888)) {
      return DRM_FORMAT_ARGB8888;
    }
    if (has(DRM_FORMAT_XBGR8888)) {
      return DRM_FORMAT_XBGR8888;
    }
    if (has(DRM_FORMAT_ABGR8888)) {
      return DRM_FORMAT_ABGR8888;
    }
    return formats.empty() ? 0 : formats.front();
  }

} // namespace xdpu
