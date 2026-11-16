#include "global_shortcuts_credentials.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace xdpu {
  namespace {
    using Status = GlobalShortcutCacheStatus;
    using Json = nlohmann::json;
    using Credentials = std::map<std::string, std::string>;
    constexpr size_t kMaxApplications = 128;
    constexpr size_t kMaxFileBytes = 4 * 1024 * 1024;
    constexpr size_t kMaxApplicationBytes = 256;
    constexpr size_t kMaxCredentialBytes = 4096;
    constexpr auto kLockName = ".global-shortcuts-credentials.lock";

    struct Fd {
      int value = -1;
      explicit Fd(int fd = -1) : value(fd) {}
      ~Fd() {
        if (value >= 0) {
          close(value);
        }
      }
      Fd(const Fd&) = delete;
      Fd& operator=(const Fd&) = delete;
    };

    bool validText(std::string_view value, size_t limit) {
      if (value.empty() || value.size() > limit || value.contains('\0')) {
        return false;
      }
      try {
        (void)Json(std::string(value)).dump();
        return true;
      } catch (const Json::exception&) {
        return false;
      }
    }

    bool privateFile(int fd, struct stat& st) {
      return fstat(fd, &st) == 0
          && S_ISREG(st.st_mode)
          && st.st_uid == geteuid()
          && (st.st_mode & 0077) == 0
          && st.st_nlink == 1;
    }

    Status lockRange(int fd, off_t position, short type) {
      struct flock lock{};
      lock.l_type = type;
      lock.l_whence = SEEK_SET;
      lock.l_start = position;
      lock.l_len = 1;
      if (fcntl(fd, F_OFD_SETLK, &lock) == 0) {
        return Status::Ready;
      }
      return errno == EAGAIN || errno == EACCES ? Status::Busy : Status::Failed;
    }

    struct FileLock {
      int fd;
      Status status;
      explicit FileLock(int fd) : fd(fd), status(lockRange(fd, 0, F_WRLCK)) {}
      ~FileLock() {
        if (status == Status::Ready) {
          (void)lockRange(fd, 0, F_UNLCK);
        }
      }
      FileLock(const FileLock&) = delete;
      FileLock& operator=(const FileLock&) = delete;
    };

    off_t applicationLock(std::string_view application) {
      uint64_t hash = 14695981039346656037ULL;
      for (const unsigned char byte : application) {
        hash = (hash ^ byte) * 1099511628211ULL;
      }
      // Hash collisions only serialize unrelated applications. Exact IDs select
      // credentials. Byte zero protects the shared file; OFD locks also exclude
      // other transactions in this process and disappear on descriptor close.
      return static_cast<off_t>((hash & 0x3FFFFFFFFFFFFFFFULL) + 1);
    }

    bool readCredentials(int directory, const std::string& filename, Credentials& credentials) {
      const Fd file(openat(directory, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
      if (file.value < 0) {
        return errno == ENOENT;
      }
      struct stat st{};
      if (!privateFile(file.value, st) || st.st_size < 0 || static_cast<uint64_t>(st.st_size) > kMaxFileBytes) {
        return false;
      }
      std::string contents(static_cast<size_t>(st.st_size), '\0');
      size_t offset = 0;
      while (offset < contents.size()) {
        const auto count = read(file.value, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
        offset += static_cast<size_t>(count);
      }
      char extra = 0;
      ssize_t count = 0;
      do {
        count = read(file.value, &extra, 1);
      } while (count < 0 && errno == EINTR);
      if (count != 0) {
        return false;
      }

      // Reject duplicate object keys rather than accepting a parser's last value.
      bool uniqueKeys = true;
      std::vector<std::set<std::string>> keys;
      const auto parsed = Json::parse(
          contents,
          [&](int, Json::parse_event_t event, Json& value) {
            if (event == Json::parse_event_t::object_start) {
              keys.emplace_back();
            } else if (event == Json::parse_event_t::key) {
              uniqueKeys &= keys.back().insert(value.get<std::string>()).second;
            } else if (event == Json::parse_event_t::object_end) {
              keys.pop_back();
            }
            return true;
          },
          false
      );
      if (!uniqueKeys
          || !parsed.is_object()
          || parsed.size() != 2
          || !parsed.contains("version")
          || !parsed["version"].is_number_unsigned()
          || parsed["version"] != 1
          || !parsed.contains("credentials")
          || !parsed["credentials"].is_array()
          || parsed["credentials"].size() > kMaxApplications) {
        return false;
      }
      for (const auto& entry : parsed["credentials"]) {
        if (!entry.is_object()
            || entry.size() != 2
            || !entry.contains("application")
            || !entry["application"].is_string()
            || !entry.contains("credential")
            || !entry["credential"].is_string()) {
          return false;
        }
        const auto& application = entry["application"].get_ref<const std::string&>();
        const auto& credential = entry["credential"].get_ref<const std::string&>();
        if (!validText(application, kMaxApplicationBytes)
            || !validText(credential, kMaxCredentialBytes)
            || !credentials.emplace(application, credential).second) {
          return false;
        }
      }
      return true;
    }

    bool writeCredentials(int directory, const std::string& filename, const Credentials& credentials) {
      Json entries = Json::array();
      for (const auto& [application, credential] : credentials) {
        entries.push_back({{"application", application}, {"credential", credential}});
      }
      const auto contents = Json{{"version", 1}, {"credentials", std::move(entries)}}.dump() + '\n';
      if (contents.size() > kMaxFileBytes) {
        return false;
      }
      std::array<unsigned char, 16> random{};
      size_t offset = 0;
      while (offset < random.size()) {
        const auto count = getrandom(random.data() + offset, random.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
        offset += static_cast<size_t>(count);
      }
      std::string temporary = ".global-shortcuts-credentials.";
      constexpr std::string_view kHex = "0123456789abcdef";
      for (const auto byte : random) {
        temporary += kHex[byte >> 4];
        temporary += kHex[byte & 15];
      }
      temporary += ".tmp";
      Fd file(openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
      if (file.value < 0) {
        return false;
      }
      bool written = fchmod(file.value, 0600) == 0;
      offset = 0;
      while (written && offset < contents.size()) {
        const auto count = write(file.value, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          written = false;
          break;
        }
        offset += static_cast<size_t>(count);
      }
      written = written && fsync(file.value) == 0;
      const auto closed = close(std::exchange(file.value, -1));
      if (!written || closed != 0 || renameat(directory, temporary.c_str(), directory, filename.c_str()) != 0) {
        unlinkat(directory, temporary.c_str(), 0);
        return false;
      }
      // Rename commits the rotation. A later directory-sync error cannot leave
      // the caller believing the previous credential is still on disk.
      (void)fsync(directory);
      return true;
    }
  } // namespace

  struct GlobalShortcutCredentials::Transaction::Impl {
    std::filesystem::path parent;
    std::string filename;
    std::string application;
    Fd directory;
    Fd lock;
    std::optional<std::string> credential;

    bool currentDirectory() const {
      struct stat opened{};
      struct stat named{};
      struct stat lockStat{};
      struct stat namedLock{};
      return fstat(directory.value, &opened) == 0
          && lstat(parent.c_str(), &named) == 0
          && S_ISDIR(named.st_mode)
          && named.st_uid == geteuid()
          && (named.st_mode & 0077) == 0
          && opened.st_dev == named.st_dev
          && opened.st_ino == named.st_ino
          && privateFile(lock.value, lockStat)
          && fstatat(directory.value, kLockName, &namedLock, AT_SYMLINK_NOFOLLOW) == 0
          && lockStat.st_dev == namedLock.st_dev
          && lockStat.st_ino == namedLock.st_ino;
    }
  };

  std::filesystem::path GlobalShortcutCredentials::defaultPath() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
      const std::filesystem::path path(state);
      if (path.is_absolute()) {
        return path / "xdg-desktop-portal-umbriel/global-shortcuts-credentials.json";
      }
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      const std::filesystem::path path(home);
      if (path.is_absolute()) {
        return path / ".local/state/xdg-desktop-portal-umbriel/global-shortcuts-credentials.json";
      }
    }
    return {};
  }

  GlobalShortcutCredentials::GlobalShortcutCredentials(std::filesystem::path path) : m_path(std::move(path)) {}

  GlobalShortcutCredentials::Acquisition GlobalShortcutCredentials::acquire(std::string_view application) const {
    if (!validText(application, kMaxApplicationBytes) || !m_path.is_absolute() || m_path.native().contains('\0')) {
      return {};
    }
    const auto filename = m_path.filename().string();
    if (filename.empty() || filename == "." || filename == ".." || filename == kLockName) {
      return {};
    }
    try {
      auto state = std::make_unique<Transaction::Impl>();
      state->parent = m_path.parent_path();
      state->filename = filename;
      state->application = application;
      std::error_code error;
      std::filesystem::create_directories(state->parent, error);
      if (error) {
        return {};
      }
      state->directory.value = open(state->parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      struct stat st{};
      if (state->directory.value < 0
          || fstat(state->directory.value, &st) != 0
          || st.st_uid != geteuid()
          || fchmod(state->directory.value, 0700) != 0) {
        return {};
      }
      state->lock.value =
          openat(state->directory.value, kLockName, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
      if (state->lock.value < 0
          || !privateFile(state->lock.value, st)
          || fchmod(state->lock.value, 0600) != 0
          || !state->currentDirectory()) {
        return {};
      }
      const auto appStatus = lockRange(state->lock.value, applicationLock(application), F_WRLCK);
      if (appStatus != Status::Ready) {
        return {appStatus, nullptr};
      }
      const FileLock fileLock(state->lock.value);
      if (fileLock.status != Status::Ready) {
        return {fileLock.status, nullptr};
      }
      Credentials credentials;
      if (!readCredentials(state->directory.value, filename, credentials)) {
        return {};
      }
      if (const auto it = credentials.find(state->application); it != credentials.end()) {
        state->credential = it->second;
      }
      return {Status::Ready, std::make_unique<Transaction>(std::move(state))};
    } catch (const Json::exception&) {
      return {};
    }
  }

  GlobalShortcutCredentials::Transaction::Transaction(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
  GlobalShortcutCredentials::Transaction::~Transaction() = default;

  const std::optional<std::string>& GlobalShortcutCredentials::Transaction::credential() const {
    return m_impl->credential;
  }

  GlobalShortcutCacheStatus GlobalShortcutCredentials::Transaction::save(std::string_view credential) {
    if (!validText(credential, kMaxCredentialBytes)) {
      return Status::Failed;
    }
    return update(credential);
  }

  GlobalShortcutCacheStatus GlobalShortcutCredentials::Transaction::clear() { return update(std::nullopt); }

  GlobalShortcutCacheStatus GlobalShortcutCredentials::Transaction::update(std::optional<std::string_view> credential) {
    const FileLock fileLock(m_impl->lock.value);
    if (fileLock.status != Status::Ready) {
      return fileLock.status;
    }
    try {
      Credentials credentials;
      if (!m_impl->currentDirectory() || !readCredentials(m_impl->directory.value, m_impl->filename, credentials)) {
        return Status::Failed;
      }
      const auto before = credentials.find(m_impl->application);
      const std::optional<std::string> replacement =
          credential ? std::optional(std::string(*credential)) : std::nullopt;
      if (before != credentials.end() ? replacement && before->second == *replacement : !replacement) {
        m_impl->credential = replacement;
        return Status::Ready;
      }
      if (replacement) {
        if (before == credentials.end() && credentials.size() >= kMaxApplications) {
          return Status::Failed;
        }
        credentials[m_impl->application] = *replacement;
      } else {
        credentials.erase(m_impl->application);
      }
      if (!writeCredentials(m_impl->directory.value, m_impl->filename, credentials)) {
        return Status::Failed;
      }
      m_impl->credential = replacement;
      return Status::Ready;
    } catch (const Json::exception&) {
      return Status::Failed;
    }
  }
} // namespace xdpu
