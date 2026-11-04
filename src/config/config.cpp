#include "config/config.h"

#include "loop/loop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <toml++/toml.hpp>
#include <unistd.h>
#include <utility>

namespace xdpu {

  namespace {

    constexpr int kDebounceMs = 50;
    constexpr uint32_t kFileWatchMask =
        IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED;
    constexpr uint32_t kDirWatchMask =
        IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF | IN_DELETE | IN_MOVED_FROM | IN_IGNORED;

    void closeFd(int fd) {
      if (fd >= 0) {
        while (::close(fd) < 0 && errno == EINTR) {
        }
      }
    }

    std::filesystem::path configPath() {
      if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
          xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
        return std::filesystem::path(xdgConfigHome) / "xdg-desktop-portal-umbriel" / "config.toml";
      }

      if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "xdg-desktop-portal-umbriel" / "config.toml";
      }

      return std::filesystem::path(".config") / "xdg-desktop-portal-umbriel" / "config.toml";
    }

    std::filesystem::path normalized(const std::filesystem::path& path) {
      std::error_code error;
      auto absolute = std::filesystem::absolute(path, error);
      if (error) {
        absolute = path;
      }
      return absolute.lexically_normal();
    }

    bool eventNameMatches(const inotify_event& event, std::string_view name) {
      return event.len > 0 && name == std::string_view(event.name);
    }

    void readString(const toml::table& table, std::string_view tableName, std::string_view key, std::string& target) {
      if (const auto value = table[tableName][key].value<std::string>()) {
        target = *value;
      }
    }

    void readInt(const toml::table& table, std::string_view tableName, std::string_view key, int& target) {
      if (const auto value = table[tableName][key].value<int64_t>()) {
        if (*value >= std::numeric_limits<int>::min() && *value <= std::numeric_limits<int>::max()) {
          target = static_cast<int>(*value);
        }
      }
    }

  } // namespace

  Config loadConfig() {
    Config config;
    const auto path = configPath();

    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
      return config;
    }

    try {
      const toml::table table = toml::parse_file(path.string());

      readString(table, "screencast", "chooser_cmd", config.screencast.chooserCmd);
      readInt(table, "screencast", "max_fps", config.screencast.maxFps);
      if (const auto value = table["screencast"]["independent_sources"].value<bool>()) {
        config.screencast.independentSources = *value;
      }

      readString(table, "screenshot", "cmd", config.screenshot.cmd);
      readString(table, "screenshot", "color_pick_cmd", config.screenshot.colorPickCmd);

    } catch (const toml::parse_error& error) {
      const std::string description(error.description());
      std::fprintf(stderr, "config: unable to parse %s: %s\n", path.c_str(), description.c_str());
    }

    return config;
  }

  struct ConfigWatcher::Impl {
    Impl(Loop& loop, std::function<void(const Config& oldCfg, const Config& newCfg)> onChange)
        : loop(loop), onChange(std::move(onChange)), path(normalized(configPath())), configDir(path.parent_path()),
          configDirName(configDir.filename().string()), current(loadConfig()) {
      fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (fd < 0) {
        std::fprintf(stderr, "config: inotify unavailable; hot reload disabled: %s\n", std::strerror(errno));
        return;
      }

      establishWatches();

      fdWatchId = loop.addFd(fd, EPOLLIN, [this](uint32_t) { handleInotify(); });
      if (fdWatchId == 0) {
        std::fprintf(stderr, "config: unable to register inotify fd with loop; hot reload disabled\n");
        closeFd(fd);
        fd = -1;
      }
    }

    ~Impl() {
      if (timerId != 0) {
        loop.removeTimer(timerId);
        timerId = 0;
      }
      if (fdWatchId != 0) {
        loop.removeFd(fdWatchId);
        fdWatchId = 0;
      }
      if (fd >= 0) {
        if (fileWatch >= 0) {
          ::inotify_rm_watch(fd, fileWatch);
        }
        if (dirWatch >= 0) {
          ::inotify_rm_watch(fd, dirWatch);
        }
        if (ancestorWatch >= 0) {
          ::inotify_rm_watch(fd, ancestorWatch);
        }
        closeFd(fd);
        fd = -1;
      }
    }

    // Set up the full watch chain.  When the config directory doesn't exist,
    // watch the nearest existing ancestor so we notice its creation.
    void establishWatches() {
      addDirectoryWatch();
      addFileWatch();

      // If the config directory didn't exist (dirWatch < 0), watch the
      // nearest ancestor that does so we detect mkdir/rename-into.
      if (dirWatch < 0) {
        addAncestorWatch();
      } else {
        removeAncestorWatch();
      }
    }

    void addDirectoryWatch() {
      if (fd < 0) {
        return;
      }

      if (dirWatch >= 0) {
        ::inotify_rm_watch(fd, dirWatch);
        dirWatch = -1;
      }

      if (configDir.empty()) {
        return;
      }

      dirWatch = ::inotify_add_watch(fd, configDir.c_str(), kDirWatchMask);
      if (dirWatch < 0 && errno != ENOENT) {
        std::fprintf(stderr, "config: unable to watch directory %s: %s\n", configDir.c_str(), std::strerror(errno));
      }
    }

    void addFileWatch() {
      if (fd < 0) {
        return;
      }

      if (fileWatch >= 0) {
        ::inotify_rm_watch(fd, fileWatch);
        fileWatch = -1;
      }

      fileWatch = ::inotify_add_watch(fd, path.c_str(), kFileWatchMask);
      if (fileWatch < 0 && errno != ENOENT) {
        std::fprintf(stderr, "config: unable to watch file %s: %s\n", path.c_str(), std::strerror(errno));
      }
    }

    void addAncestorWatch() {
      if (fd < 0) {
        return;
      }

      removeAncestorWatch();

      // Walk up from the config directory until we find one that exists.
      std::error_code ec;
      for (auto candidate = configDir.parent_path(); !candidate.empty() && candidate != candidate.parent_path();
           candidate = candidate.parent_path()) {
        if (!std::filesystem::is_directory(candidate, ec)) {
          continue;
        }
        ancestorWatch = ::inotify_add_watch(fd, candidate.c_str(), kDirWatchMask);
        if (ancestorWatch >= 0) {
          ancestorPath = candidate;
          return;
        }
        if (errno != ENOENT) {
          std::fprintf(stderr, "config: unable to watch ancestor %s: %s\n", candidate.c_str(), std::strerror(errno));
          return;
        }
      }
    }

    void removeAncestorWatch() {
      if (fd >= 0 && ancestorWatch >= 0) {
        ::inotify_rm_watch(fd, ancestorWatch);
        ancestorWatch = -1;
        ancestorPath.clear();
      }
    }

    void scheduleReload() {
      if (timerId != 0) {
        loop.removeTimer(timerId);
        timerId = 0;
      }

      timerId = loop.addTimer(kDebounceMs, [this] {
        timerId = 0;
        reload();
      });
    }

    void reload() {
      const Config next = loadConfig();
      if (next == current) {
        return;
      }

      const Config previous = current;
      current = next;
      if (onChange) {
        onChange(previous, current);
      }
    }

    void handleEvent(const inotify_event& event, bool& changed, bool& rewatchFile, bool& rewatchAll) {
      // Event on the config file itself.
      if (event.wd == fileWatch) {
        if ((event.mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0) {
          changed = true;
        }
        if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0) {
          fileWatch = -1;
          rewatchFile = true;
        }
        return;
      }

      // Event on the config directory.
      if (event.wd == dirWatch) {
        if ((event.mask & IN_IGNORED) != 0) {
          dirWatch = -1;
          rewatchAll = true;
          return;
        }
        if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
          // Directory removed — need to fall back to ancestor watch.
          dirWatch = -1;
          rewatchAll = true;
          changed = true;
          return;
        }
        if (!eventNameMatches(event, path.filename().string())) {
          return;
        }
        if ((event.mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM)) != 0) {
          changed = true;
        }
        if ((event.mask & (IN_MOVED_TO | IN_CREATE | IN_CLOSE_WRITE)) != 0) {
          rewatchFile = true;
        }
        return;
      }

      // Event on the ancestor directory — config dir may have been created.
      if (event.wd == ancestorWatch) {
        if ((event.mask & IN_IGNORED) != 0) {
          ancestorWatch = -1;
          rewatchAll = true;
          return;
        }
        // Check whether the event is for our config directory name, or if
        // the ancestor itself was deleted (re-watch higher).
        if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
          ancestorWatch = -1;
          rewatchAll = true;
          return;
        }
        if (eventNameMatches(event, configDirName) && (event.mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
          // Config directory appeared — try the full watch chain.
          rewatchAll = true;
          changed = true;
        }
      }
    }

    void handleInotify() {
      alignas(inotify_event) char buffer[4096];
      bool changed = false;
      bool rewatchFile = false;
      bool rewatchAll = false;

      while (true) {
        const ssize_t size = ::read(fd, buffer, sizeof(buffer));
        if (size < 0) {
          if (errno == EINTR) {
            continue;
          }
          if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::fprintf(stderr, "config: inotify read failed: %s\n", std::strerror(errno));
          }
          break;
        }
        if (size == 0) {
          break;
        }

        size_t offset = 0;
        while (offset < static_cast<size_t>(size)) {
          const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
          if ((event->mask & IN_Q_OVERFLOW) != 0) {
            changed = true;
            rewatchAll = true;
          } else {
            handleEvent(*event, changed, rewatchFile, rewatchAll);
          }
          offset += sizeof(inotify_event) + event->len;
        }
      }

      if (rewatchAll) {
        establishWatches();
      } else if (rewatchFile) {
        addFileWatch();
      }
      if (changed) {
        scheduleReload();
      }
    }

    Loop& loop;
    std::function<void(const Config& oldCfg, const Config& newCfg)> onChange;
    std::filesystem::path path;
    std::filesystem::path configDir;
    std::string configDirName;
    Config current;
    int fd = -1;
    int fdWatchId = 0;
    int fileWatch = -1;
    int dirWatch = -1;
    int ancestorWatch = -1;
    std::filesystem::path ancestorPath;
    int timerId = 0;
  };

  ConfigWatcher::ConfigWatcher(Loop& loop, std::function<void(const Config& oldCfg, const Config& newCfg)> onChange)
      : m_impl(std::make_unique<Impl>(loop, std::move(onChange))) {}

  ConfigWatcher::~ConfigWatcher() = default;

} // namespace xdpu
