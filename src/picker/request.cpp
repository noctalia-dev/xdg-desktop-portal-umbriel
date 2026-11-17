#include "picker/request.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <print>

using nlohmann::json;

namespace xdpu::picker {
  namespace {
    std::string jsonString(const json& object, const char* key) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    int jsonInt(const json& object, const char* key) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_number_integer()) {
        return 0;
      }
      return it->get<int>();
    }

    bool jsonBool(const json& object, const char* key) {
      const auto it = object.find(key);
      return it != object.end() && it->is_boolean() && it->get<bool>();
    }
  } // namespace

  std::string readStdin() {
    std::string input;
    char buffer[4096];
    while (std::cin.good()) {
      std::cin.read(buffer, sizeof(buffer));
      input.append(buffer, static_cast<size_t>(std::cin.gcount()));
    }
    return input;
  }

  ShareRequest parseRequest(const std::string& input) {
    ShareRequest request;

    try {
      const json root = json::parse(input);
      if (!root.is_object()) {
        return request;
      }

      request.multiple = jsonBool(root, "multiple");

      const auto types = root.find("types");
      if (types != root.end() && types->is_array()) {
        for (const json& type : *types) {
          if (!type.is_string()) {
            continue;
          }
          const std::string value = type.get<std::string>();
          request.showMonitors = request.showMonitors || value == "monitor";
          request.showWindows = request.showWindows || value == "window";
        }
      }

      const auto outputs = root.find("outputs");
      if (outputs != root.end() && outputs->is_array()) {
        for (const json& output : *outputs) {
          if (!output.is_object()) {
            continue;
          }
          request.outputs.push_back({
              .name = jsonString(output, "name"),
              .title = jsonString(output, "title"),
              .description = jsonString(output, "description"),
              .width = jsonInt(output, "width"),
              .height = jsonInt(output, "height"),
              .focused = jsonBool(output, "focused"),
          });
        }
      }

      const auto windows = root.find("windows");
      if (windows != root.end() && windows->is_array()) {
        for (const json& window : *windows) {
          if (!window.is_object()) {
            continue;
          }
          request.windows.push_back({
              .identifier = jsonString(window, "identifier"),
              .appId = jsonString(window, "app_id"),
              .title = jsonString(window, "title"),
          });
        }
      }
    } catch (const json::exception& error) {
      std::println(stderr, "umbriel-share-picker: invalid request JSON: {}", error.what());
    }

    return request;
  }

} // namespace xdpu::picker
