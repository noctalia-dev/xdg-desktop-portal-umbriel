#pragma once

#include <functional>
#include <gdk/gdk.h>
#include <memory>
#include <string>
#include <vector>

namespace xdpu {

  struct PreviewSource {
    bool monitor;
    std::string identifier;
  };

  struct PreviewResult {
    size_t index;
    // Owned by the caller after takeResults(); null means capture was unavailable.
    GdkTexture* texture;
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
    void request(std::vector<size_t> indices);
    void stop();
    std::vector<PreviewResult> takeResults();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
