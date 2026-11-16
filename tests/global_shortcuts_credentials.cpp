#include "dbus/global_shortcuts_credentials.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using Cache = xdpu::GlobalShortcutCredentials;
  using Status = xdpu::GlobalShortcutCacheStatus;
  using Json = nlohmann::json;
  constexpr auto kLockName = ".global-shortcuts-credentials.lock";
  enum class Fault : uint8_t { None, Write, FileSync, Rename, DirectorySync };
  Fault fault = Fault::None;
  bool faultHit = false;

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct Temporary {
    std::filesystem::path root;
    std::filesystem::path path;
    Temporary() {
      std::array<char, 48> name{};
      std::string_view("/tmp/xdpu-shortcut-cache-XXXXXX").copy(name.data(), name.size());
      const auto* created = mkdtemp(name.data());
      require(created != nullptr, "temporary directory creation failed");
      root = created;
      path = root / "state/credentials.json";
    }
    ~Temporary() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
    Temporary(const Temporary&) = delete;
    Temporary& operator=(const Temporary&) = delete;

    void write(std::string_view contents) const {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream file(path);
      file << contents;
      file.close();
      require(file.good() && chmod(path.c_str(), 0600) == 0, "fixture write failed");
    }
    std::string read() const {
      std::ifstream file(path);
      return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
  };

  std::unique_ptr<Cache::Transaction> acquire(const Cache& cache, std::string_view application) {
    auto result = cache.acquire(application);
    require(result.status == Status::Ready && result.transaction != nullptr, "cache acquisition failed");
    return std::move(result.transaction);
  }

  void expectStatus(const Cache& cache, std::string_view application, Status status, const char* message) {
    auto result = cache.acquire(application);
    require(result.status == status && result.transaction == nullptr, message);
  }

  void basicAndConcurrentUpdates() {
    const Temporary files;
    const Cache cache(files.path);
    auto first = acquire(cache, "org.example.First");
    require(!first->credential(), "missing file invented a credential");
    require(first->clear() == Status::Ready && !std::filesystem::exists(files.path), "empty clear wrote state");
    require(first->save("first-credential") == Status::Ready, "initial save failed");
    require(first->credential() == "first-credential", "initial save did not update snapshot");
    require(first->save("first-credential") == Status::Ready, "idempotent save failed");
    struct stat st{};
    require(
        stat(files.path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_uid == geteuid(),
        "credential file is not private"
    );
    require(
        stat(files.path.parent_path().c_str(), &st) == 0 && (st.st_mode & 0777) == 0700,
        "credential directory is not private"
    );
    require(
        stat((files.path.parent_path() / kLockName).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
        "lock file is not private"
    );

    const Cache otherCache(files.path);
    expectStatus(otherCache, "org.example.First", Status::Busy, "same-process application operation was not excluded");
    auto second = acquire(otherCache, "org.example.Second");
    require(second->save("second-credential") == Status::Ready, "independent application save failed");
    require(first->save("replacement") == Status::Ready, "credential rotation failed");
    second.reset();
    second = acquire(Cache(files.path), "org.example.Second");
    require(second->credential() == "second-credential", "rotation lost another application's update");
    first.reset();
    first = acquire(cache, "org.example.First");
    require(first->credential() == "replacement", "restart did not load replacement");
    require(second->clear() == Status::Ready && !second->credential(), "clear did not remove snapshot");
    require(first->save("after-clear") == Status::Ready, "save after unrelated clear failed");
    second.reset();
    second = acquire(cache, "org.example.Second");
    require(!second->credential(), "save restored an unrelated cleared credential");
    const auto document = Json::parse(files.read());
    require(
        document.size() == 2 && document["credentials"].size() == 1 && document["credentials"][0].size() == 2,
        "cache stored data beyond application credentials"
    );

    auto exactId = acquire(cache, "org.example.First/../Other");
    require(
        !exactId->credential() && exactId->save("opaque/nonhex/+credential") == Status::Ready,
        "application IDs were treated as paths or credentials were interpreted"
    );
    exactId.reset();
    exactId = acquire(cache, "org.example.First/../Other");
    require(exactId->credential() == "opaque/nonhex/+credential", "exact application key did not survive restart");
  }

  void fileContention() {
    const Temporary files;
    const Cache cache(files.path);
    auto transaction = acquire(cache, "app");
    require(transaction->save("old") == Status::Ready, "contention fixture save failed");
    const auto before = files.read();
    const int fd = open((files.path.parent_path() / kLockName).c_str(), O_RDWR | O_CLOEXEC);
    require(fd >= 0, "file lock open failed");
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_len = 1;
    require(fcntl(fd, F_OFD_SETLK, &lock) == 0, "file lock acquisition failed");
    expectStatus(cache, "other", Status::Busy, "file contention did not report busy on acquisition");
    require(
        transaction->save("new") == Status::Busy && transaction->credential() == "old" && files.read() == before,
        "contended save changed state or failed to report busy"
    );
    require(
        transaction->clear() == Status::Busy && transaction->credential() == "old", "contended clear changed state"
    );
    require(close(fd) == 0, "file lock close failed");
    require(transaction->save("new") == Status::Ready, "retry after file unlock failed");
    transaction.reset();
    require(acquire(cache, "app")->credential() == "new", "retry did not persist replacement");
    require(acquire(cache, "other")->save("other") == Status::Ready, "busy acquisition leaked its app lock");
  }

  void processLocking() {
    const Temporary files;
    const Cache cache(files.path);
    auto transaction = acquire(cache, "app");
    require(transaction->save("parent") == Status::Ready, "process locking fixture save failed");
    const auto child = fork();
    require(child >= 0, "lock contender fork failed");
    if (child == 0) {
      const auto result = cache.acquire("app");
      _exit(result.status == Status::Busy && result.transaction == nullptr ? 0 : 1);
    }
    int status = 0;
    require(
        waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "application lock did not exclude another process"
    );
    transaction.reset();

    std::array<int, 2> signal{};
    require(pipe2(signal.data(), O_CLOEXEC) == 0, "lease signal pipe failed");
    const auto holder = fork();
    require(holder >= 0, "lease holder fork failed");
    if (holder == 0) {
      close(signal[0]);
      auto result = cache.acquire("app");
      const char ready = result.status == Status::Ready ? '1' : '0';
      (void)write(signal[1], &ready, 1);
      _exit(ready == '1' ? 0 : 1);
    }
    close(signal[1]);
    char ready = 0;
    require(read(signal[0], &ready, 1) == 1 && ready == '1', "child could not acquire released lease");
    close(signal[0]);
    require(
        waitpid(holder, &status, 0) == holder && WIFEXITED(status) && WEXITSTATUS(status) == 0, "lease holder failed"
    );
    require(acquire(cache, "app")->credential() == "parent", "process exit did not release application lease");
  }

  void invalidState() {
    const Temporary files;
    const Cache cache(files.path);
    const std::array<std::string, 16> malformed{
        {"", "not json", "[]", "{}", R"({"version":2,"credentials":[]})", R"({"version":1.0,"credentials":[]})",
         R"({"version":1,"credentials":{}})", R"({"version":1,"credentials":[],"extra":0})",
         R"({"version":1,"version":1,"credentials":[]})", R"({"version":1,"credentials":[{}]})",
         R"({"version":1,"credentials":[{"application":"","credential":"opaque"}]})",
         R"({"version":1,"credentials":[{"application":"app","credential":""}]})",
         R"({"version":1,"credentials":[{"application":"app","credential":4}]})",
         R"({"version":1,"credentials":[{"application":"app","credential":"one","extra":0}]})",
         R"({"version":1,"credentials":[{"application":"app","credential":"one","credential":"two"}]})",
         "{\"version\":1,\"credentials\":[{\"application\":\"app\",\"credential\":\"one\"},{\"application\":\"app\","
         "\"credential\":\"two\"}]}"}
    };
    for (const auto& contents : malformed) {
      files.write(contents);
      expectStatus(cache, "app", Status::Failed, "malformed cache accepted");
      require(files.read() == contents, "malformed cache was overwritten");
    }
    for (const auto& invalid : {std::string(257, 'a'), std::string("a\0b", 3), std::string(1, '\xFF')}) {
      const Json record = {{"application", invalid}, {"credential", "opaque"}};
      if (invalid.back() != '\xFF') {
        files.write(Json{{"version", 1}, {"credentials", Json::array({record})}}.dump());
        expectStatus(cache, "app", Status::Failed, "invalid stored application accepted");
      }
      expectStatus(cache, invalid, Status::Failed, "invalid application accepted");
    }
    expectStatus(cache, "", Status::Failed, "empty application accepted");
    for (const auto& invalid : {std::string(4097, 'a'), std::string("a\0b", 3)}) {
      files.write(
          Json{{"version", 1}, {"credentials", Json::array({{{"application", "app"}, {"credential", invalid}}})}}.dump()
      );
      expectStatus(cache, "app", Status::Failed, "invalid stored credential accepted");
    }
    files.write("{\"version\":1,\"credentials\":[{\"application\":\"app\",\"credential\":\"\xFF\"}]}");
    expectStatus(cache, "app", Status::Failed, "invalid UTF-8 accepted");
    files.write(std::string(4 * 1024 * 1024 + 1, ' '));
    expectStatus(cache, "app", Status::Failed, "oversized cache accepted");
    files.write(R"({"version":1,"credentials":[]})");
    auto transaction = acquire(cache, "app");
    require(transaction->save("old") == Status::Ready, "validation fixture save failed");
    for (const auto& invalid :
         {std::string(), std::string(4097, 'a'), std::string("a\0b", 3), std::string(1, '\xFF')}) {
      require(
          transaction->save(invalid) == Status::Failed && transaction->credential() == "old",
          "invalid replacement changed the snapshot"
      );
    }
    files.write("malformed after acquisition");
    const auto before = files.read();
    require(
        transaction->save("new") == Status::Failed
            && transaction->clear() == Status::Failed
            && transaction->credential() == "old"
            && files.read() == before,
        "transaction overwrote newly malformed state"
    );
    expectStatus(Cache("relative/credentials.json"), "app", Status::Failed, "relative path accepted");
    expectStatus(Cache(files.path.parent_path() / kLockName), "app", Status::Failed, "cache used lock filename");
  }

  void unsafeFiles() {
    const Temporary files;
    const Cache cache(files.path);
    files.write(R"({"version":1,"credentials":[]})");
    const auto contents = files.read();
    require(chmod(files.path.c_str(), 0644) == 0, "unsafe file mode setup failed");
    expectStatus(cache, "app", Status::Failed, "readable credential file accepted");
    require(files.read() == contents, "unsafe file was changed");
    require(chmod(files.path.c_str(), 0600) == 0, "file mode restore failed");
    const auto victim = files.root / "victim";
    std::filesystem::rename(files.path, victim);
    std::filesystem::create_symlink(victim, files.path);
    expectStatus(cache, "app", Status::Failed, "symlink credential file accepted");
    std::filesystem::remove(files.path);
    std::filesystem::create_hard_link(victim, files.path);
    expectStatus(cache, "app", Status::Failed, "hardlinked credential file accepted");
    std::filesystem::remove(files.path);
    require(mkfifo(files.path.c_str(), 0600) == 0, "FIFO setup failed");
    expectStatus(cache, "app", Status::Failed, "FIFO credential file accepted");
    std::filesystem::remove(files.path);
    std::filesystem::create_directory(files.path);
    expectStatus(cache, "app", Status::Failed, "directory credential file accepted");
    std::filesystem::remove(files.path);
    std::filesystem::rename(victim, files.path);
    auto transaction = acquire(cache, "app");
    require(transaction->save("old") == Status::Ready, "directory replacement fixture save failed");
    const auto moved = files.root / "moved";
    std::filesystem::rename(files.path.parent_path(), moved);
    std::filesystem::create_symlink(moved, files.path.parent_path());
    require(
        transaction->save("new") == Status::Failed && transaction->credential() == "old",
        "transaction saved through replaced directory"
    );
    expectStatus(cache, "other", Status::Failed, "symlink credential directory accepted");
    std::filesystem::remove(files.path.parent_path());
    std::filesystem::rename(moved, files.path.parent_path());
    require(acquire(cache, "other")->save("other") == Status::Ready, "restored directory unusable");
    transaction.reset();
    require(acquire(cache, "app")->credential() == "old", "failed directory save changed disk credential");

    const auto lock = files.path.parent_path() / kLockName;
    require(chmod(lock.c_str(), 0644) == 0, "unsafe lock mode setup failed");
    expectStatus(cache, "app", Status::Failed, "readable lock file accepted");
    require(chmod(lock.c_str(), 0600) == 0, "lock mode restore failed");
    std::filesystem::rename(lock, victim);
    std::filesystem::create_symlink(victim, lock);
    expectStatus(cache, "app", Status::Failed, "symlink lock file accepted");
    std::filesystem::remove(lock);
    std::filesystem::create_hard_link(victim, lock);
    expectStatus(cache, "app", Status::Failed, "hardlinked lock file accepted");
    std::filesystem::remove(lock);
    std::filesystem::rename(victim, lock);
    transaction = acquire(cache, "app");
    std::filesystem::remove(lock);
    require(
        transaction->save("new") == Status::Failed && transaction->credential() == "old",
        "transaction ignored a replaced application lock"
    );
  }

  void boundsAndAtomicFailure() {
    const Temporary files;
    const Cache cache(files.path);
    auto transaction = acquire(cache, "app");
    require(transaction->save("old") == Status::Ready, "atomic fixture save failed");
    const auto before = files.read();
    for (const auto failure : {Fault::Write, Fault::FileSync, Fault::Rename}) {
      fault = failure;
      faultHit = false;
      const auto result = transaction->save("new");
      fault = Fault::None;
      require(faultHit && result == Status::Failed, "atomic failure did not fail save");
      require(transaction->credential() == "old" && files.read() == before, "failed save published replacement");
      for (const auto& entry : std::filesystem::directory_iterator(files.path.parent_path())) {
        require(
            entry.path() == files.path || entry.path().filename() == kLockName, "failed save leaked temporary file"
        );
      }
    }
    fault = Fault::Rename;
    faultHit = false;
    const auto cleared = transaction->clear();
    fault = Fault::None;
    require(
        faultHit && cleared == Status::Failed && transaction->credential() == "old" && files.read() == before,
        "failed clear lost credential"
    );
    fault = Fault::DirectorySync;
    faultHit = false;
    const auto committed = transaction->save("committed");
    fault = Fault::None;
    require(
        faultHit && committed == Status::Ready && transaction->credential() == "committed",
        "directory-sync failure reported a committed rotation as rolled back"
    );
    transaction.reset();
    transaction = acquire(cache, "app");
    require(transaction->credential() == "committed", "committed rotation was not on disk");
    require(transaction->save(std::string(4096, 'a')) == Status::Ready, "maximum credential size rejected");
    transaction.reset();
    const std::string application(256, 'a');
    require(acquire(cache, application)->save("opaque") == Status::Ready, "maximum application size rejected");

    Json entries = Json::array();
    for (size_t i = 0; i < 128; ++i) {
      entries.push_back({{"application", "app" + std::to_string(i)}, {"credential", "opaque"}});
    }
    files.write(Json{{"version", 1}, {"credentials", entries}}.dump());
    auto existing = acquire(cache, "app0");
    require(existing->save("replacement") == Status::Ready, "full cache refused existing application update");
    transaction = acquire(cache, "overflow");
    const auto full = files.read();
    require(
        transaction->save("opaque") == Status::Failed && !transaction->credential() && files.read() == full,
        "cache application limit was not enforced"
    );
    require(
        existing->clear() == Status::Ready && transaction->save("opaque") == Status::Ready,
        "clear did not make capacity available"
    );
    transaction.reset();
    existing.reset();
    entries.push_back({{"application", "extra"}, {"credential", "opaque"}});
    files.write(Json{{"version", 1}, {"credentials", entries}}.dump());
    expectStatus(cache, "app", Status::Failed, "oversized application list accepted");
  }

  void defaultPath() {
    const char* original = std::getenv("XDG_STATE_HOME");
    const std::optional<std::string> saved = original != nullptr ? std::optional(std::string(original)) : std::nullopt;
    require(setenv("XDG_STATE_HOME", "/tmp/xdpu-state-path", 1) == 0, "state path environment setup failed");
    require(
        Cache::defaultPath() == "/tmp/xdpu-state-path/xdg-desktop-portal-umbriel/global-shortcuts-credentials.json",
        "XDG state path ignored"
    );
    require(setenv("XDG_STATE_HOME", "relative", 1) == 0, "relative state path environment setup failed");
    const char* home = std::getenv("HOME");
    if (home != nullptr && std::filesystem::path(home).is_absolute()) {
      const auto fallback =
          std::filesystem::path(home) / ".local/state/xdg-desktop-portal-umbriel/global-shortcuts-credentials.json";
      require(Cache::defaultPath() == fallback, "relative state path did not use fallback");
      require(
          unsetenv("XDG_STATE_HOME") == 0 && Cache::defaultPath() == fallback, "missing state path fallback failed"
      );
      require(
          setenv("XDG_STATE_HOME", "", 1) == 0 && Cache::defaultPath() == fallback, "empty state path fallback failed"
      );
    }
    if (saved) {
      require(setenv("XDG_STATE_HOME", saved->c_str(), 1) == 0, "state path environment restore failed");
    } else {
      require(unsetenv("XDG_STATE_HOME") == 0, "state path environment restore failed");
    }
  }
} // namespace

extern "C" {
ssize_t __real_write(int fd, const void* buffer, size_t count);
ssize_t __wrap_write(int fd, const void* buffer, size_t count) {
  if (fault == Fault::Write) {
    faultHit = true;
    errno = EIO;
    return -1;
  }
  return __real_write(fd, buffer, count);
}

int __real_fsync(int fd);
int __wrap_fsync(int fd) {
  struct stat st{};
  if (fstat(fd, &st) == 0
      && ((fault == Fault::FileSync && S_ISREG(st.st_mode))
          || (fault == Fault::DirectorySync && S_ISDIR(st.st_mode)))) {
    faultHit = true;
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

int __real_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int __wrap_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath) {
  if (fault == Fault::Rename) {
    faultHit = true;
    errno = EIO;
    return -1;
  }
  return __real_renameat(olddirfd, oldpath, newdirfd, newpath);
}
}

int main() {
  try {
    defaultPath();
    basicAndConcurrentUpdates();
    fileContention();
    processLocking();
    invalidState();
    unsafeFiles();
    boundsAndAtomicFailure();
    std::println("global shortcuts credential cache passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "global shortcuts credential cache: {}", error.what());
    return 1;
  }
}
