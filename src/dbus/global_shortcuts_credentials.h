#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace xdpu {
  enum class GlobalShortcutCacheStatus : uint8_t { Ready, Busy, Failed };

  class GlobalShortcutCredentials {
  public:
    class Transaction {
    public:
      struct Impl;
      explicit Transaction(std::unique_ptr<Impl> impl);
      ~Transaction();
      Transaction(const Transaction&) = delete;
      Transaction& operator=(const Transaction&) = delete;

      const std::optional<std::string>& credential() const;
      // Keep the transaction alive through restore/bind and persistence. Only a
      // successful save publishes the replacement to this snapshot.
      [[nodiscard]] GlobalShortcutCacheStatus save(std::string_view credential);
      [[nodiscard]] GlobalShortcutCacheStatus clear();

    private:
      [[nodiscard]] GlobalShortcutCacheStatus update(std::optional<std::string_view> credential);
      std::unique_ptr<Impl> m_impl;
    };

    struct Acquisition {
      GlobalShortcutCacheStatus status = GlobalShortcutCacheStatus::Failed;
      std::unique_ptr<Transaction> transaction;
    };

    static std::filesystem::path defaultPath();
    explicit GlobalShortcutCredentials(std::filesystem::path path = defaultPath());

    // Busy never waits on another application operation or file update. A
    // transaction owns its application lock and may outlive this cache object.
    [[nodiscard]] Acquisition acquire(std::string_view application) const;

  private:
    std::filesystem::path m_path;
  };
} // namespace xdpu
