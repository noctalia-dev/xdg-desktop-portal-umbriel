#include "pipewire/pipewire.h"

#include "loop/loop.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <memory>
#include <optional>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <pipewire/version.h>
#include <spa/buffer/alloc.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wayland-client.h>
#include <xf86drm.h>

extern "C" {
void pw_init(int* argc, char** argv[]);
void pw_deinit(void);
}

namespace xdpu {

  namespace {

    constexpr struct {
      uint32_t drm;
      uint32_t spa;
    } kFormatTable[] = {
        {DRM_FORMAT_XRGB8888, SPA_VIDEO_FORMAT_BGRx},
        {DRM_FORMAT_ARGB8888, SPA_VIDEO_FORMAT_BGRA},
        {DRM_FORMAT_XBGR8888, SPA_VIDEO_FORMAT_RGBx},
        {DRM_FORMAT_ABGR8888, SPA_VIDEO_FORMAT_RGBA},
    };

    constexpr uint32_t kCursorMaxWidth = 512;
    constexpr uint32_t kCursorMaxHeight = 512;
    constexpr uint32_t kCursorBytesPerPixel = 4;
    constexpr size_t kCursorBitmapSize = static_cast<size_t>(kCursorMaxWidth) * kCursorMaxHeight * kCursorBytesPerPixel;
    constexpr size_t kCursorBitmapMetaOffset = sizeof(spa_meta_cursor);
    constexpr size_t kCursorBitmapDataOffset = sizeof(spa_meta_bitmap);
    constexpr size_t kCursorMetaSize = sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + kCursorBitmapSize;

    struct FormatChoice {
      uint32_t drm = DRM_FORMAT_XRGB8888;
      bool valid = false;
      uint32_t spa = SPA_VIDEO_FORMAT_BGRx;
      uint64_t modifier = DRM_FORMAT_MOD_INVALID;
      bool dmabuf = false;
    };

    std::optional<uint32_t> drmToSpa(uint32_t drm) {
      for (const auto& entry : kFormatTable) {
        if (entry.drm == drm) {
          return entry.spa;
        }
      }
      return std::nullopt;
    }

    std::optional<uint32_t> spaToDrm(uint32_t spa) {
      for (const auto& entry : kFormatTable) {
        if (entry.spa == spa) {
          return entry.drm;
        }
      }
      return std::nullopt;
    }

    int createMemfd(const char* name, size_t size) {
      const int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (fd < 0) {
        return -1;
      }
      if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        close(fd);
        return -1;
      }
      return fd;
    }

