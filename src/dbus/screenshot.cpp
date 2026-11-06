#include "dbus/screenshot.h"

#include "config/config.h"
#include "dbus/request.h"
#include "loop/loop.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <cairo/cairo.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <drm_fourcc.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-client.h>

namespace xdpu {

  namespace {

    constexpr char kInterface[] = "org.freedesktop.impl.portal.Screenshot";
    using Color = sdbus::Struct<double, double, double>;

    std::string firstLine(std::string_view text) {
      const size_t begin = text.find_first_not_of(" \t\r\n");
      if (begin == std::string_view::npos) {
        return {};
      }
      const size_t end = text.find_first_of("\r\n", begin);
      const std::string_view line =
          text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
      const size_t trimmedEnd = line.find_last_not_of(" \t");
      if (trimmedEnd == std::string_view::npos) {
        return {};
      }
      return std::string(line.substr(0, trimmedEnd + 1));
    }

    template <typename T> std::optional<T> optionValue(const PortalResults& options, const std::string& key) {
      const auto it = options.find(key);
      if (it == options.end() || !it->second.containsValueOfType<T>()) {
        return std::nullopt;
      }
      try {
        return it->second.get<T>();
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

    std::optional<std::string> selectedMonitorFromChooser(std::string_view output) {
      const std::string line = firstLine(output);
      if (line.empty()) {
        return std::nullopt;
      }

      try {
        const auto json = nlohmann::json::parse(line);
        const auto selections = json.value("selections", nlohmann::json::array());
        if (!selections.is_array() || selections.empty()) {
          return std::nullopt;
        }
        const auto& selection = selections.front();
        if (!selection.is_object() || selection.value("kind", "") != "monitor") {
          return std::nullopt;
        }
        std::string name = selection.value("output", "");
        if (name.empty()) {
          return std::nullopt;
        }
        return name;
      } catch (const std::exception& error) {
        std::println(stderr, "screenshot: chooser returned invalid JSON: {}", error.what());
        return std::nullopt;
      }
    }

    std::optional<std::filesystem::path> runtimeDir() {
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      if (runtime == nullptr || *runtime == '\0') {
        return std::filesystem::path{"/tmp"} / "xdg-desktop-portal-umbriel";
      }
      return std::filesystem::path{runtime} / "xdg-desktop-portal-umbriel";
    }

    std::optional<std::string> savePng(WaylandContext::ScreenshotResult& shot) {
      cairo_format_t cairoFormat = CAIRO_FORMAT_INVALID;
      bool needSwapRB = false;
      switch (shot.format) {
      case DRM_FORMAT_XRGB8888:
        cairoFormat = CAIRO_FORMAT_RGB24;
        break;
      case DRM_FORMAT_ARGB8888:
        cairoFormat = CAIRO_FORMAT_ARGB32;
        break;
      case DRM_FORMAT_XBGR8888:
        cairoFormat = CAIRO_FORMAT_RGB24;
        needSwapRB = true;
        break;
      case DRM_FORMAT_ABGR8888:
        cairoFormat = CAIRO_FORMAT_ARGB32;
        needSwapRB = true;
        break;
      default:
        std::println(stderr, "screenshot: unsupported screenshot DRM format 0x{:08x}", shot.format);
        return std::nullopt;
      }

      // Cairo expects BGRX/BGRA byte order; swap R↔B for XBGR/ABGR inputs.
      if (needSwapRB) {
        for (size_t i = 0; i + 3 < shot.pixels.size(); i += 4) {
          std::swap(shot.pixels[i], shot.pixels[i + 2]);
        }
      }

      // The capture is in the output's untransformed buffer orientation; write
      // the PNG as the output displays it.
      if (shot.transform != 0) {
        const auto width = static_cast<int32_t>(shot.width);
        const auto height = static_cast<int32_t>(shot.height);
        const bool swapAxes = (shot.transform & 1U) != 0;
        const int32_t displayWidth = swapAxes ? height : width;
        const int32_t displayHeight = swapAxes ? width : height;
        const size_t displayStride = static_cast<size_t>(displayWidth) * 4;
        std::vector<uint8_t> display(displayStride * static_cast<size_t>(displayHeight));
        for (int32_t y = 0; y < displayHeight; ++y) {
          for (int32_t x = 0; x < displayWidth; ++x) {
            const auto [bx, by] = displayToBuffer(x, y, width, height, shot.transform);
            std::memcpy(
                &display[static_cast<size_t>(y) * displayStride + static_cast<size_t>(x) * 4],
                &shot.pixels[static_cast<size_t>(by) * shot.stride + static_cast<size_t>(bx) * 4], 4
            );
          }
        }
        shot.pixels = std::move(display);
        shot.width = static_cast<uint32_t>(displayWidth);
        shot.height = static_cast<uint32_t>(displayHeight);
        shot.stride = static_cast<uint32_t>(displayStride);
        shot.transform = 0;
      }

      auto dir = runtimeDir();
      if (!dir) {
        return std::nullopt;
      }

      std::error_code error;
      std::filesystem::create_directories(*dir, error);
      if (error) {
        std::println(stderr, "screenshot: unable to create {}: {}", dir->c_str(), error.message());
        return std::nullopt;
      }
      (void)::chmod(dir->c_str(), 0700);

      const auto now = std::chrono::system_clock::now().time_since_epoch();
      const auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
      const std::filesystem::path path =
          *dir / ("screenshot-" + std::to_string(nsec) + "-" + std::to_string(::getpid()) + ".png");

      auto* data = shot.pixels.data();
      cairo_surface_t* surface = cairo_image_surface_create_for_data(
          data, cairoFormat, static_cast<int>(shot.width), static_cast<int>(shot.height), static_cast<int>(shot.stride)
      );
      if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        std::println(
            stderr, "screenshot: cairo surface creation failed: {}",
            cairo_status_to_string(cairo_surface_status(surface))
        );
        cairo_surface_destroy(surface);
        return std::nullopt;
      }

      cairo_surface_mark_dirty(surface);
      const cairo_status_t status = cairo_surface_write_to_png(surface, path.c_str());
      cairo_surface_destroy(surface);
      if (status != CAIRO_STATUS_SUCCESS) {
        std::println(
            stderr, "screenshot: cairo PNG write failed for {}: {}", path.c_str(), cairo_status_to_string(status)
        );
        return std::nullopt;
      }

      return path.string();
    }

    std::optional<Color> parseColorTriplet(std::string_view output) {
      std::istringstream stream{std::string(firstLine(output))};
      double r = 0.0;
      double g = 0.0;
      double b = 0.0;
      if (!(stream >> r >> g >> b)) {
        return std::nullopt;
      }
      if (r < 0.0 || r > 1.0 || g < 0.0 || g > 1.0 || b < 0.0 || b > 1.0) {
        return std::nullopt;
      }
      return Color{r, g, b};
    }

    struct PickPoint {
      int32_t x = 0;
      int32_t y = 0;
      std::string output;
    };

    std::optional<PickPoint> parseSlurpPoint(std::string_view output) {
      std::istringstream stream{std::string(firstLine(output))};
      PickPoint point;
      if (!(stream >> point.x >> point.y)) {
        return std::nullopt;
      }
      stream >> point.output;
      return point;
    }

    std::optional<Color> sampleColor(
        const WaylandContext::ScreenshotResult& shot, const OutputInfo& output, int32_t globalX, int32_t globalY
    ) {
      // Little-endian DRM formats: XRGB/ARGB store B,G,R in memory, XBGR/ABGR R,G,B.
      bool bgr = false;
      switch (shot.format) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
        break;
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        bgr = true;
        break;
      default:
        return std::nullopt;
      }
      if (shot.width == 0 || shot.height == 0 || shot.stride < shot.width * 4 || shot.pixels.empty()) {
        return std::nullopt;
      }

      // The capture is in the output's untransformed buffer orientation; a
      // rotated output displays it with width and height swapped.
      const bool swapAxes = (shot.transform & 1U) != 0;
      const auto displayWidth = static_cast<int32_t>(swapAxes ? shot.height : shot.width);
      const auto displayHeight = static_cast<int32_t>(swapAxes ? shot.width : shot.height);

      // The point is in global logical coordinates, which differ from buffer
      // pixels under fractional scale.  Without xdg-output, assume scale 1 at
      // the wl_output position.
      const bool logical = output.logicalWidth > 0 && output.logicalHeight > 0;
      const int32_t originX = logical ? output.logicalX : output.x;
      const int32_t originY = logical ? output.logicalY : output.y;
      const int32_t width = logical ? output.logicalWidth : displayWidth;
      const int32_t height = logical ? output.logicalHeight : displayHeight;

      const double localX =
          static_cast<double>(globalX - originX) * static_cast<double>(displayWidth) / static_cast<double>(width);
      const double localY =
          static_cast<double>(globalY - originY) * static_cast<double>(displayHeight) / static_cast<double>(height);
      const int32_t dx = std::clamp(static_cast<int32_t>(std::floor(localX)), int32_t{0}, displayWidth - 1);
      const int32_t dy = std::clamp(static_cast<int32_t>(std::floor(localY)), int32_t{0}, displayHeight - 1);

      const auto [px, py] =
          displayToBuffer(dx, dy, static_cast<int32_t>(shot.width), static_cast<int32_t>(shot.height), shot.transform);
      const size_t offset = static_cast<size_t>(py) * shot.stride + static_cast<size_t>(px) * 4;
      if (offset + 2 >= shot.pixels.size()) {
        return std::nullopt;
      }

      constexpr double scale = 1.0 / 255.0;
      const double first = static_cast<double>(shot.pixels[offset + 0]) * scale;
      const double g = static_cast<double>(shot.pixels[offset + 1]) * scale;
      const double third = static_cast<double>(shot.pixels[offset + 2]) * scale;
      return bgr ? Color{first, g, third} : Color{third, g, first};
    }

  } // namespace

