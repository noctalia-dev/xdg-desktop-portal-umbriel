#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace xdpu {

  struct BufferSize {
    uint32_t width = 0;
    uint32_t height = 0;
  };

  inline BufferSize displaySize(uint32_t width, uint32_t height, uint32_t transform) {
    return (transform & 1U) != 0 ? BufferSize{height, width} : BufferSize{width, height};
  }

  struct BufferPoint {
    int32_t x = 0;
    int32_t y = 0;
  };

  // Map a displayed pixel index onto the raw capture buffer. Reflected pixel
  // indices start at width - 1 or height - 1; odd transforms swap the axes.
  inline BufferPoint
  displayToBuffer(int32_t x, int32_t y, int32_t bufferWidth, int32_t bufferHeight, uint32_t transform) {
    const int32_t displayWidth = (transform & 1U) != 0 ? bufferHeight : bufferWidth;
    const int32_t fx = (transform & 4U) != 0 ? displayWidth - 1 - x : x;
    switch (transform & 3U) {
    case 1:
      return {y, bufferHeight - 1 - fx};
    case 2:
      return {bufferWidth - 1 - fx, bufferHeight - 1 - y};
    case 3:
      return {bufferWidth - 1 - y, fx};
    default:
      return {fx, y};
    }
  }

  struct BufferOffset {
    int64_t x = 0;
    int64_t y = 0;
  };

  // Cursor hotspots are geometric offsets, rather than pixel indices. Reflect
  // around the image edge, preserving coordinates outside the image too.
  inline BufferOffset
  bufferToDisplay(int32_t x, int32_t y, int32_t bufferWidth, int32_t bufferHeight, uint32_t transform) {
    BufferOffset point;
    const int64_t bx = x;
    const int64_t by = y;
    switch (transform & 3U) {
    case 1:
      point = {bufferHeight - by, bx};
      break;
    case 2:
      point = {bufferWidth - bx, bufferHeight - by};
      break;
    case 3:
      point = {by, bufferWidth - bx};
      break;
    default:
      point = {bx, by};
      break;
    }
    if ((transform & 4U) != 0) {
      const int32_t displayWidth = (transform & 1U) != 0 ? bufferHeight : bufferWidth;
      point.x = displayWidth - point.x;
    }
    return point;
  }

  // Normalize a packed 32-bit image into separate, possibly padded storage.
  // Channel order is preserved. Source and destination must not overlap.
  inline bool transformPacked32(
      const void* source, size_t sourceSize, uint32_t width, uint32_t height, uint32_t sourceStride, uint32_t transform,
      void* destination, size_t destinationSize, uint32_t destinationStride
  ) {
    if (source == nullptr
        || destination == nullptr
        || width == 0
        || height == 0
        || width > INT32_MAX
        || height > INT32_MAX
        || transform > 7) {
      return false;
    }
    const BufferSize displayed = displaySize(width, height, transform);
    const uint64_t sourceRow = static_cast<uint64_t>(width) * 4;
    const uint64_t destinationRow = static_cast<uint64_t>(displayed.width) * 4;
    if (sourceRow > sourceStride || destinationRow > destinationStride) {
      return false;
    }
    const uint64_t sourceRequired = static_cast<uint64_t>(height - 1) * sourceStride + sourceRow;
    const uint64_t destinationRequired =
        static_cast<uint64_t>(displayed.height - 1) * destinationStride + destinationRow;
    if (sourceRequired > sourceSize || destinationRequired > destinationSize) {
      return false;
    }
    const auto sourceAddress = reinterpret_cast<uintptr_t>(source);
    const auto destinationAddress = reinterpret_cast<uintptr_t>(destination);
    if (destinationAddress >= sourceAddress ? destinationAddress - sourceAddress < sourceRequired
                                            : sourceAddress - destinationAddress < destinationRequired) {
      return false;
    }
    const auto* input = static_cast<const uint8_t*>(source);
    auto* output = static_cast<uint8_t*>(destination);
    for (uint32_t y = 0; y < displayed.height; ++y) {
      auto* row = output + static_cast<size_t>(y) * destinationStride;
      if (transform == 0) {
        std::memcpy(row, input + static_cast<size_t>(y) * sourceStride, static_cast<size_t>(sourceRow));
        continue;
      }
      for (uint32_t x = 0; x < displayed.width; ++x) {
        const BufferPoint point = displayToBuffer(
            static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(width), static_cast<int32_t>(height),
            transform
        );
        const auto* pixel = input + static_cast<size_t>(point.y) * sourceStride + static_cast<size_t>(point.x) * 4;
        std::memcpy(row + static_cast<size_t>(x) * 4, pixel, 4);
      }
    }
    return true;
  }

} // namespace xdpu
