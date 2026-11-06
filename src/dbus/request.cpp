#include "dbus/request.h"

#include "loop/loop.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace xdpu {

  namespace {

    constexpr char kRequestInterface[] = "org.freedesktop.impl.portal.Request";

    void closeFd(int& fd) {
      if (fd < 0) {
        return;
      }
      while (::close(fd) < 0 && errno == EINTR) {
      }
      fd = -1;
    }

    bool setNonBlocking(int fd) {
      const int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags < 0) {
        return false;
      }
      return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

  } // namespace

  struct Request::Impl {
    std::unique_ptr<sdbus::IObject> object;
    std::string path;
    CloseHandler closeHandler;
    bool isClosed = false;

    void close() {
      if (isClosed) {
        return;
      }
      isClosed = true;
      auto handler = std::move(closeHandler);
      if (handler) {
        handler();
      }
    }
  };

  Request::Request(sdbus::IConnection& connection, std::string path, CloseHandler closeHandler)
      : m_impl(std::make_unique<Impl>()) {
    m_impl->path = std::move(path);
    m_impl->closeHandler = std::move(closeHandler);
    m_impl->object = sdbus::createObject(connection, sdbus::ObjectPath{m_impl->path});
    m_impl->object->addVTable(
                      sdbus::registerMethod("Close").implementedAs([this] { m_impl->close(); })
    ).forInterface(kRequestInterface);
  }

  Request::~Request() = default;

  const std::string& Request::path() const { return m_impl->path; }

  bool Request::closed() const { return m_impl->isClosed; }

  void Request::setCloseHandler(CloseHandler closeHandler) { m_impl->closeHandler = std::move(closeHandler); }

  void Request::close() { m_impl->close(); }

  bool ProcessResult::success() const { return exited && exitStatus == 0; }

  bool ProcessResult::commandNotFound() const { return exited && exitStatus == 127; }

  struct AsyncProcess::Impl {
    Loop& loop;
    std::string command;
    std::string input;
    Callback callback;
    std::string output;
    pid_t pid = -1;
    int stdinFd = -1;
    int stdoutFd = -1;
    int stdinWatch = 0;
    int stdoutWatch = 0;
    int waitTimer = 0;
    size_t inputOffset = 0;
    bool finished = false;

    Impl(Loop& loop, std::string command, std::string input, Callback callback)
        : loop(loop), command(std::move(command)), input(std::move(input)), callback(std::move(callback)) {}

    bool spawn() {
      int stdinPipe[2] = {-1, -1};
      int stdoutPipe[2] = {-1, -1};
      if (::pipe2(stdinPipe, O_CLOEXEC) < 0) {
        std::println(stderr, "dbus: pipe2(stdin) failed: {}", std::strerror(errno));
        return false;
      }
      if (::pipe2(stdoutPipe, O_CLOEXEC) < 0) {
        std::println(stderr, "dbus: pipe2(stdout) failed: {}", std::strerror(errno));
        closeFd(stdinPipe[0]);
        closeFd(stdinPipe[1]);
        return false;
      }

      pid = ::fork();
      if (pid < 0) {
        std::println(stderr, "dbus: fork failed for '{}': {}", command, std::strerror(errno));
        closeFd(stdinPipe[0]);
        closeFd(stdinPipe[1]);
        closeFd(stdoutPipe[0]);
        closeFd(stdoutPipe[1]);
        return false;
      }

      if (pid == 0) {
        ::dup2(stdinPipe[0], STDIN_FILENO);
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        closeFd(stdinPipe[0]);
        closeFd(stdinPipe[1]);
        closeFd(stdoutPipe[0]);
        closeFd(stdoutPipe[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
      }

      closeFd(stdinPipe[0]);
      closeFd(stdoutPipe[1]);
      stdinFd = stdinPipe[1];
      stdoutFd = stdoutPipe[0];
      if (!setNonBlocking(stdinFd) || !setNonBlocking(stdoutFd)) {
        std::println(stderr, "dbus: fcntl(O_NONBLOCK) failed for '{}': {}", command, std::strerror(errno));
      }
      return true;
    }

    void install(const std::shared_ptr<AsyncProcess>& self) {
      std::weak_ptr<AsyncProcess> weak = self;
      stdoutWatch = loop.addFd(stdoutFd, EPOLLIN | EPOLLHUP | EPOLLERR, [weak](uint32_t events) {
        if (auto process = weak.lock()) {
          process->m_impl->readReady(process, events);
        }
      });
      if (stdoutWatch == 0) {
        std::println(stderr, "dbus: unable to watch child stdout for '{}'", command);
        if (pid > 0) {
          ::kill(pid, SIGTERM);
        }
        ProcessResult result;
        finish(std::move(result));
        return;
      }

      if (input.empty()) {
        closeFd(stdinFd);
        return;
      }

      stdinWatch = loop.addFd(stdinFd, EPOLLOUT | EPOLLHUP | EPOLLERR, [weak](uint32_t events) {
        if (auto process = weak.lock()) {
          process->m_impl->writeReady(process, events);
        }
      });
      if (stdinWatch == 0) {
        std::println(stderr, "dbus: unable to watch child stdin for '{}'", command);
        closeFd(stdinFd);
      }
    }

    void removeStdinWatch() {
      if (stdinWatch != 0) {
        loop.removeFd(stdinWatch);
        stdinWatch = 0;
      }
    }

    void removeStdoutWatch() {
      if (stdoutWatch != 0) {
        loop.removeFd(stdoutWatch);
        stdoutWatch = 0;
      }
    }

    void closeStdin() {
      removeStdinWatch();
      closeFd(stdinFd);
    }

    void closeStdout() {
      removeStdoutWatch();
      closeFd(stdoutFd);
    }

    void writeReady(const std::shared_ptr<AsyncProcess>&, uint32_t events) {
      if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
        closeStdin();
        return;
      }

      while (inputOffset < input.size()) {
        const char* data = input.data() + inputOffset;
        const size_t remaining = input.size() - inputOffset;
        const ssize_t written = ::write(stdinFd, data, remaining);
        if (written > 0) {
          inputOffset += static_cast<size_t>(written);
          continue;
        }
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          return;
        }
        closeStdin();
        return;
      }

      closeStdin();
    }

    void readReady(const std::shared_ptr<AsyncProcess>& self, uint32_t events) {
      char buffer[4096];
      while (true) {
        const ssize_t size = ::read(stdoutFd, buffer, sizeof(buffer));
        if (size > 0) {
          output.append(buffer, buffer + size);
          continue;
        }
        if (size == 0) {
          closeStdout();
          scheduleWait(self, 0);
          return;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        closeStdout();
        scheduleWait(self, 0);
        return;
      }

      if ((events & (EPOLLHUP | EPOLLERR)) != 0) {
        closeStdout();
        scheduleWait(self, 0);
      }
    }

    void scheduleWait(const std::shared_ptr<AsyncProcess>& self, int timeoutMs) {
      if (finished || pid <= 0) {
        return;
      }
      if (waitTimer != 0) {
        loop.removeTimer(waitTimer);
        waitTimer = 0;
      }
      waitTimer = loop.addTimer(timeoutMs, [self] { self->m_impl->pollExit(self); });
    }

    void pollExit(const std::shared_ptr<AsyncProcess>& self) {
      waitTimer = 0;
      if (finished || pid <= 0) {
        return;
      }

      int status = 0;
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == 0) {
        scheduleWait(self, 25);
        return;
      }
      if (waited < 0 && errno == EINTR) {
        scheduleWait(self, 0);
        return;
      }

      ProcessResult result;
      result.output = std::move(output);
      if (waited == pid) {
        if (WIFEXITED(status)) {
          result.exited = true;
          result.exitStatus = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          result.signaled = true;
          result.termSignal = WTERMSIG(status);
        }
      }
      pid = -1;
      finish(std::move(result));
    }

    void terminate(const std::shared_ptr<AsyncProcess>& self) {
      callback = nullptr;
      closeStdin();
      closeStdout();
      if (pid > 0) {
        ::kill(pid, SIGTERM);
        scheduleWait(self, 25);
      }
    }

    void finish(ProcessResult result) {
      if (finished) {
        return;
      }
      finished = true;
      closeStdin();
      closeStdout();
      if (waitTimer != 0) {
        const int timer = waitTimer;
        waitTimer = 0;
        loop.removeTimer(timer);
      }

      auto cb = std::move(callback);
      if (cb) {
        cb(std::move(result));
      }
    }

    void destroy() {
      callback = nullptr;
      if (waitTimer != 0) {
        const int timer = waitTimer;
        waitTimer = 0;
        loop.removeTimer(timer);
      }
      closeStdin();
      closeStdout();
      if (pid > 0) {
        ::kill(pid, SIGTERM);
        int status = 0;
        (void)::waitpid(pid, &status, WNOHANG);
        pid = -1;
      }
    }
  };

  std::shared_ptr<AsyncProcess>
  AsyncProcess::start(Loop& loop, std::string command, std::string input, Callback callback) {
    std::signal(SIGPIPE, SIG_IGN);
    auto process = std::shared_ptr<AsyncProcess>(
        new AsyncProcess(loop, std::move(command), std::move(input), std::move(callback))
    );
    if (!process->m_impl->spawn()) {
      ProcessResult result;
      result.output = {};
      process->m_impl->finish(std::move(result));
      return process;
    }
    process->m_impl->install(process);
    return process;
  }

  AsyncProcess::AsyncProcess(Loop& loop, std::string command, std::string input, Callback callback)
      : m_impl(std::make_unique<Impl>(loop, std::move(command), std::move(input), std::move(callback))) {}

  AsyncProcess::~AsyncProcess() {
    if (m_impl) {
      m_impl->destroy();
    }
  }

  void AsyncProcess::terminate() {
    if (!m_impl || m_impl->finished) {
      return;
    }
    m_impl->terminate(shared_from_this());
  }

  pid_t AsyncProcess::pid() const { return m_impl ? m_impl->pid : -1; }

} // namespace xdpu
