#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xdpu {

  struct PreviewSource {
    bool monitor;
    std::string identifier;
  };

  // Tightly packed RGBA8, `width * 4` bytes per row.
  struct Thumbnail {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
  };

  struct PreviewResult {
    size_t index;
    // nullopt means capture was unavailable.
    std::optional<Thumbnail> thumbnail;
  };

  // One snapshot per requested source, captured sequentially on a private Wayland connection.
  // Full-size buffers are released before the next capture; only thumbnails survive.
  class Previews {
  public:
    // Invoked on the worker thread once a result is queued: post takeResults() to the UI thread.
    using ResultsReadyCallback = std::function<void()>;

    Previews(std::vector<PreviewSource> sources, ResultsReadyCallback onResultsReady);
    ~Previews();
    // Replace pending work with the currently visible indices. Already captured or
    // in-flight sources are ignored; hidden sources never open capture sessions.
    void request(const std::vector<size_t>& indices);
    void stop();
    std::vector<PreviewResult> takeResults();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
