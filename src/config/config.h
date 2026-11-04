#pragma once
#include "defaults.h"

#include <functional>
#include <memory>
#include <string>

namespace xdpu {

  class Loop;

  struct Config {
    struct Screencast {
      std::string chooserCmd = XDPU_DEFAULT_CHOOSER_CMD;
      int maxFps = 0;
      bool independentSources = false;

      bool operator==(const Screencast&) const = default;
    } screencast;

    struct Screenshot {
      std::string cmd;
      std::string colorPickCmd;

      bool operator==(const Screenshot&) const = default;
    } screenshot;

    bool operator==(const Config&) const = default;
  };

  Config loadConfig();

  class ConfigWatcher {
  public:
    ConfigWatcher(Loop& loop, std::function<void(const Config& oldCfg, const Config& newCfg)> onChange);
    ~ConfigWatcher();

    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;
    ConfigWatcher(ConfigWatcher&&) = delete;
    ConfigWatcher& operator=(ConfigWatcher&&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