  struct ScreenshotPortal::Impl {
    Loop& loop;
    sdbus::IConnection& connection;
    sdbus::IObject& object;
    Config config;
    WaylandContext& wayland;

    Impl(Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, Config config, WaylandContext& wayland)
        : loop(loop), connection(connection), object(object), config(std::move(config)), wayland(wayland) {
      object
          .addVTable(
              sdbus::registerMethod("Screenshot")
                  .implementedAs([this](
                                     PortalResponse&& result, const sdbus::ObjectPath& handle, const std::string& appId,
                                     const std::string& parentWindow, const PortalResults& options
                                 ) {
                    (void)appId;
                    (void)parentWindow;
                    screenshot(std::move(result), std::string(handle), options);
                  })
                  .withInputParamNames("handle", "app_id", "parent_window", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("PickColor")
                  .implementedAs([this](
                                     PortalResponse&& result, const sdbus::ObjectPath& handle, const std::string& appId,
                                     const std::string& parentWindow, const PortalResults& options
                                 ) {
                    (void)appId;
                    (void)parentWindow;
                    (void)options;
                    pickColor(std::move(result), std::string(handle));
                  })
                  .withInputParamNames("handle", "app_id", "parent_window", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerProperty("AvailableTargets").withGetter([] { return uint32_t{1}; }),
              sdbus::registerProperty("version").withGetter([] { return uint32_t{2}; })
          )
          .forInterface(kInterface);
    }

    // Holds all Wayland capture resources; destroyed when the operation completes.
    // Destruction order: frame first (cancels pending capture and unregisters
    // Wayland listeners), then session, buffer, mapping, fd.
    struct CaptureState {
      WaylandContext& wayland;
      std::unique_ptr<WaylandContext::CaptureFrame> frame; // destroyed first
      std::unique_ptr<WaylandContext::CaptureSession> session;
      struct wl_buffer* buffer = nullptr;
      void* mapping = nullptr;
      size_t mappingSize = 0;
      int fd = -1;
      CaptureConstraints constraints;

      explicit CaptureState(WaylandContext& wl) : wayland(wl) {}

      ~CaptureState() {
        // Order matters: frame proxy must be destroyed before its buffer/session.
        frame.reset();
        session.reset();
        if (buffer != nullptr) {
          wl_buffer_destroy(buffer);
        }
        if (mapping != nullptr && mapping != MAP_FAILED) {
          munmap(mapping, mappingSize);
        }
        if (fd >= 0) {
          ::close(fd);
        }
      }

      CaptureState(const CaptureState&) = delete;
      CaptureState& operator=(const CaptureState&) = delete;
    };

    struct Operation : public std::enable_shared_from_this<Operation> {
      Impl& portal;
      PortalResponse result;
      std::shared_ptr<Request> request;
      std::shared_ptr<AsyncProcess> process;
      std::shared_ptr<CaptureState> capture;
      int timeoutId = 0;
      bool done = false;

      Operation(Impl& portal, PortalResponse&& result) : portal(portal), result(std::move(result)) {}

      ~Operation() { cancelTimeout(); }

      void attachRequest(const std::string& handle) {
        std::weak_ptr<Operation> weak = shared_from_this();
        request = std::make_shared<Request>(portal.connection, handle, [weak] {
          if (auto op = weak.lock()) {
            op->cancel();
          }
        });
      }

      void deferRequestDestroy() {
        if (!request) {
          return;
        }
        auto doomed = std::move(request);
        portal.loop.addTimer(0, [doomed = std::move(doomed)] mutable { doomed.reset(); });
      }

      void finish(uint32_t response, const PortalResults& results = {}) {
        if (done) {
          return;
        }
        done = true;
        cancelTimeout();
        result.returnResults(response, results);
        deferRequestDestroy();
        process.reset();
        capture.reset(); // destroys frame → session → buffer → mapping → fd
        // Prevent the shared_ptr erase from destroying `this` while we're in it.
        auto prevent = shared_from_this();
        portal.inflight.erase(prevent);
      }

      void cancel() {
        if (done) {
          return;
        }
        if (process) {
          process->terminate();
        }
        finish(1, {});
      }

      void cancelTimeout() {
        if (timeoutId != 0) {
          portal.loop.removeTimer(timeoutId);
          timeoutId = 0;
        }
      }

      void finishWithUri(const std::string& path) {
        PortalResults results;
        results.emplace("uri", sdbus::Variant{std::string{"file://"} + path});
        finish(0, results);
      }

      // Start an async output capture.  onComplete is called on success;
      // finish(failureResponse) is called on any failure or timeout.
      // All protocol callbacks capture weak_ptr<Operation> to avoid cycles.
      template <typename Callback>
      void startCapture(
          const std::string& outputName, CaptureCursorMode cursorMode, uint32_t failureResponse, Callback onComplete
      ) {
        capture = std::make_shared<CaptureState>(portal.wayland);

        std::weak_ptr<Operation> weakSelf = shared_from_this();

        capture->session = portal.wayland.createOutputCapture(
            outputName, cursorMode,
            [weakSelf, failureResponse,
             onComplete = std::move(onComplete)](const CaptureConstraints& constraints) mutable {
              auto self = weakSelf.lock();
              if (!self || self->done || !self->capture) {
                return;
              }
              self->capture->constraints = constraints;
              self->onCaptureConstraints(*self->capture, failureResponse, std::move(onComplete));
            }
        );

        if (!capture->session) {
          std::println(stderr, "screenshot: unable to create capture session for output '{}'", outputName);
          finish(failureResponse, {});
          return;
        }

        // Timeout: cancel if the capture doesn't complete within 5 seconds.
        timeoutId = portal.loop.addTimer(5000, [weakSelf, failureResponse] {
          auto self = weakSelf.lock();
          if (!self) {
            return;
          }
          self->timeoutId = 0;
          if (!self->done) {
            std::println(stderr, "screenshot: capture timed out");
            self->finish(failureResponse, {});
          }
        });
      }

      template <typename Callback>
      void onCaptureConstraints(CaptureState& cap, uint32_t failureResponse, Callback onComplete) {
        if (cap.constraints.bufferWidth == 0 || cap.constraints.bufferHeight == 0) {
          std::println(stderr, "screenshot: empty buffer constraints");
          finish(failureResponse, {});
          return;
        }

        const uint32_t format = WaylandContext::preferredShmFormat(cap.constraints.shmFormats);
        if (format == 0) {
          std::println(stderr, "screenshot: no supported SHM format");
          finish(failureResponse, {});
          return;
        }

        const uint32_t stride = cap.constraints.bufferWidth * 4;
        cap.mappingSize = static_cast<size_t>(stride) * cap.constraints.bufferHeight;

        cap.fd = memfd_create("umbriel-screenshot", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (cap.fd < 0 || ftruncate(cap.fd, static_cast<off_t>(cap.mappingSize)) < 0) {
          std::println(stderr, "screenshot: memfd allocation failed: {}", std::strerror(errno));
          finish(failureResponse, {});
          return;
        }

        cap.mapping = mmap(nullptr, cap.mappingSize, PROT_READ | PROT_WRITE, MAP_SHARED, cap.fd, 0);
        if (cap.mapping == MAP_FAILED) {
          std::println(stderr, "screenshot: mmap failed: {}", std::strerror(errno));
          finish(failureResponse, {});
          return;
        }

        cap.buffer = portal.wayland.createShmBuffer(
            cap.constraints.bufferWidth, cap.constraints.bufferHeight, format, stride, cap.fd, cap.mappingSize
        );
        if (cap.buffer == nullptr) {
          std::println(stderr, "screenshot: wl_buffer creation failed");
          finish(failureResponse, {});
          return;
        }

        std::weak_ptr<Operation> weakSelf = shared_from_this();

        cap.frame = portal.wayland.captureFrame(
            *cap.session, cap.buffer, /*damageBuffer=*/true,
            /*onReady=*/
            [weakSelf, format, stride,
             onComplete = std::move(onComplete)](CaptureBuffer& buf, uint64_t /*ptsSec*/, uint32_t /*ptsNsec*/) {
              auto self = weakSelf.lock();
              if (!self || self->done || !self->capture) {
                return;
              }
              auto& c = *self->capture;
              WaylandContext::ScreenshotResult shot;
              shot.width = c.constraints.bufferWidth;
              shot.height = c.constraints.bufferHeight;
              shot.stride = stride;
              shot.format = format;
              shot.transform = buf.transform;
              shot.pixels.resize(c.mappingSize);
              std::memcpy(shot.pixels.data(), c.mapping, c.mappingSize);
              onComplete(self, std::move(shot));
            },
            /*onFailed=*/
            [weakSelf, failureResponse](CaptureFailureReason /*reason*/) {
              auto self = weakSelf.lock();
              if (!self || self->done) {
                return;
              }
              std::println(stderr, "screenshot: capture frame failed");
              self->finish(failureResponse, {});
            }
        );
        if (!cap.frame) {
          // captureFrame invoked onFailed synchronously; finish already called.
          return;
        }
      }

      // Convenience: capture + save PNG + finish with URI.
      void captureOutput(const std::string& outputName, uint32_t failureResponse) {
        if (done) {
          return;
        }
        startCapture(
            outputName, CaptureCursorMode::Embedded, failureResponse,
            [failureResponse](const std::shared_ptr<Operation>& self, WaylandContext::ScreenshotResult shot) {
              const auto path = savePng(shot);
              if (!path) {
                self->finish(failureResponse, {});
                return;
              }
              self->finishWithUri(*path);
            }
        );
      }

      std::string fallbackOutputName() const {
        const auto& outputs = portal.wayland.outputs();
        return outputs.empty() ? std::string{} : outputs.front().name;
      }
    };

    struct ScreenshotOperation : public Operation {
      using Operation::Operation;

      void start(const std::string& handle, bool interactive) {
        attachRequest(handle);
        if (!portal.config.screenshot.cmd.empty()) {
          runExternal(interactive);
          return;
        }
        if (interactive) {
          runChooser();
          return;
        }
        auto self = std::static_pointer_cast<ScreenshotOperation>(shared_from_this());
        portal.loop.addTimer(0, [self] { self->captureFirstOutput(); });
      }

      void runExternal(bool interactive) {
        std::string command = portal.config.screenshot.cmd;
        if (interactive) {
          command += " --interactive";
        }
        auto self = std::static_pointer_cast<ScreenshotOperation>(shared_from_this());
        process = AsyncProcess::start(portal.loop, std::move(command), {}, [self](const ProcessResult& child) {
          if (self->done) {
            return;
          }
          const std::string path = firstLine(child.output);
          if (!child.success() || path.empty()) {
            self->finish(2, {});
            return;
          }
          self->finishWithUri(path);
        });
      }

      void runChooser() {
        if (portal.config.screencast.chooserCmd.empty()) {
          std::println(stderr, "screenshot: no chooser command configured; cancelling interactive screenshot");
          finish(1, {});
          return;
        }

        auto self = std::static_pointer_cast<ScreenshotOperation>(shared_from_this());
        std::string input = portal.wayland.buildChooserJson(1, false);
        input.push_back('\n');
        process = AsyncProcess::start(
            portal.loop, portal.config.screencast.chooserCmd, std::move(input), [self](const ProcessResult& child) {
              if (self->done) {
                return;
              }
              if (child.commandNotFound()) {
                std::println(
                    stderr, "screenshot: chooser command not found ({}); falling back to first output",
                    self->portal.config.screencast.chooserCmd
                );
                self->captureFirstOutput();
                return;
              }
              if (!child.success()) {
                self->finish(1, {});
                return;
              }
              const auto outputName = selectedMonitorFromChooser(child.output);
              if (!outputName) {
                self->finish(1, {});
                return;
              }
              self->captureOutput(*outputName, 2);
            }
        );
      }

      void captureFirstOutput() {
        if (done) {
          return;
        }
        const std::string name = fallbackOutputName();
        if (name.empty()) {
          finish(2, {});
          return;
        }
        captureOutput(name, 2);
      }
    };

    struct PickColorOperation : public Operation {
      using Operation::Operation;

      struct Frame {
        OutputInfo output;
        WaylandContext::ScreenshotResult shot;
      };
      std::vector<OutputInfo> outputs;
      std::vector<Frame> frames;

      void start(const std::string& handle) {
        attachRequest(handle);
        if (!portal.config.screenshot.colorPickCmd.empty()) {
          runExternal();
          return;
        }
        // Capture before the point picker starts: once it exits, its overlay
        // can still be on screen (the compositor fades closing layer surfaces),
        // and a later capture would sample that overlay instead of the content.
        outputs = portal.wayland.outputs();
        if (outputs.empty()) {
          std::println(stderr, "screenshot: no outputs to pick a color from");
          finish(1, {});
          return;
        }
        frames.reserve(outputs.size());
        captureNextOutput();
      }

      void finishWithColor(const Color& color) {
        PortalResults results;
        results.emplace("color", sdbus::Variant{color});
        finish(0, results);
      }

      void runExternal() {
        auto self = std::static_pointer_cast<PickColorOperation>(shared_from_this());
        process = AsyncProcess::start(
            portal.loop, portal.config.screenshot.colorPickCmd, {}, [self](const ProcessResult& child) {
              if (self->done) {
                return;
              }
              const auto color = parseColorTriplet(child.output);
              if (!child.success() || !color) {
                self->finish(1, {});
                return;
              }
              self->finishWithColor(*color);
            }
        );
      }

      void captureNextOutput() {
        if (done) {
          return;
        }
        if (frames.size() == outputs.size()) {
          capture.reset();
          runSlurp();
          return;
        }
        auto self = std::static_pointer_cast<PickColorOperation>(shared_from_this());
        // The pointer rests on the picked pixel, so an embedded cursor would be
        // sampled instead of the content beneath it.
        startCapture(
            outputs[frames.size()].name, CaptureCursorMode::Hidden, 1,
            [self](const std::shared_ptr<Operation>& /*base*/, WaylandContext::ScreenshotResult shot) {
              self->cancelTimeout();
              self->frames.push_back({self->outputs[self->frames.size()], std::move(shot)});
              // Replace the capture state outside its own frame callback.
              self->portal.loop.addTimer(0, [self] { self->captureNextOutput(); });
            }
        );
      }

      void runSlurp() {
        auto self = std::static_pointer_cast<PickColorOperation>(shared_from_this());
        process = AsyncProcess::start(portal.loop, "slurp -p -f \"%x %y %o\"", {}, [self](const ProcessResult& child) {
          if (self->done) {
            return;
          }
          if (!child.success() || child.commandNotFound()) {
            self->finish(1, {});
            return;
          }
          const auto point = parseSlurpPoint(child.output);
          if (!point) {
            self->finish(1, {});
            return;
          }
          self->sample(*point);
        });
      }

      const Frame* frameAt(const PickPoint& point) const {
        const auto named = std::ranges::find_if(frames, [&](const Frame& f) { return f.output.name == point.output; });
        if (named != frames.end()) {
          return &*named;
        }
        const auto containing = std::ranges::find_if(frames, [&](const Frame& f) {
          const OutputInfo& o = f.output;
          return point.x >= o.logicalX
              && point.x < o.logicalX + o.logicalWidth
              && point.y >= o.logicalY
              && point.y < o.logicalY + o.logicalHeight;
        });
        return containing != frames.end() ? &*containing : nullptr;
      }

      void sample(const PickPoint& point) {
        const Frame* frame = frameAt(point);
        if (frame == nullptr) {
          std::println(stderr, "screenshot: no captured output contains {},{}", point.x, point.y);
          finish(1, {});
          return;
        }
        const auto color = sampleColor(frame->shot, frame->output, point.x, point.y);
        if (!color) {
          std::println(stderr, "screenshot: unsupported capture format for color picking");
          finish(1, {});
          return;
        }
        finishWithColor(*color);
      }
    };

    // In-flight operations kept alive by this set; deregistered in finish().
    std::set<std::shared_ptr<Operation>> inflight;

    void screenshot(PortalResponse&& result, const std::string& handle, const PortalResults& options) {
      const bool interactive = optionValue<bool>(options, "interactive").value_or(false);
      auto op = std::make_shared<ScreenshotOperation>(*this, std::move(result));
      inflight.insert(op);
      op->start(handle, interactive);
    }

    void pickColor(PortalResponse&& result, const std::string& handle) {
      auto op = std::make_shared<PickColorOperation>(*this, std::move(result));
      inflight.insert(op);
      op->start(handle);
    }
  };

  ScreenshotPortal::ScreenshotPortal(
      Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, const Config& config, WaylandContext& wayland
  )
      : m_impl(std::make_unique<Impl>(loop, connection, object, config, wayland)) {}

  ScreenshotPortal::~ScreenshotPortal() = default;

  void ScreenshotPortal::onConfigChanged(const Config&, const Config& newCfg) { m_impl->config = newCfg; }

} // namespace xdpu
