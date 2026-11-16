#include "global_shortcuts_fixture.h"
#include "loop/loop.h"
#include "wayland/global_shortcuts.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <global-shortcuts-server-protocol.h>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace {
  using Json = nlohmann::json;
  using Client = xdpu::GlobalShortcutsClient;
  using Status = xdpu::GlobalShortcutStatus;
  using Result = xdpu::GlobalShortcutResult;

  using shortcut_test::Fixture;
  using shortcut_test::require;

  void operationsAndEvents() {
    Fixture fixture;
    Client client(*fixture.loop, *fixture.wayland);
    require(client.available(), "bridge was not discovered");
    fixture.sync();
    require(fixture.send({{"op", "version"}}) == 1, "bridge version was not clamped to one");
    std::vector<Result> results;
    std::vector<std::vector<xdpu::GlobalShortcut>> changes;
    std::vector<uint64_t> times;
    auto session = client.createSession(
        "org.test.App",
        {
            .activated =
                [&](const std::string& id, uint64_t time) {
                  require(id == "talk", "activation ID changed");
                  times.push_back(time);
                },
            .deactivated = [&](const std::string&, uint64_t time) { times.push_back(time); },
            .changed = [&](auto entries) { changes.push_back(std::move(entries)); },
            .closed = {},
        }
    );
    require(session != nullptr, "session creation failed");
    auto complete = [&](Result result) { results.push_back(std::move(result)); };
    const auto restored = session->restore("old-private-credential", complete).value();
    fixture.sync();
    require(fixture.send({{"op", "seen"}})[0]["credential"] == "old-private-credential", "restore was not sent");
    fixture.send({{"op", "grant"}, {"request", restored}, {"credential", "replacement-private-credential"}});
    fixture.send({{"op", "restored"}, {"request", restored}});
    fixture.sync();
    require(
        results.size() == 1
            && results[0].status == Status::Success
            && results[0].credential == "replacement-private-credential",
        "restore result lost its replacement credential"
    );
    require(!session->restore("again", complete), "restore was allowed twice");
    const auto bound =
        session->bind(std::array{xdpu::GlobalShortcutRequest{"talk", "Talk", "CTRL+m"}}, complete).value();
    require(!session->bind({}, complete), "second bind was accepted");
    fixture.sync();
    const auto seen = fixture.send({{"op", "seen"}});
    require(seen[1]["shortcuts"][0]["trigger"] == "CTRL+m", "staged shortcut was changed");
    fixture.send({{"op", "shortcut"}, {"request", bound}});
    fixture.send({{"op", "bound"}, {"request", bound}});
    fixture.send({{"op", "activated"}, {"time", std::numeric_limits<uint32_t>::max()}});
    fixture.send({{"op", "deactivated"}, {"time", 7}});
    fixture.send({{"op", "changed"}, {"trigger", "Ctrl+N"}});
    fixture.send({{"op", "changes-done"}});
    fixture.send({{"op", "changes-done"}});
    fixture.sync();
    require(
        results.size() == 2 && results[1].shortcuts.size() == 1 && results[1].credential.empty(), "bind result was lost"
    );
    require(times == std::vector<uint64_t>{4294967295ULL, 7}, "event timestamps were truncated or reordered");
    require(
        changes.size() == 2
            && changes[0].size() == 1
            && changes[0][0].triggerDescription == "Ctrl+N"
            && changes[1].empty(),
        "changed batches were not complete replacements"
    );
    results.clear();
    const auto first = session->list(complete).value();
    const auto second = session->list(complete).value();
    fixture.sync();
    fixture.send({{"op", "shortcut"}, {"request", first}, {"id", "first"}});
    fixture.send({{"op", "shortcut"}, {"request", second}, {"id", "second"}});
    fixture.send({{"op", "listed"}, {"request", second}});
    fixture.send({{"op", "listed"}, {"request", first}});
    fixture.send({{"op", "listed"}, {"request", first}});
    fixture.sync();
    require(
        results.size() == 2
            && results[0].shortcuts.size() == 1
            && results[1].shortcuts.size() == 1
            && results[0].shortcuts[0].id == "second"
            && results[1].shortcuts[0].id == "first",
        "operation IDs did not isolate list replies"
    );
    const auto empty = session->list(complete).value();
    fixture.sync();
    fixture.send({{"op", "listed"}, {"request", empty}});
    fixture.sync();
    require(results.size() == 3 && results.back().shortcuts.empty(), "empty list failed");
  }

  void cancellationAndClosure() {
    Fixture fixture;
    Client client(*fixture.loop, *fixture.wayland);
    std::vector<Result> results;
    int closed = 0;
    auto session = client.createSession(
        "org.test.App", {.activated = {}, .deactivated = {}, .changed = {}, .closed = [&] { ++closed; }}
    );
    const auto request = session->bind({}, [&](Result result) { results.push_back(std::move(result)); }).value();
    session->cancel(request);
    session->cancel(request);
    fixture.sync();
    require(fixture.send({{"op", "seen"}}).size() == 2, "bind cancellation was not sent once");
    fixture.send({{"op", "grant"}, {"request", request}, {"credential", "late-private-credential"}});
    fixture.send({{"op", "shortcut"}, {"request", request}});
    fixture.send({{"op", "bound"}, {"request", request}});
    fixture.sync();
    require(
        results.size() == 1 && results[0].status == Status::Cancelled && results[0].credential.empty(),
        "cancelled bind completed again"
    );
    const auto list = session->list([&](Result result) { results.push_back(std::move(result)); }).value();
    session->cancel(list);
    fixture.sync();
    fixture.send({{"op", "listed"}, {"request", list}});
    fixture.sync();
    require(results.size() == 2 && !session->closed(), "list cancellation closed the session");
    fixture.send({{"op", "closed"}});
    fixture.sync();
    require(session->closed() && closed == 1 && !session->list([](const auto&) {}), "closed session accepted requests");
    session->close();
    require(closed == 1, "closure callback repeated");
    auto restoring = client.createSession("org.test.Other");
    const auto restore =
        restoring->restore("private", [&](Result result) { results.push_back(std::move(result)); }).value();
    restoring->cancel(restore);
    require(restoring->closed() && results.back().status == Status::Cancelled, "abandoned restore stayed live");
    auto invalid = client.createSession("org.test.InvalidCredential");
    const auto rejected =
        invalid->restore("invalid-private", [&](Result result) { results.push_back(std::move(result)); }).value();
    fixture.sync();
    fixture.send({{"op", "restored"}, {"session", 2}, {"request", rejected}, {"status", 2}});
    fixture.sync();
    require(
        !invalid->closed() && results.back().status == Status::Failed, "invalid credential prevented fresh approval"
    );
    const auto fresh = invalid->bind({}, [&](Result result) { results.push_back(std::move(result)); }).value();
    fixture.sync();
    fixture.send({{"op", "bound"}, {"session", 2}, {"request", fresh}});
    fixture.sync();
    require(
        results.back().status == Status::Success && results.back().credential.empty(),
        "empty fresh bind failed after invalid restore"
    );
  }

  void managerAndOwnerLifetime() {
    Fixture fixture(false);
    auto client = std::make_unique<Client>(*fixture.loop, *fixture.wayland);
    require(!client->available() && !client->createSession("org.test.App"), "missing global was treated as available");
    fixture.send({{"op", "announce"}});
    fixture.sync();
    require(client->available(), "late bridge global was not bound");
    int replies = 0;
    int closed = 0;
    auto session = client->createSession(
        "org.test.App", {.activated = {}, .deactivated = {}, .changed = {}, .closed = [&] { ++closed; }}
    );
    require(
        session
            ->list([&](const Result& result) {
              require(result.status == Status::Failed, "manager loss did not fail request");
              ++replies;
            })
            .has_value(),
        "list was rejected"
    );
    fixture.sync();
    fixture.send({{"op", "remove"}});
    fixture.sync();
    require(
        !client->available() && session->closed() && replies == 1 && closed == 1,
        "manager loss left pending state alive"
    );
    fixture.send({{"op", "announce"}});
    fixture.sync();
    auto replacement = client->createSession(
        "org.test.Replacement", {.activated = {}, .deactivated = {}, .changed = {}, .closed = [&] { ++closed; }}
    );
    require(
        replacement && replacement->list([&](const Result&) { ++replies; }).has_value(),
        "replacement manager could not create a session"
    );
    client.reset();
    require(replacement->closed() && replies == 2 && closed == 2, "session outlived its owning client");
  }

  void malformedRepliesAndTimeouts() {
    for (const std::string kind :
         {"missing-grant", "wrong-kind", "duplicate-entry", "oversized-description", "invalid-status",
          "duplicate-grant", "duplicate-change", "overflow"}) {
      Fixture fixture;
      Client client(*fixture.loop, *fixture.wayland);
      int replies = 0;
      auto session = client.createSession("org.test.App");
      auto failed = [&](const Result& result) {
        require(
            result.status == Status::Failed && result.credential.empty(),
            "malformed reply exposed success or credentials"
        );
        ++replies;
      };
      const auto request = kind == "missing-grant" || kind == "duplicate-grant"
          ? session->restore("private", failed).value()
          : session->list(failed).value();
      fixture.sync();
      if (kind == "missing-grant") {
        fixture.send({{"op", "restored"}, {"request", request}});
      } else if (kind == "wrong-kind") {
        fixture.send({{"op", "bound"}, {"request", request}});
      } else if (kind == "invalid-status") {
        fixture.send({{"op", "listed"}, {"request", request}, {"status", 9}});
      } else if (kind == "duplicate-grant") {
        fixture.send({{"op", "grant"}, {"request", request}, {"credential", "first-private"}});
        fixture.send({{"op", "grant"}, {"request", request}, {"credential", "second-private"}});
      } else if (kind == "duplicate-change") {
        fixture.send({{"op", "changed"}});
        fixture.send({{"op", "changed"}});
      } else if (kind == "overflow") {
        for (size_t i = 0; i < 129; ++i) {
          fixture.send({{"op", "shortcut"}, {"request", request}, {"id", "id-" + std::to_string(i)}});
        }
      } else {
        fixture.send(
            {{"op", "shortcut"},
             {"request", request},
             {"description", kind == "oversized-description" ? std::string(1025, 'x') : "Action"}}
        );
        if (kind == "duplicate-entry") {
          fixture.send({{"op", "shortcut"}, {"request", request}});
        }
      }
      fixture.sync();
      require(session->closed() && replies == 1, "malformed reply did not close the session exactly once");
    }
    for (const bool bind : {false, true}) {
      Fixture fixture;
      Client client(*fixture.loop, *fixture.wayland, {5, 5});
      auto session = client.createSession("org.test.Timeout");
      bool expired = false;
      bool watchdog = false;
      auto failed = [&](const Result& result) {
        expired = result.status == Status::Failed;
        fixture.loop->quit();
      };
      require(
          (bind ? session->bind({}, failed) : session->list(failed)).has_value(), "deadline test request was rejected"
      );
      const int timer = fixture.loop->addTimer(2000, [&] {
        watchdog = true;
        fixture.loop->quit();
      });
      fixture.loop->run();
      fixture.loop->removeTimer(timer);
      require(expired && !watchdog && session->closed(), "operation deadline did not retire the session");
    }
  }

  void validationLimitsAndReentrancy() {
    Fixture fixture;
    auto client = std::make_unique<Client>(*fixture.loop, *fixture.wayland);
    require(
        !client->createSession("")
            && !client->createSession(std::string(257, 'a'))
            && !client->createSession(std::string(1, '\xFF')),
        "invalid application label was accepted"
    );
    std::vector<std::shared_ptr<Client::Session>> sessions;
    for (size_t i = 0; i < 16; ++i) {
      sessions.push_back(client->createSession("org.test.App"));
      require(sessions.back() != nullptr, "session quota was too small");
    }
    require(!client->createSession("org.test.Extra"), "session quota was not enforced");
    auto complete = [](const Result&) {};
    require(
        !sessions[0]->bind(
            std::array{xdpu::GlobalShortcutRequest{"id", "", ""}, xdpu::GlobalShortcutRequest{"id", "", ""}}, complete
        ),
        "duplicate shortcut IDs were staged"
    );
    require(
        !sessions[0]->bind(std::vector<xdpu::GlobalShortcutRequest>(129, {"id", "", ""}), complete),
        "oversized batch was staged"
    );
    require(
        !sessions[0]->bind(std::array{xdpu::GlobalShortcutRequest{"id", std::string(1025, 'x'), ""}}, complete),
        "oversized description was staged"
    );
    for (size_t i = 0; i < 64; ++i) {
      require(sessions[i % 16]->list(complete).has_value(), "operation quota was too small");
    }
    require(!sessions[0]->list(complete), "operation quota was not enforced");
    sessions[0]->close();
    auto recovered = client->createSession("org.test.Recovered");
    require(recovered && recovered->list(complete).has_value(), "closing a session did not recover quota");
    sessions.clear();
    recovered.reset();
    fixture.sync();
    auto session = client->createSession("org.test.Reentrant");
    int replies = 0;
    const auto request = session
                             ->list([&](const Result& result) {
                               require(result.status == Status::Success, "reentrant result failed");
                               ++replies;
                               session.reset();
                               client.reset();
                             })
                             .value();
    fixture.sync();
    fixture.send({{"op", "listed"}, {"session", 17}, {"request", request}});
    fixture.sync();
    require(replies == 1 && !session && !client, "callback could not release its session and client");
  }
} // namespace

int main() {
  try {
    operationsAndEvents();
    cancellationAndClosure();
    managerAndOwnerLifetime();
    malformedRepliesAndTimeouts();
    validationLimitsAndReentrancy();
  } catch (const std::exception& error) {
    std::println(stderr, "global shortcuts client: {}", error.what());
    return EXIT_FAILURE;
  }
  std::println(
      "global shortcut transport, lifecycle, bounds, deadlines, and "
      "reentrancy passed"
  );
}
