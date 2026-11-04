#include "config/config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }
} // namespace

int main() {
  char directory[] = "/tmp/xdpu-config-test-XXXXXX";
  require(mkdtemp(directory) != nullptr, "cannot create temporary config directory");
  const std::filesystem::path root(directory);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  require(setenv("XDG_CONFIG_HOME", directory, 1) == 0, "cannot set config directory");
  require(!xdpu::loadConfig().screencast.independentSources, "missing config must retain dynamic sharing");

  std::filesystem::create_directories(root / "xdg-desktop-portal-umbriel");
  const auto path = root / "xdg-desktop-portal-umbriel" / "config.toml";
  auto write = [&](const std::string& content) {
    std::ofstream file(path);
    file << content;
    require(file.good(), "cannot write config");
  };
  write("[screencast]\nindependent_sources = true\n");
  require(xdpu::loadConfig().screencast.independentSources, "true must enable independent sources");
  write("[screencast]\nindependent_sources = false\n");
  require(!xdpu::loadConfig().screencast.independentSources, "false must retain dynamic sharing");
  write("[screencast]\nmax_fps = 30\n");
  const auto config = xdpu::loadConfig();
  require(!config.screencast.independentSources, "omitted option must retain dynamic sharing");
  require(config.screencast.maxFps == 30, "existing configuration must still load");
  write("[screencast]\nindependent_sources = \"true\"\n");
  require(!xdpu::loadConfig().screencast.independentSources, "non-boolean must not enable independent sources");
}
