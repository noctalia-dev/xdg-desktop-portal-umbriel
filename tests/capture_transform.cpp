#include "wayland/capture_transform.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "{}", message);
      std::exit(EXIT_FAILURE);
    }
  }
} // namespace

int main() {
  // Every source pixel is distinct, so each expected row detects both rotation
  // direction and flip composition independently of the coordinate helpers.
  constexpr std::array<std::array<uint32_t, 6>, 8> expected{{
      {1, 2, 3, 4, 5, 6},
      {4, 1, 5, 2, 6, 3},
      {6, 5, 4, 3, 2, 1},
      {3, 6, 2, 5, 1, 4},
      {3, 2, 1, 6, 5, 4},
      {1, 4, 2, 5, 3, 6},
      {4, 5, 6, 1, 2, 3},
      {6, 3, 5, 2, 4, 1},
  }};
  const std::array<uint32_t, 8> source{1, 2, 3, 0xabcdef01, 4, 5, 6, 0xabcdef01};
  for (uint32_t transform = 0; transform < 8; ++transform) {
    const auto dimensions = xdpu::displaySize(3, 2, transform);
    require(
        dimensions.width == (transform % 2 == 0 ? 3U : 2U) && dimensions.height == (transform % 2 == 0 ? 2U : 3U),
        "display dimensions did not swap for an odd transform"
    );
    const uint32_t stride = (dimensions.width + 1) * 4;
    std::array<uint32_t, 16> destination;
    destination.fill(0x76543210);
    require(
        xdpu::transformPacked32(
            source.data(), sizeof(source), 3, 2, 16, transform, destination.data(), sizeof(destination), stride
        ),
        "valid padded image was rejected"
    );
    for (uint32_t y = 0; y < dimensions.height; ++y) {
      for (uint32_t x = 0; x < dimensions.width; ++x) {
        require(
            destination[y * (dimensions.width + 1) + x] == expected[transform][y * dimensions.width + x],
            "normalized image pixels have the wrong orientation"
        );
      }
      require(destination[y * (dimensions.width + 1) + dimensions.width] == 0x76543210, "row padding was overwritten");
    }
    require(destination[dimensions.height * (dimensions.width + 1)] == 0x76543210, "image guard was overwritten");
  }

  // Hotspots describe edge-based offsets. The 0,0 hotspot differs by one from
  // reflecting pixel 0,0; outside offsets must not be clamped into the image.
  constexpr std::array<xdpu::BufferPoint, 8> origin{{{0, 0}, {2, 0}, {3, 2}, {0, 3}, {3, 0}, {0, 0}, {0, 2}, {2, 3}}};
  constexpr std::array<xdpu::BufferPoint, 8> outside{{
      {-2, 5},
      {-3, -2},
      {5, -3},
      {5, 5},
      {5, 5},
      {5, -2},
      {-2, -3},
      {-3, 5},
  }};
  for (uint32_t transform = 0; transform < 8; ++transform) {
    const auto a = xdpu::bufferToDisplay(0, 0, 3, 2, transform);
    const auto b = xdpu::bufferToDisplay(-2, 5, 3, 2, transform);
    require(a.x == origin[transform].x && a.y == origin[transform].y, "hotspot used pixel-index reflection");
    require(b.x == outside[transform].x && b.y == outside[transform].y, "outside hotspot was transformed incorrectly");
  }

  std::array<uint32_t, 16> destination{};
  require(
      !xdpu::transformPacked32(source.data(), sizeof(source), 3, 2, 16, 8, destination.data(), 64, 16),
      "invalid transform accepted"
  );
  require(
      !xdpu::transformPacked32(source.data(), 27, 3, 2, 16, 0, destination.data(), 64, 16), "short source accepted"
  );
  require(
      !xdpu::transformPacked32(source.data(), sizeof(source), 3, 2, 8, 0, destination.data(), 64, 16),
      "short source stride accepted"
  );
  require(
      !xdpu::transformPacked32(source.data(), sizeof(source), 3, 2, 16, 1, destination.data(), 27, 12),
      "short destination accepted"
  );
  require(
      !xdpu::transformPacked32(source.data(), sizeof(source), 3, 2, 16, 0, destination.data(), 64, 8),
      "short destination stride accepted"
  );
  require(
      !xdpu::transformPacked32(destination.data(), 64, 3, 2, 16, 2, destination.data() + 1, 60, 16),
      "overlapping storage accepted"
  );
  require(!xdpu::transformPacked32(nullptr, 64, 3, 2, 16, 0, destination.data(), 64, 16), "null source accepted");
  require(
      !xdpu::transformPacked32(source.data(), sizeof(source), 0, 2, 16, 0, destination.data(), 64, 16),
      "empty image accepted"
  );
  return EXIT_SUCCESS;
}