    uint32_t bytesPerPixel(uint32_t drm) {
      switch (drm) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        return 4;
      default:
        return 4;
      }
    }

    FormatChoice firstSupportedChoice(const CaptureConstraints& constraints) {
      for (uint32_t format : constraints.shmFormats) {
        if (auto spa = drmToSpa(format)) {
          return {.drm = format, .valid = true};
        }
      }
      return {};
    }

    bool openRenderNode(dev_t deviceId, int& fdOut, gbm_device*& gbmOut) {
      if (deviceId == 0) {
        return false;
      }

      drmDevicePtr device = nullptr;
      if (drmGetDeviceFromDevId(deviceId, 0, &device) != 0 || device == nullptr) {
        return false;
      }

      const char* node = nullptr;
      if ((device->available_nodes & (1 << DRM_NODE_RENDER)) != 0) {
        node = device->nodes[DRM_NODE_RENDER];
      } else if ((device->available_nodes & (1 << DRM_NODE_PRIMARY)) != 0) {
        node = device->nodes[DRM_NODE_PRIMARY];
      }

      int fd = -1;
      gbm_device* gbm = nullptr;
      if (node != nullptr) {
        fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
          gbm = gbm_create_device(fd);
          if (gbm == nullptr) {
            close(fd);
            fd = -1;
          }
        }
      }

      drmFreeDevice(&device);
      if (gbm == nullptr) {
        return false;
      }
      fdOut = fd;
      gbmOut = gbm;
      return true;
    }

  } // namespace

  struct PipeWireStream::Impl {
    struct StreamBuffer {
      CaptureBuffer capture;
      gbm_bo* bo = nullptr;
      std::vector<int> planeFds;
      std::vector<DmabufPlane> planes;
      int fd = -1;
      void* map = nullptr;
      size_t mapSize = 0;

      ~StreamBuffer() {
        if (capture.wlBuffer != nullptr) {
          wl_buffer_destroy(capture.wlBuffer);
        }
        if (map != nullptr && map != MAP_FAILED) {
          munmap(map, mapSize);
        }
        if (fd >= 0) {
          close(fd);
        }
        for (int planeFd : planeFds) {
          if (planeFd >= 0) {
            close(planeFd);
          }
        }
        if (bo != nullptr) {
          gbm_bo_destroy(bo);
        }
      }
    };

    Impl(uint32_t width, uint32_t height, CaptureConstraints constraints, uint32_t maxFps, bool cursorMetadata)
        : width(width), height(height), constraints(std::move(constraints)), maxFps(maxFps),
          cursorMetadata(cursorMetadata) {}
    uint32_t width = 0;
    uint32_t height = 0;
    CaptureConstraints constraints;
    uint32_t maxFps = 0;
    bool cursorMetadata = false;
    pw_stream* stream = nullptr;
    spa_hook streamListener = {};
    FormatChoice negotiated;
    std::unordered_set<pw_buffer*> buffers;
    int drmFd = -1;
    gbm_device* gbm = nullptr;

    ~Impl() {
      if (stream != nullptr) {
        pw_stream_destroy(stream);
        stream = nullptr;
      }
      for (pw_buffer* buffer : buffers) {
        if (buffer != nullptr && buffer->user_data != nullptr) {
          delete static_cast<StreamBuffer*>(buffer->user_data);
          buffer->user_data = nullptr;
        }
      }
      buffers.clear();
      if (gbm != nullptr) {
        gbm_device_destroy(gbm);
      }
      if (drmFd >= 0) {
        close(drmFd);
      }
    }

    bool ensureGbmDevice() { return gbm != nullptr || openRenderNode(constraints.dmabufDevice, drmFd, gbm); }

    gbm_bo* createDmabufBo(uint32_t format, uint64_t modifier) {
      gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm, width, height, format, &modifier, 1, GBM_BO_USE_RENDERING);
      if (bo == nullptr) {
        bo = gbm_bo_create_with_modifiers(gbm, width, height, format, &modifier, 1);
      }
      if (bo == nullptr && modifier == DRM_FORMAT_MOD_INVALID) {
        bo = gbm_bo_create(gbm, width, height, format, GBM_BO_USE_RENDERING);
      }
      return bo;
    }

    bool supportsDmabuf(uint32_t format, uint64_t modifier) {
      if (!ensureGbmDevice()) {
        return false;
      }
      gbm_bo* test = createDmabufBo(format, modifier);
      if (test == nullptr) {
        return false;
      }
      const bool matches = gbm_bo_get_modifier(test) == modifier;
      gbm_bo_destroy(test);
      return matches;
    }

    FormatChoice currentChoice() const {
      if (negotiated.valid) {
        return negotiated;
      }
      return firstSupportedChoice(constraints);
    }

    std::unique_ptr<StreamBuffer> allocateDmabuf(const FormatChoice& choice) {
      if (!ensureGbmDevice()) {
        return nullptr;
      }
      gbm_bo* bo = createDmabufBo(choice.drm, choice.modifier);
      if (bo == nullptr || gbm_bo_get_modifier(bo) != choice.modifier) {
        if (bo != nullptr) {
          gbm_bo_destroy(bo);
        }
        return nullptr;
      }

      auto allocation = std::make_unique<StreamBuffer>();
      allocation->bo = bo;
      allocation->capture.format = choice.drm;
      allocation->capture.width = width;
      allocation->capture.height = height;

      const int planeCount = gbm_bo_get_plane_count(bo);
      if (planeCount <= 0) {
        return nullptr;
      }
      allocation->planeFds.reserve(static_cast<size_t>(planeCount));
      allocation->planes.reserve(static_cast<size_t>(planeCount));
      for (int plane = 0; plane < planeCount; ++plane) {
        const int fd = gbm_bo_get_fd_for_plane(bo, plane);
        if (fd < 0) {
          return nullptr;
        }
        allocation->planeFds.push_back(fd);
        allocation->planes.push_back({
            .fd = fd,
            .stride = gbm_bo_get_stride_for_plane(bo, plane),
            .offset = gbm_bo_get_offset(bo, plane),
        });
      }
      allocation->capture.stride = allocation->planes.front().stride;
      allocation->capture.size = static_cast<size_t>(allocation->capture.stride) * height;

      if (WaylandContext* wayland = WaylandContext::defaultContext()) {
        allocation->capture.wlBuffer =
            wayland->createDmabufBuffer(width, height, choice.drm, choice.modifier, allocation->planes);
        if (allocation->capture.wlBuffer == nullptr) {
          return nullptr;
        }
      }
      return allocation;
    }

    std::unique_ptr<StreamBuffer> allocateShm(const FormatChoice& choice) {
      const uint32_t stride = width * bytesPerPixel(choice.drm);
      const size_t size = static_cast<size_t>(stride) * height;
      const int fd = createMemfd("umbriel-pw-shm", size);
      if (fd < 0) {
        fprintf(stderr, "pipewire: memfd allocation failed: %s\n", std::strerror(errno));
        return nullptr;
      }

      void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (map == MAP_FAILED) {
        fprintf(stderr, "pipewire: mmap failed for SHM buffer: %s\n", std::strerror(errno));
        close(fd);
        return nullptr;
      }

      auto allocation = std::make_unique<StreamBuffer>();
      allocation->fd = fd;
      allocation->map = map;
      allocation->mapSize = size;
      allocation->capture.format = choice.drm;
      allocation->capture.width = width;
      allocation->capture.height = height;
      allocation->capture.stride = stride;
      allocation->capture.data = map;
      allocation->capture.size = size;

      if (WaylandContext* wayland = WaylandContext::defaultContext()) {
        allocation->capture.wlBuffer = wayland->createShmBuffer(width, height, choice.drm, stride, fd, size);
        if (allocation->capture.wlBuffer == nullptr) {
          return nullptr;
        }
      }
      return allocation;
    }

    void configureSpaData(pw_buffer* pwBuffer, StreamBuffer& allocation) {
      spa_buffer* buffer = pwBuffer->buffer;
      if (buffer == nullptr || buffer->n_datas == 0 || buffer->datas == nullptr) {
        return;
      }

      if (allocation.bo != nullptr) {
        if (buffer->n_datas != allocation.planes.size()) {
          fprintf(stderr, "pipewire: expected %zu DMA-BUF planes, got %u\n", allocation.planes.size(), buffer->n_datas);
          return;
        }
        for (uint32_t index = 0; index < buffer->n_datas; ++index) {
          spa_data& data = buffer->datas[index];
          const DmabufPlane& plane = allocation.planes[index];
          data.type = SPA_DATA_DmaBuf;
          data.flags = SPA_DATA_FLAG_READABLE;
          data.fd = plane.fd;
          data.mapoffset = 0;
          data.maxsize = 0;
          data.data = nullptr;
          if (data.chunk != nullptr) {
            data.chunk->offset = plane.offset;
            data.chunk->size = 9;
            data.chunk->stride = static_cast<int32_t>(plane.stride);
            data.chunk->flags = SPA_CHUNK_FLAG_NONE;
          }
        }
      } else {
        spa_data& data = buffer->datas[0];
        data.type = SPA_DATA_MemFd;
        data.flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_MAPPABLE;
        data.fd = allocation.fd;
        data.mapoffset = 0;
        data.maxsize = static_cast<uint32_t>(allocation.capture.size);
        data.data = allocation.map;
        if (data.chunk != nullptr) {
          data.chunk->offset = 0;
          data.chunk->size = static_cast<uint32_t>(allocation.capture.size);
          data.chunk->stride = static_cast<int32_t>(allocation.capture.stride);
          data.chunk->flags = SPA_CHUNK_FLAG_NONE;
        }
      }

      if (auto* header = static_cast<spa_meta_header*>(
              spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header))
          )) {
        header->flags = SPA_META_HEADER_FLAG_DISCONT;
        header->offset = 0;
        header->pts = 0;
        header->dts_offset = 0;
        header->seq = 0;
      }
    }

    void updateBufferParams(const FormatChoice& choice) {
      std::array<uint8_t, 512> buffersStorage{};
      std::array<uint8_t, 256> headerMetaStorage{};
      std::array<uint8_t, 256> cursorMetaStorage{};
      spa_pod_builder buffersBuilder = SPA_POD_BUILDER_INIT(buffersStorage.data(), buffersStorage.size());
      spa_pod_builder headerMetaBuilder = SPA_POD_BUILDER_INIT(headerMetaStorage.data(), headerMetaStorage.size());
      spa_pod_builder cursorMetaBuilder = SPA_POD_BUILDER_INIT(cursorMetaStorage.data(), cursorMetaStorage.size());

      const uint32_t stride = choice.dmabuf ? 0 : width * bytesPerPixel(choice.drm);
      const uint32_t size = stride * height;
      const int dataTypes = choice.dmabuf ? (1 << SPA_DATA_DmaBuf) : (1 << SPA_DATA_MemFd);
      const int blocks =
          choice.dmabuf ? gbm_device_get_format_modifier_plane_count(gbm, choice.drm, choice.modifier) : 1;
      if (blocks <= 0) {
        fprintf(stderr, "pipewire: invalid plane count for negotiated DMA-BUF format\n");
        return;
      }

      spa_pod_frame buffersFrame;
      spa_pod_builder_push_object(&buffersBuilder, &buffersFrame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
      spa_pod_builder_add(
          &buffersBuilder, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8), SPA_PARAM_BUFFERS_blocks,
          SPA_POD_Int(blocks), 0
      );
      if (size > 0) {
        spa_pod_builder_add(&buffersBuilder, SPA_PARAM_BUFFERS_size, SPA_POD_Int(static_cast<int32_t>(size)), 0);
      }
      if (stride > 0) {
        spa_pod_builder_add(&buffersBuilder, SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int32_t>(stride)), 0);
      }
      spa_pod_builder_add(
          &buffersBuilder, SPA_PARAM_BUFFERS_align, SPA_POD_Int(16), SPA_PARAM_BUFFERS_dataType,
          SPA_POD_CHOICE_FLAGS_Int(dataTypes), 0
      );

      std::array<const spa_pod*, 3> params{};
      size_t paramCount = 0;
      params[paramCount++] = static_cast<const spa_pod*>(spa_pod_builder_pop(&buffersBuilder, &buffersFrame));
      params[paramCount++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
          &headerMetaBuilder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
          SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(static_cast<int32_t>(sizeof(spa_meta_header)))
      ));
      if (cursorMetadata) {
        params[paramCount++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &cursorMetaBuilder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
            SPA_POD_Id(SPA_META_Cursor), SPA_PARAM_META_size, SPA_POD_Int(static_cast<int32_t>(kCursorMetaSize))
        ));
      }
      pw_stream_update_params(stream, params.data(), paramCount);
    }
  };

  struct PipeWireContext::Impl {
    explicit Impl(Loop& loop) : loop(loop) {}

    Loop& loop;
    pw_loop* pwLoop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    int fdId = 0;

    ~Impl() {
      if (fdId != 0) {
        loop.removeFd(fdId);
      }
      if (core != nullptr) {
        pw_core_disconnect(core);
        core = nullptr;
      }
      if (context != nullptr) {
        pw_context_destroy(context);
        context = nullptr;
      }
      if (pwLoop != nullptr) {
        pw_loop_leave(pwLoop);
        pw_loop_destroy(pwLoop);
        pwLoop = nullptr;
      }
      pw_deinit();
    }
  };

  namespace {

    struct PodList {
      std::vector<std::unique_ptr<std::array<uint8_t, 1024>>> storage;
      std::vector<const spa_pod*> pods;

      void addVideoFormat(
          uint32_t width, uint32_t height, uint32_t spaFormat, std::optional<uint64_t> modifier, uint32_t maxFps
      ) {
        auto bytes = std::make_unique<std::array<uint8_t, 1024>>();
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes->data(), static_cast<uint32_t>(bytes->size()));
        const spa_rectangle size = SPA_RECTANGLE(width, height);
        const spa_fraction framerate = SPA_FRACTION(0, 1);
        const spa_fraction maxFramerate = SPA_FRACTION(maxFps, 1);

        spa_pod_frame frame;
        spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(
            &builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(spaFormat), SPA_FORMAT_VIDEO_size,
            SPA_POD_Rectangle(&size), 0
        );
        if (maxFps > 0) {
          spa_pod_builder_add(
              &builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&framerate), SPA_FORMAT_VIDEO_maxFramerate,
              SPA_POD_Fraction(&maxFramerate), 0
          );
        }
        if (modifier.has_value()) {
          spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
          spa_pod_builder_long(&builder, static_cast<int64_t>(*modifier));
        }
        const spa_pod* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame));
        pods.push_back(pod);
        storage.push_back(std::move(bytes));
      }
    };

    PodList buildFormatParams(PipeWireStream::Impl& impl) {
      PodList list;
      for (const CaptureConstraints::DmabufFormat& format : impl.constraints.dmabufFormats) {
        if (auto spa = drmToSpa(format.format); spa && impl.supportsDmabuf(format.format, format.modifier)) {
          list.addVideoFormat(impl.width, impl.height, *spa, format.modifier, impl.maxFps);
        }
      }
      for (uint32_t shmFormat : impl.constraints.shmFormats) {
        if (auto spa = drmToSpa(shmFormat)) {
          list.addVideoFormat(impl.width, impl.height, *spa, std::nullopt, impl.maxFps);
        } else {
          // Say so: a consumer that cannot import dmabuf sees only "no more
          // input formats" on its side, with nothing here naming the cause.
          fprintf(stderr, "pipewire: shm format 0x%08x has no SPA mapping; not advertised\n", shmFormat);
        }
      }
      return list;
    }

    void onStreamStateChanged(void* data, pw_stream_state oldState, pw_stream_state state, const char* error) {
      (void)oldState;
      auto* stream = static_cast<PipeWireStream*>(data);
      if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "pipewire: stream error: %s\n", error == nullptr ? "unknown" : error);
      }
      // A driver stream must submit its first buffer itself. Triggering the graph
      // here is not sufficient because there is no queued buffer to schedule yet.
      if (state == PW_STREAM_STATE_STREAMING && stream->onProcessRequest) {
        stream->onProcessRequest();
      }
    }

    void onStreamParamChanged(void* data, uint32_t id, const spa_pod* param) {
      auto* stream = static_cast<PipeWireStream*>(data);
      if (id != SPA_PARAM_Format || param == nullptr) {
        return;
      }

      spa_video_info_raw info = {};
      if (spa_format_video_raw_parse(param, &info) < 0) {
        return;
      }
      auto drm = spaToDrm(info.format);
      if (!drm) {
        return;
      }

      PipeWireStream::Impl& impl = *stream->implForCallbacks();
      impl.negotiated.drm = *drm;
      impl.negotiated.spa = info.format;
      impl.negotiated.dmabuf = (info.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
      impl.negotiated.modifier = impl.negotiated.dmabuf ? info.modifier : DRM_FORMAT_MOD_INVALID;
      impl.negotiated.valid = true;
      if (info.size.width != 0 && info.size.height != 0) {
        impl.width = info.size.width;
        impl.height = info.size.height;
      }
      impl.updateBufferParams(impl.negotiated);
    }

    void onStreamAddBuffer(void* data, pw_buffer* buffer) {
      auto* stream = static_cast<PipeWireStream*>(data);
      PipeWireStream::Impl& impl = *stream->implForCallbacks();
      if (buffer == nullptr || buffer->buffer == nullptr) {
        return;
      }

      const FormatChoice choice = impl.currentChoice();
      if (!choice.valid) {
        fprintf(stderr, "pipewire: no supported format for new buffer\n");
        return;
      }

      auto allocation = choice.dmabuf ? impl.allocateDmabuf(choice) : impl.allocateShm(choice);
      if (allocation == nullptr) {
        return;
      }

      impl.configureSpaData(buffer, *allocation);
      buffer->size = allocation->capture.size;
      buffer->user_data = allocation.release();
      impl.buffers.insert(buffer);

      if (stream->onAddBuffer) {
        stream->onAddBuffer(buffer);
      }
    }

    void onStreamRemoveBuffer(void* data, pw_buffer* buffer) {
      auto* stream = static_cast<PipeWireStream*>(data);
      PipeWireStream::Impl& impl = *stream->implForCallbacks();
      if (buffer == nullptr) {
        return;
      }
      // Stop accepting this buffer before releasing its allocation.  A
      // pending Wayland completion must not touch it after removal.
      impl.buffers.erase(buffer);
      if (stream->onRemoveBuffer) {
        stream->onRemoveBuffer(buffer);
      }
      delete static_cast<PipeWireStream::Impl::StreamBuffer*>(buffer->user_data);
      buffer->user_data = nullptr;
    }

    void onStreamProcess(void* data) {
      auto* stream = static_cast<PipeWireStream*>(data);
      if (stream->onProcessRequest) {
        stream->onProcessRequest();
      }
    }

    constexpr pw_stream_events kStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = onStreamStateChanged,
        .control_info = nullptr,
        .io_changed = nullptr,
        .param_changed = onStreamParamChanged,
        .add_buffer = onStreamAddBuffer,
        .remove_buffer = onStreamRemoveBuffer,
        .process = onStreamProcess,
        .drained = nullptr,
        .command = nullptr,
        .trigger_done = nullptr,
    };

  } // namespace

  PipeWireContext::PipeWireContext(Loop& loop) : m_impl(std::make_unique<Impl>(loop)) {
    pw_init(nullptr, nullptr);

    m_impl->pwLoop = pw_loop_new(nullptr);
    if (m_impl->pwLoop == nullptr) {
      fprintf(stderr, "pipewire: unable to create loop\n");
      return;
    }

    pw_loop_enter(m_impl->pwLoop);
    m_impl->context = pw_context_new(m_impl->pwLoop, nullptr, 0);
    if (m_impl->context == nullptr) {
      fprintf(stderr, "pipewire: unable to create context\n");
      return;
    }

    m_impl->core = pw_context_connect(m_impl->context, nullptr, 0);
    if (m_impl->core == nullptr) {
      fprintf(stderr, "pipewire: unable to connect to core\n");
      return;
    }

    const int fd = pw_loop_get_fd(m_impl->pwLoop);
    m_impl->fdId = loop.addFd(fd, EPOLLIN | EPOLLERR | EPOLLHUP, [impl = m_impl.get()](uint32_t events) {
      if ((events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0) {
        pw_loop_iterate(impl->pwLoop, 0);
      }
    });
  }

  PipeWireContext::~PipeWireContext() = default;

  pw_loop* PipeWireContext::pwLoop() const { return m_impl->pwLoop; }

  void PipeWireContext::processPending() {
    if (m_impl->pwLoop == nullptr) {
      return;
    }
    // Iterate with a brief timeout to let the PipeWire daemon assign node IDs.
    // A zero-timeout iterate may not be enough if the daemon hasn't responded yet.
    for (int i = 0; i < 5; ++i) {
      pw_loop_iterate(m_impl->pwLoop, 10); // 10ms per iteration, up to 50ms total
    }
  }

  std::unique_ptr<PipeWireStream> PipeWireContext::createStream(
      uint32_t width, uint32_t height, const CaptureConstraints& constraints, uint32_t maxFps, bool cursorMetadata
  ) {
    if (m_impl->core == nullptr || width == 0 || height == 0) {
      return nullptr;
    }

    auto streamImpl = std::make_unique<PipeWireStream::Impl>(width, height, constraints, maxFps, cursorMetadata);
    auto stream = std::unique_ptr<PipeWireStream>(new PipeWireStream(std::move(streamImpl)));

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_MEDIA_CLASS,
        "Video/Source", PW_KEY_MEDIA_NAME, "umbriel-screen-capture", PW_KEY_NODE_NAME, "umbriel-screen-capture",
        PW_KEY_NODE_DESCRIPTION, "Umbriel Screen Capture", nullptr
    );
    stream->m_impl->stream = pw_stream_new(m_impl->core, "umbriel-screen-capture", props);
    if (stream->m_impl->stream == nullptr) {
      fprintf(stderr, "pipewire: unable to create stream\n");
      return nullptr;
    }
    pw_stream_add_listener(stream->m_impl->stream, &stream->m_impl->streamListener, &kStreamEvents, stream.get());

    PodList params = buildFormatParams(*stream->m_impl);
    if (params.pods.empty()) {
      fprintf(stderr, "pipewire: capture constraints contain no PipeWire-compatible formats\n");
      return nullptr;
    }

    const auto flags =
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS | PW_STREAM_FLAG_ASYNC);
    const int rc = pw_stream_connect(
        stream->m_impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params.pods.data(), params.pods.size()
    );
    if (rc < 0) {
      fprintf(stderr, "pipewire: unable to connect stream: %s\n", spa_strerror(rc));
      return nullptr;
    }

    return stream;
  }

  bool PipeWireContext::supportsCursorMetadata() const { return pw_check_library_version(1, 4, 8); }

  PipeWireStream::PipeWireStream(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

  PipeWireStream::~PipeWireStream() = default;

  PipeWireStream::Impl* PipeWireStream::implForCallbacks() const { return m_impl.get(); }

  uint32_t PipeWireStream::nodeId() const {
    if (m_impl->stream == nullptr) {
      return 0;
    }
    const uint32_t id = pw_stream_get_node_id(m_impl->stream);
    return id == SPA_ID_INVALID ? 0 : id;
  }

  pw_buffer* PipeWireStream::dequeueBuffer() {
    return m_impl->stream == nullptr ? nullptr : pw_stream_dequeue_buffer(m_impl->stream);
  }

  void PipeWireStream::queueBuffer(pw_buffer* buf) {
    // A Wayland frame completion may arrive after teardown disconnected the
    // stream.  PipeWire rejects queueing buffers on a disconnected stream.
    if (m_impl->stream == nullptr || buf == nullptr || !connected() || !ownsBuffer(buf)) {
      return;
    }
    if (buf->buffer != nullptr && buf->buffer->datas != nullptr) {
      const CaptureBuffer* capture = captureBuffer(buf);
      for (uint32_t index = 0; index < buf->buffer->n_datas; ++index) {
        spa_data& data = buf->buffer->datas[index];
        if (data.chunk == nullptr) {
          continue;
        }
        if (data.type == SPA_DATA_DmaBuf) {
          data.chunk->size = 9;
        } else {
          data.chunk->offset = 0;
          data.chunk->size =
              capture != nullptr ? static_cast<uint32_t>(capture->stride * capture->height) : data.maxsize;
          data.chunk->stride = capture != nullptr ? static_cast<int32_t>(capture->stride) : 0;
        }
        data.chunk->flags = SPA_CHUNK_FLAG_NONE;
      }
    }
    pw_stream_queue_buffer(m_impl->stream, buf);
  }

  void PipeWireStream::setCursorMetadata(pw_buffer* buffer, const CursorMetadata* metadata) {
    if (!m_impl->cursorMetadata || buffer == nullptr || buffer->buffer == nullptr) {
      return;
    }
    auto* cursor =
        static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_Cursor, kCursorMetaSize));
    if (cursor == nullptr) {
      return;
    }

    cursor->id = 1;
    cursor->flags = 0;
    cursor->position.x = metadata != nullptr ? metadata->x : 0;
    cursor->position.y = metadata != nullptr ? metadata->y : 0;
    cursor->hotspot.x = metadata != nullptr ? metadata->hotspotX : 0;
    cursor->hotspot.y = metadata != nullptr ? metadata->hotspotY : 0;
    cursor->bitmap_offset = static_cast<uint32_t>(kCursorBitmapMetaOffset);

    auto* bitmap = reinterpret_cast<spa_meta_bitmap*>(reinterpret_cast<uint8_t*>(cursor) + kCursorBitmapMetaOffset);
    bitmap->format = SPA_VIDEO_FORMAT_BGRA;
    bitmap->size.width = 1;
    bitmap->size.height = 1;
    bitmap->stride = static_cast<int32_t>(kCursorBytesPerPixel);
    bitmap->offset = static_cast<uint32_t>(kCursorBitmapDataOffset);

    auto* pixels = reinterpret_cast<uint8_t*>(bitmap) + kCursorBitmapDataOffset;
    std::memset(pixels, 0, kCursorBytesPerPixel);
    if (metadata == nullptr || !metadata->visible || metadata->pixels.empty()) {
      return;
    }

    const std::optional<uint32_t> format = drmToSpa(metadata->format);
    if (!format.has_value() || metadata->stride < metadata->width * kCursorBytesPerPixel) {
      return;
    }
    const uint32_t width = std::min(metadata->width, kCursorMaxWidth);
    const uint32_t height = std::min(metadata->height, kCursorMaxHeight);
    const uint32_t stride = width * kCursorBytesPerPixel;
    if (width == 0
        || height == 0
        || metadata->pixels.size() < static_cast<size_t>(metadata->stride) * metadata->height) {
      return;
    }

    bitmap->format = *format;
    bitmap->size.width = width;
    bitmap->size.height = height;
    bitmap->stride = static_cast<int32_t>(stride);
    for (uint32_t row = 0; row < height; ++row) {
      std::memcpy(
          pixels + static_cast<size_t>(row) * stride,
          metadata->pixels.data() + static_cast<size_t>(row) * metadata->stride, stride
      );
    }
  }

  bool PipeWireStream::reconfigure(const CaptureConstraints& constraints) {
    if (m_impl->stream == nullptr || constraints.bufferWidth == 0 || constraints.bufferHeight == 0) {
      return false;
    }

    const uint32_t previousWidth = m_impl->width;
    const uint32_t previousHeight = m_impl->height;
    CaptureConstraints previousConstraints = m_impl->constraints;
    const FormatChoice previousNegotiated = m_impl->negotiated;

    m_impl->width = constraints.bufferWidth;
    m_impl->height = constraints.bufferHeight;
    m_impl->constraints = constraints;
    m_impl->negotiated = {};

    PodList params = buildFormatParams(*m_impl);
    if (params.pods.empty()) {
      m_impl->width = previousWidth;
      m_impl->height = previousHeight;
      m_impl->constraints = std::move(previousConstraints);
      m_impl->negotiated = previousNegotiated;
      return false;
    }

    const int rc = pw_stream_update_params(m_impl->stream, params.pods.data(), params.pods.size());
    if (rc < 0) {
      std::fprintf(stderr, "pipewire: unable to reconfigure stream: %s\n", spa_strerror(rc));
      m_impl->width = previousWidth;
      m_impl->height = previousHeight;
      m_impl->constraints = std::move(previousConstraints);
      m_impl->negotiated = previousNegotiated;
      return false;
    }
    return true;
  }

  void PipeWireStream::disconnect() {
    if (m_impl->stream != nullptr) {
      pw_stream_disconnect(m_impl->stream);
    }
  }

  bool PipeWireStream::connected() const {
    if (m_impl->stream == nullptr) {
      return false;
    }
    const char* error = nullptr;
    const pw_stream_state state = pw_stream_get_state(m_impl->stream, &error);
    (void)error;
    return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
  }

  bool PipeWireStream::streaming() const {
    if (m_impl->stream == nullptr) {
      return false;
    }
    const char* error = nullptr;
    return pw_stream_get_state(m_impl->stream, &error) == PW_STREAM_STATE_STREAMING;
  }

  void PipeWireStream::triggerProcess() {
    if (m_impl->stream != nullptr) {
      pw_stream_trigger_process(m_impl->stream);
    }
  }

  CaptureBuffer* PipeWireStream::captureBuffer(pw_buffer* buffer) const {
    if (buffer == nullptr || buffer->user_data == nullptr) {
      return nullptr;
    }
    auto* streamBuffer = static_cast<Impl::StreamBuffer*>(buffer->user_data);
    return &streamBuffer->capture;
  }

  bool PipeWireStream::ownsBuffer(pw_buffer* buffer) const {
    return buffer != nullptr && m_impl->buffers.contains(buffer);
  }

} // namespace xdpu
