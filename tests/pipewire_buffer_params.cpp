// Exercise the production parameter builder with controlled GBM responses.
// Function sections let this test link without the unused Wayland/DBus paths.
#include "pipewire/pipewire.cpp"

#include <cstdlib>
#include <print>

namespace {
  struct Fixture {
    int queryResult = 1;
    int bufferPlanes = 1;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    bool allocationFails = false;
    bool legacyOnly = false;
    bool modifierMismatch = false;
    int updateResult = 0;
    int queries = 0;
    int allocations = 0;
    int destroys = 0;
    int updates = 0;
    int blocks = 0;
    int cursorBytes = 0;
  } fixture;

  // Opaque sentinels: wrappers never dereference these addresses.
  char deviceStorage;
  char bufferStorage;

  void require(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "{}", message);
      std::exit(EXIT_FAILURE);
    }
  }

  gbm_bo* allocate(bool legacy) {
    ++fixture.allocations;
    return fixture.allocationFails || (fixture.legacyOnly && !legacy) ? nullptr
                                                                      : reinterpret_cast<gbm_bo*>(&bufferStorage);
  }

  bool configure(bool dmabuf = true, bool cursorMetadata = true) {
    xdpu::PipeWireStream::Impl impl(1920, 1080, {}, 60, cursorMetadata);
    impl.gbm = reinterpret_cast<gbm_device*>(&deviceStorage);
    const xdpu::FormatChoice choice{
        .drm = DRM_FORMAT_XRGB8888,
        .valid = true,
        .spa = SPA_VIDEO_FORMAT_BGRx,
        .modifier = fixture.modifier,
        .dmabuf = dmabuf,
    };
    return impl.updateBufferParams(choice);
  }

  void cursorPresent(int planes) {
    require(fixture.updates == 1, "capture parameters were not published exactly once");
    require(fixture.blocks == planes, "negotiated block count differs from the allocation");
    require(fixture.cursorBytes == static_cast<int>(xdpu::kCursorMetaSize), "cursor metadata was omitted");
  }
} // namespace

extern "C" {
int __wrap_gbm_device_get_format_modifier_plane_count(gbm_device*, uint32_t, uint64_t) {
  ++fixture.queries;
  return fixture.queryResult;
}
gbm_bo*
__wrap_gbm_bo_create_with_modifiers2(gbm_device*, uint32_t, uint32_t, uint32_t, const uint64_t*, unsigned, uint32_t) {
  return allocate(false);
}
gbm_bo* __wrap_gbm_bo_create_with_modifiers(gbm_device*, uint32_t, uint32_t, uint32_t, const uint64_t*, unsigned) {
  return allocate(false);
}
gbm_bo* __wrap_gbm_bo_create(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t) { return allocate(true); }
uint64_t __wrap_gbm_bo_get_modifier(gbm_bo*) {
  return fixture.modifierMismatch ? DRM_FORMAT_MOD_LINEAR : fixture.modifier;
}
int __wrap_gbm_bo_get_plane_count(gbm_bo*) { return fixture.bufferPlanes; }
void __wrap_gbm_bo_destroy(gbm_bo*) { ++fixture.destroys; }
void __wrap_gbm_device_destroy(gbm_device*) {}
int __wrap_pw_stream_update_params(pw_stream*, const spa_pod** params, uint32_t count) {
  ++fixture.updates;
  for (uint32_t i = 0; i < count; ++i) {
    if (SPA_POD_OBJECT_ID(params[i]) == SPA_PARAM_Buffers) {
      spa_pod_parse_object(
          params[i], SPA_TYPE_OBJECT_ParamBuffers, nullptr, SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(&fixture.blocks)
      );
    } else if (SPA_POD_OBJECT_ID(params[i]) == SPA_PARAM_Meta) {
      uint32_t type = 0;
      int bytes = 0;
      spa_pod_parse_object(
          params[i], SPA_TYPE_OBJECT_ParamMeta, nullptr, SPA_PARAM_META_type, SPA_POD_Id(&type), SPA_PARAM_META_size,
          SPA_POD_Int(&bytes)
      );
      if (type == SPA_META_Cursor) {
        fixture.cursorBytes = bytes;
      }
    }
  }
  return fixture.updateResult;
}
}

int main() {
  // The implicit modifier is not queryable; use the buffer's actual plane count.
  fixture = {.queryResult = -1, .bufferPlanes = 1, .legacyOnly = true};
  require(configure(), "implicit DMA-BUF setup failed");
  cursorPresent(1);
  require(
      fixture.queries == 0 && fixture.allocations == 3 && fixture.destroys == 1,
      "implicit allocation fallback or lifetime is incorrect"
  );

  // Recover query failures for allocatable explicit formats without guessing
  // one plane: compressed layouts can need an auxiliary plane.
  for (int query : {0, -1}) {
    fixture = {.queryResult = query, .bufferPlanes = 2, .modifier = DRM_FORMAT_MOD_LINEAR};
    require(configure(), "allocatable explicit DMA-BUF setup failed");
    cursorPresent(2);
    require(fixture.queries == 1 && fixture.destroys == 1, "fallback buffer was not released");
  }

  fixture = {.queryResult = 2, .modifier = DRM_FORMAT_MOD_LINEAR};
  require(configure(), "queryable explicit DMA-BUF setup failed");
  cursorPresent(2);
  require(fixture.allocations == 0, "working explicit queries should not allocate a probe buffer");

  fixture = {.allocationFails = true};
  require(!configure() && fixture.updates == 0, "unallocatable format published incomplete parameters");
  require(fixture.destroys == 0, "failed allocation destroyed a nonexistent buffer");

  fixture = {.bufferPlanes = 0};
  require(!configure() && fixture.updates == 0, "invalid buffer plane count was accepted");
  require(fixture.destroys == 1, "invalid buffer leaked");

  fixture = {.modifierMismatch = true};
  require(!configure() && fixture.updates == 0, "a different buffer modifier was accepted");
  require(fixture.destroys == 1, "mismatched buffer leaked");

  fixture = {};
  require(configure(false), "shared-memory setup failed");
  cursorPresent(1);
  require(fixture.queries == 0 && fixture.allocations == 0, "shared-memory setup used GBM");

  fixture = {};
  require(configure(false, false), "hidden/embedded cursor setup failed");
  require(fixture.cursorBytes == 0, "cursor metadata was advertised when not requested");

  fixture = {.updateResult = -EIO};
  require(!configure(false), "PipeWire parameter update failure was ignored");
  std::puts(
      "PipeWire buffer negotiation: implicit layouts, query failures, plane counts, cursor metadata and errors passed"
  );
}
