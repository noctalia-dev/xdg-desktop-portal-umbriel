#pragma once

#include <string>
#include <vector>

namespace xdpu::picker {

  struct OutputItem {
    std::string name;
    std::string title;
    std::string description;
    int width = 0;
    int height = 0;
    bool focused = false;
  };

  struct WindowItem {
    std::string identifier;
    std::string appId;
    std::string title;
  };

  // The chooser request the portal writes to stdin; see "Share chooser protocol" in CONTRIBUTING.md.
  struct ShareRequest {
    bool multiple = false;
    bool showMonitors = false;
    bool showWindows = false;
    std::vector<OutputItem> outputs;
    std::vector<WindowItem> windows;
  };

  std::string readStdin();
  // Invalid JSON yields an empty request; malformed entries are skipped or read as empty fields.
  ShareRequest parseRequest(const std::string& input);

} // namespace xdpu::picker
