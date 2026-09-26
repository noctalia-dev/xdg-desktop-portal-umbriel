#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace xdpu {

  class Loop;

  struct ScreenCastCommand {
    enum class Kind {
      Clear,
      Output,
      Window,
      FollowWindow,
      FollowOutput,
      FollowStop,
    };

    Kind kind = Kind::Clear;
    std::string value;
    uint64_t serial = 0;
  };

  class UmbrielIpc {
  public:
    using ScreenCastHandler = std::function<void(const ScreenCastCommand&)>;
    using FocusHandler = std::function<void(const std::optional<std::string>&)>;

    UmbrielIpc(
        Loop& loop, ScreenCastHandler screenCastHandler, FocusHandler focusedWindowHandler,
        FocusHandler focusedOutputHandler
    );
    ~UmbrielIpc();

    UmbrielIpc(const UmbrielIpc&) = delete;
    UmbrielIpc& operator=(const UmbrielIpc&) = delete;

    [[nodiscard]] bool connected() const;
    void setScreenCastActive(bool active);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
