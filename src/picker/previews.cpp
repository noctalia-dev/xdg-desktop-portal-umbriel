#include "picker/previews.h"

#include "loop/loop.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <drm_fourcc.h>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

namespace xdpu {
  namespace {
    struct Capture {
      std::unique_ptr<WaylandContext::CaptureSession> session;
      std::unique_ptr<WaylandContext::CaptureFrame> frame;
      wl_buffer* buffer = nullptr;
      void* mapping = MAP_FAILED;
      size_t size = 0;
      int fd = -1;

      ~Capture() {
        frame.reset();
        session.reset();
        if (buffer != nullptr) {
          wl_buffer_destroy(buffer);
        }
        if (mapping != MAP_FAILED) {
          munmap(mapping, size);
        }
        if (fd >= 0) {
          close(fd);
        }
      }
    };

    GdkTexture*
    thumbnail(const Capture& capture, uint32_t width, uint32_t height, uint32_t format, uint32_t transform) {
      // Downsample directly from SHM so a second full-resolution copy is never needed.
      const double scale = std::min(1.0, 512.0 / std::max(width, height));
      const int sw = std::max(1, static_cast<int>(width * scale));
      const int sh = std::max(1, static_cast<int>(height * scale));
      // Rotated buffers swap the thumbnail's axes; sampling undoes the transform in
      // the same pass, so no rotated or flipped intermediate copy is ever allocated.
      const bool swapAxes = (transform & 1U) != 0;
      const int w = swapAxes ? sh : sw;
      const int h = swapAxes ? sw : sh;
      const size_t stride = static_cast<size_t>(w) * 3;
      auto* target = static_cast<uint8_t*>(g_malloc(stride * h));
      const bool bgr = format == DRM_FORMAT_XBGR8888 || format == DRM_FORMAT_ABGR8888;
      auto* pixels = static_cast<const uint32_t*>(capture.mapping);
      for (int y = 0; y < h; ++y) {
        auto* row = target + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
          // Invert the horizontal flip first, then the rotation, mirroring how
          // wl_output.transform composes them.
          const int fx = (transform & 4U) != 0 ? w - 1 - x : x;
          int u = fx;
          int v = y;
          switch (transform & 3U) {
          case 1:
            u = y;
            v = sh - 1 - fx;
            break;
          case 2:
            u = sw - 1 - fx;
            v = sh - 1 - y;
            break;
          case 3:
            u = sw - 1 - y;
            v = fx;
            break;
          default:
            break;
          }
          const uint32_t pixel =
              pixels[(static_cast<size_t>(v) * height / sh) * width + static_cast<size_t>(u) * width / sw];
          auto* rgb = row + static_cast<size_t>(x) * 3;
          rgb[0] = (pixel >> (bgr ? 0 : 16)) & 0xff;
          rgb[1] = (pixel >> 8) & 0xff;
          rgb[2] = (pixel >> (bgr ? 16 : 0)) & 0xff;
        }
      }
      GBytes* bytes = g_bytes_new_take(target, stride * h);
      GdkTexture* texture = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8, bytes, stride);
      g_bytes_unref(bytes);
      return texture;
    }

    bool finishCaptureCleanup(Loop& loop, WaylandContext& wayland) {
      // Capture destruction only queues protocol requests. Keep dispatching until
      // the compositor has processed them, rather than parking an active capture
      // session on an idle connection until the picker closes.
      struct Sync {
        Loop& loop;
        bool done = false;
      } sync{loop};
      static constexpr wl_callback_listener listener = {
          .done = +[](void* data, wl_callback*, uint32_t) {
            auto& sync = *static_cast<Sync*>(data);
            sync.done = true;
            sync.loop.quit();
          },
      };
      wl_callback* callback = wl_display_sync(wayland.display());
      if (callback == nullptr) {
        return false;
      }
      wl_callback_add_listener(callback, &listener, &sync);
      // Never wait indefinitely on a compositor that stopped responding. The
      // caller closes the private connection if cleanup cannot be acknowledged.
      const int timeout = loop.addTimer(250, [&loop] { loop.quit(); });
      wayland.flush();
      if (timeout != 0 && wayland.connected()) {
        loop.run();
      }
      loop.removeTimer(timeout);
      wl_callback_destroy(callback);
      return sync.done;
    }

    GdkTexture* captureSource(Loop& loop, WaylandContext& wayland, const PreviewSource& source, std::stop_token stop) {
      Capture capture;
      GdkTexture* result = nullptr;
      bool done = false;
      auto finish = [&] {
        done = true;
        loop.quit();
      };
      auto constraints = [&](const CaptureConstraints& info) {
        // Resizes or unsupported formats leave a usable source with a fallback thumbnail.
        if (done || capture.buffer != nullptr) {
          finish();
          return;
        }
        const uint32_t format = WaylandContext::preferredShmFormat(info.shmFormats);
        if ((format != DRM_FORMAT_XRGB8888
             && format != DRM_FORMAT_ARGB8888
             && format != DRM_FORMAT_XBGR8888
             && format != DRM_FORMAT_ABGR8888)
            || info.bufferWidth == 0
            || info.bufferHeight == 0
            || info.bufferWidth > 16384
            || info.bufferHeight > 16384) {
          finish();
          return;
        }
        const uint32_t stride = info.bufferWidth * 4;
        capture.size = static_cast<size_t>(stride) * info.bufferHeight;
        if (capture.size > 128 * 1024 * 1024) {
          finish();
          return;
        }
        capture.fd = memfd_create("umbriel-preview", MFD_CLOEXEC);
        if (capture.fd < 0 || ftruncate(capture.fd, static_cast<off_t>(capture.size)) < 0) {
          finish();
          return;
        }
        capture.mapping = mmap(nullptr, capture.size, PROT_READ | PROT_WRITE, MAP_SHARED, capture.fd, 0);
        if (capture.mapping == MAP_FAILED) {
          finish();
          return;
        }
        capture.buffer =
            wayland.createShmBuffer(info.bufferWidth, info.bufferHeight, format, stride, capture.fd, capture.size);
        capture.frame = wayland.captureFrame(
            *capture.session, capture.buffer,
            [&, width = info.bufferWidth, height = info.bufferHeight,
             format](CaptureBuffer& buffer, uint64_t, uint32_t) {
              result = thumbnail(capture, width, height, format, buffer.transform);
              finish();
            },
            [&](CaptureFailureReason) { finish(); }
        );
      };
      capture.session = source.monitor
          ? wayland.createOutputCapture(source.identifier, CaptureCursorMode::Hidden, constraints)
          : wayland.createToplevelCapture(source.identifier, CaptureCursorMode::Hidden, constraints);
      if (!capture.session) {
        return nullptr;
      }
      capture.session->stoppedCb = finish;
      // Check cancellation frequently, and bound sources that never produce a frame.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      int timer = 0;
      std::function<void()> tick = [&] {
        timer = 0;
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
          finish();
        } else {
          timer = loop.addTimer(50, tick);
        }
      };
      timer = loop.addTimer(50, tick);
      if (!done && !stop.stop_requested() && wayland.connected()) {
        loop.run();
      }
      if (timer != 0) {
        loop.removeTimer(timer);
      }
      return result;
    }
  } // namespace

  struct Previews::Impl {
    const std::vector<PreviewSource> sources;
    const ResultsReadyCallback onResultsReady;
    std::vector<bool> started;
    std::deque<size_t> pending;
    std::mutex mutex;
    std::condition_variable_any workAvailable;
    std::vector<PreviewResult> results;
    std::jthread worker;

    Impl(std::vector<PreviewSource> sources, ResultsReadyCallback onResultsReady)
        : sources(std::move(sources)), onResultsReady(std::move(onResultsReady)), started(this->sources.size(), false),
          worker([this](std::stop_token stop) { run(stop); }) {}

    void run(std::stop_token stop) {
      std::unique_ptr<Loop> loop;
      std::unique_ptr<WaylandContext> wayland;
      while (!stop.stop_requested()) {
        size_t index = 0;
        {
          std::unique_lock lock(mutex);
          if (!workAvailable.wait(lock, stop, [this] { return !pending.empty(); })) {
            break;
          }
          index = pending.front();
          pending.pop_front();
          started[index] = true;
        }
        GdkTexture* texture = nullptr;
        try {
          // Even the Wayland connection is deferred until a card is visible.
          if (!wayland) {
            loop = std::make_unique<Loop>();
            wayland = std::make_unique<WaylandContext>(*loop);
          }
          if (!stop.stop_requested() && wayland->connected()) {
            texture = captureSource(*loop, *wayland, sources[index], stop);
            // All Capture members have now been destroyed. Flush and drain
            // their destruction before publishing the thumbnail or idling.
            if (!wayland->connected() || !finishCaptureCleanup(*loop, *wayland)) {
              wayland.reset();
            }
          }
        } catch (const std::exception& error) {
          g_warning("umbriel-share-picker: preview unavailable: %s", error.what());
        }
        {
          std::scoped_lock lock(mutex);
          results.push_back({index, texture});
        }
        if (onResultsReady) {
          onResultsReady();
        }
      }
    }

    ~Impl() {
      worker.request_stop();
      worker.join();
      for (auto& result : results) {
        g_clear_object(&result.texture);
      }
    }
  };

  Previews::Previews(std::vector<PreviewSource> sources, ResultsReadyCallback onResultsReady)
      : m_impl(std::make_unique<Impl>(std::move(sources), std::move(onResultsReady))) {}
  Previews::~Previews() = default;

  void Previews::request(std::vector<size_t> indices) {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->pending.clear();
    for (size_t index : indices) {
      if (index < m_impl->sources.size()
          && !m_impl->started[index]
          && std::ranges::find(m_impl->pending, index) == m_impl->pending.end()) {
        m_impl->pending.push_back(index);
      }
    }
    if (!m_impl->pending.empty()) {
      m_impl->workAvailable.notify_one();
    }
  }

  void Previews::stop() { m_impl->worker.request_stop(); }

  std::vector<PreviewResult> Previews::takeResults() {
    std::scoped_lock lock(m_impl->mutex);
    std::vector<PreviewResult> results;
    results.swap(m_impl->results);
    return results;
  }
} // namespace xdpu
