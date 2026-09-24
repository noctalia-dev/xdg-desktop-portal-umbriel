#include "dbus/screencast.h"

#include "config/config.h"
#include "dbus/request.h"
#include "dbus/session.h"
#include "loop/loop.h"
#include "pipewire/pipewire.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <sdbus-c++/sdbus-c++.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
#include <utility>

namespace xdpu {

  namespace {

    constexpr char kInterface[] = "org.freedesktop.impl.portal.ScreenCast";
    constexpr uint32_t kSourceMonitor = 1;
    constexpr uint32_t kSourceWindow = 2;
    constexpr uint32_t kAvailableSourceTypes = kSourceMonitor | kSourceWindow;
    constexpr uint32_t kCursorHidden = 1;
    constexpr uint32_t kCursorEmbedded = 2;
    constexpr uint32_t kCursorMetadata = 4;
    constexpr uint32_t kAvailableCursorModes = kCursorHidden | kCursorEmbedded | kCursorMetadata;

    using StreamTuple = sdbus::Struct<uint32_t, PortalResults>;
    using RestoreTuple = sdbus::Struct<std::string, uint32_t, sdbus::Variant>;

    std::string firstLine(std::string_view text) {
      const size_t begin = text.find_first_not_of(" \t\r\n");
      if (begin == std::string_view::npos) {
        return {};
      }
      const size_t end = text.find_first_of("\r\n", begin);
      const std::string_view line =
          text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
      const size_t trimmedEnd = line.find_last_not_of(" \t");
      if (trimmedEnd == std::string_view::npos) {
        return {};
      }
      return std::string(line.substr(0, trimmedEnd + 1));
    }

    template <typename T> std::optional<T> optionValue(const PortalResults& options, const std::string& key) {
      const auto it = options.find(key);
      if (it == options.end() || !it->second.containsValueOfType<T>()) {
        return std::nullopt;
      }
      try {
        return it->second.get<T>();
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

    std::optional<std::string> dictString(const PortalResults& dict, const std::string& key) {
      const auto it = dict.find(key);
      if (it == dict.end() || !it->second.containsValueOfType<std::string>()) {
        return std::nullopt;
      }
      try {
        return it->second.get<std::string>();
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

    const OutputInfo* findOutput(const WaylandContext& wayland, const std::string& name) {
      const auto& outputs = wayland.outputs();
      const auto it = std::ranges::find_if(outputs, [&](const OutputInfo& output) { return output.name == name; });
      return it == outputs.end() ? nullptr : &*it;
    }

    const ToplevelInfo* findToplevelByIdentifier(const WaylandContext& wayland, const std::string& identifier) {
      const auto& toplevels = wayland.toplevels();
      const auto it = std::ranges::find_if(toplevels, [&](const ToplevelInfo& toplevel) {
        return toplevel.identifier == identifier;
      });
      return it == toplevels.end() ? nullptr : &*it;
    }

    const ToplevelInfo* findToplevelByAppId(const WaylandContext& wayland, const std::string& appId) {
      const auto& toplevels = wayland.toplevels();
      const auto it =
          std::ranges::find_if(toplevels, [&](const ToplevelInfo& toplevel) { return toplevel.appId == appId; });
      return it == toplevels.end() ? nullptr : &*it;
    }

    Session::Selection selectionForOutput(const OutputInfo& output) {
      Session::Selection selection;
      selection.kind = Session::SourceKind::Monitor;
      selection.output = output.name;
      selection.title = output.description;
      selection.x = output.x;
      selection.y = output.y;
      selection.width = output.width;
      selection.height = output.height;
      return selection;
    }

    Session::Selection selectionForToplevel(const ToplevelInfo& toplevel) {
      Session::Selection selection;
      selection.kind = Session::SourceKind::Window;
      selection.identifier = toplevel.identifier;
      selection.appId = toplevel.appId;
      selection.title = toplevel.title;
      return selection;
    }

    std::vector<Session::Selection> parseRestoreData(const PortalResults& options) {
      const auto it = options.find("restore_data");
      if (it == options.end() || !it->second.containsValueOfType<RestoreTuple>()) {
        return {};
      }

      RestoreTuple restore;
      try {
        restore = it->second.get<RestoreTuple>();
      } catch (const std::exception&) {
        return {};
      }

      if (std::get<0>(restore) != "umbriel" || std::get<1>(restore) != 1) {
        return {};
      }

      std::vector<PortalResults> entries;
      try {
        if (!std::get<2>(restore).containsValueOfType<std::vector<PortalResults>>()) {
          return {};
        }
        entries = std::get<2>(restore).get<std::vector<PortalResults>>();
      } catch (const std::exception&) {
        return {};
      }

      std::vector<Session::Selection> selections;
      for (const PortalResults& entry : entries) {
        const auto kind = dictString(entry, "kind");
        if (!kind) {
          continue;
        }

        Session::Selection selection;
        selection.title = dictString(entry, "title").value_or(std::string{});
        if (*kind == "monitor") {
          const auto output = dictString(entry, "output");
          if (!output || output->empty()) {
            continue;
          }
          selection.kind = Session::SourceKind::Monitor;
          selection.output = *output;
        } else if (*kind == "window") {
          const auto appId = dictString(entry, "app_id");
          if (!appId || appId->empty()) {
            continue;
          }
          selection.kind = Session::SourceKind::Window;
          selection.appId = *appId;
        } else {
          continue;
        }
        selections.push_back(std::move(selection));
      }
      return selections;
    }

    std::vector<Session::Selection> matchRestoreSelections(
        const WaylandContext& wayland, const std::vector<Session::Selection>& restore, bool multiple
    ) {
      std::vector<Session::Selection> matches;
      for (const Session::Selection& stored : restore) {
        if (stored.kind == Session::SourceKind::Monitor) {
          if (const OutputInfo* output = findOutput(wayland, stored.output)) {
            matches.push_back(selectionForOutput(*output));
          }
        } else if (const ToplevelInfo* toplevel = findToplevelByAppId(wayland, stored.appId)) {
          matches.push_back(selectionForToplevel(*toplevel));
        }

        if (!multiple && !matches.empty()) {
          break;
        }
      }
      return matches;
    }

    std::vector<Session::Selection>
    parseChooserSelections(const WaylandContext& wayland, std::string_view output, bool multiple) {
      const std::string line = firstLine(output);
      if (line.empty()) {
        return {};
      }

      std::vector<Session::Selection> selections;
      try {
        const auto json = nlohmann::json::parse(line);
        const auto values = json.value("selections", nlohmann::json::array());
        if (!values.is_array()) {
          return {};
        }

        for (const auto& value : values) {
          if (!value.is_object()) {
            continue;
          }
          const std::string kind = value.value("kind", "");
          if (kind == "monitor") {
            if (const OutputInfo* outputInfo = findOutput(wayland, value.value("output", ""))) {
              selections.push_back(selectionForOutput(*outputInfo));
            }
          } else if (kind == "window") {
            if (const ToplevelInfo* toplevel = findToplevelByIdentifier(wayland, value.value("identifier", ""))) {
              selections.push_back(selectionForToplevel(*toplevel));
            }
          }

          if (!multiple && !selections.empty()) {
            break;
          }
        }
      } catch (const std::exception& error) {
        std::fprintf(stderr, "screencast: chooser returned invalid JSON: %s\n", error.what());
      }

      return selections;
    }

    std::string makeUuid() {
      std::random_device random;
      std::uniform_int_distribution<uint32_t> dist(0, 255);
      std::array<uint8_t, 16> bytes{};
      for (uint8_t& byte : bytes) {
        byte = static_cast<uint8_t>(dist(random));
      }
      bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
      bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);

      std::ostringstream out;
      out << std::hex << std::setfill('0');
      for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
          out << '-';
        }
        out << std::setw(2) << static_cast<int>(bytes[i]);
      }
      return out.str();
    }

    std::filesystem::path stateFilePath() {
      if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0') {
        return std::filesystem::path{state} / "xdg-desktop-portal-umbriel" / "restore.json";
      }
      if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".local" / "state" / "xdg-desktop-portal-umbriel" / "restore.json";
      }
      return std::filesystem::path{"/tmp"} / "xdg-desktop-portal-umbriel-restore.json";
    }

    void savePersistentRestore(const std::string& token, const std::vector<Session::Selection>& selections) {
      const std::filesystem::path path = stateFilePath();
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error) {
        std::fprintf(
            stderr, "screencast: unable to create state directory %s: %s\n", path.parent_path().c_str(),
            error.message().c_str()
        );
        return;
      }
      (void)::chmod(path.parent_path().c_str(), 0700);

      nlohmann::json root = nlohmann::json::object();
      {
        std::ifstream in(path);
        if (in.good()) {
          try {
            in >> root;
          } catch (const std::exception&) {
            root = nlohmann::json::object();
          }
        }
      }

      nlohmann::json values = nlohmann::json::array();
      for (const Session::Selection& selection : selections) {
        nlohmann::json entry;
        entry["kind"] = selection.kind == Session::SourceKind::Window ? "window" : "monitor";
        entry["title"] = selection.title;
        if (selection.kind == Session::SourceKind::Window) {
          entry["app_id"] = selection.appId;
        } else {
          entry["output"] = selection.output;
        }
        values.push_back(std::move(entry));
      }
      root[token] = std::move(values);

      std::ofstream out(path, std::ios::trunc);
      if (!out.good()) {
        std::fprintf(stderr, "screencast: unable to write %s\n", path.c_str());
        return;
      }
      out << root.dump(2) << '\n';
      (void)::chmod(path.c_str(), 0600);
    }

  } // namespace

  struct ScreenCastPortal::Impl {
    struct PendingCapture {
      Session::Selection selection;
      std::unique_ptr<WaylandContext::CaptureSession> capture;
      CaptureConstraints constraints;
      bool hasConstraints = false;
    };

    Loop& loop;
    sdbus::IConnection& connection;
    sdbus::IObject& object;
    Config config;
    WaylandContext& wayland;
    PipeWireContext& pipewire;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::map<std::string, std::vector<Session::Selection>> memoryRestores;

    Impl(
        Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, const Config& config,
        WaylandContext& wayland, PipeWireContext& pipewire
    )
        : loop(loop), connection(connection), object(object), config(config), wayland(wayland), pipewire(pipewire) {
      object
          .addVTable(
              sdbus::registerMethod("CreateSession")
                  .implementedAs([this](
                                     PortalResponse&& result, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& sessionHandle, const std::string& appId,
                                     const PortalResults& options
                                 ) {
                    (void)appId;
                    (void)options;
                    createSession(std::move(result), std::string(handle), std::string(sessionHandle));
                  })
                  .withInputParamNames("handle", "session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("SelectSources")
                  .implementedAs([this](
                                     PortalResponse&& result, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& sessionHandle, const std::string& appId,
                                     const PortalResults& options
                                 ) {
                    (void)appId;
                    selectSources(std::move(result), std::string(handle), std::string(sessionHandle), options);
                  })
                  .withInputParamNames("handle", "session_handle", "app_id", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerMethod("Start")
                  .implementedAs([this](
                                     PortalResponse&& result, const sdbus::ObjectPath& handle,
                                     const sdbus::ObjectPath& sessionHandle, const std::string& appId,
                                     const std::string& parentWindow, const PortalResults& options
                                 ) {
                    (void)appId;
                    (void)parentWindow;
                    (void)options;
                    start(std::move(result), std::string(handle), std::string(sessionHandle));
                  })
                  .withInputParamNames("handle", "session_handle", "app_id", "parent_window", "options")
                  .withOutputParamNames("response", "results"),
              sdbus::registerProperty("AvailableSourceTypes").withGetter([]() { return kAvailableSourceTypes; }),
              sdbus::registerProperty("AvailableCursorModes").withGetter([]() { return kAvailableCursorModes; }),
              sdbus::registerProperty("version").withGetter([]() { return uint32_t{4}; })
          )
          .forInterface(kInterface);
    }

    void deferEraseSession(const std::string& path) {
      loop.addTimer(0, [this, path]() { sessions.erase(path); });
    }

    void createSession(PortalResponse&& result, const std::string& handle, const std::string& sessionPath) {
      auto request = std::make_shared<Request>(connection, handle, []() {});
      if (auto existing = sessions.find(sessionPath); existing != sessions.end()) {
        existing->second->close();
        sessions.erase(existing);
      }

      sessions.emplace(sessionPath, std::make_shared<Session>(connection, sessionPath, [this, sessionPath]() {
                         deferEraseSession(sessionPath);
                       }));
      result.returnResults(uint32_t{0}, PortalResults{});
      loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
    }

    void selectSources(
        PortalResponse&& result, const std::string& handle, const std::string& sessionPath, const PortalResults& options
    ) {
      auto request = std::make_shared<Request>(connection, handle, []() {});
      const auto sessionIt = sessions.find(sessionPath);
      if (sessionIt == sessions.end() || sessionIt->second->closed()) {
        result.returnResults(uint32_t{2}, PortalResults{});
        loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
        return;
      }

      const uint32_t types = optionValue<uint32_t>(options, "types").value_or(kSourceMonitor);
      const bool multiple = optionValue<bool>(options, "multiple").value_or(false);
      const uint32_t cursorMode = optionValue<uint32_t>(options, "cursor_mode").value_or(kCursorHidden);
      if (cursorMode != kCursorHidden && cursorMode != kCursorEmbedded && cursorMode != kCursorMetadata) {
        std::fprintf(stderr, "screencast: unsupported cursor mode %u\n", cursorMode);
        sessionIt->second->closeByBackend();
        result.returnResults(uint32_t{2}, PortalResults{});
        loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
        return;
      }
      const uint32_t persistMode = optionValue<uint32_t>(options, "persist_mode").value_or(0);
      sessionIt->second->setSelectionOptions(types, multiple, cursorMode, persistMode, parseRestoreData(options));

      result.returnResults(uint32_t{0}, PortalResults{});
      loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
    }

    struct StartOperation : public std::enable_shared_from_this<StartOperation> {
      Impl& portal;
      PortalResponse result;
      std::shared_ptr<Request> request;
      std::shared_ptr<AsyncProcess> process;
      std::shared_ptr<Session> session;
      std::vector<PendingCapture> pending;
      int timeoutTimer = 0;
      bool done = false;

      StartOperation(Impl& portal, PortalResponse&& result, std::shared_ptr<Session> session)
          : portal(portal), result(std::move(result)), session(std::move(session)) {}

      void start(const std::string& handle) {
        attachRequest(handle);
        const auto restored = matchRestoreSelections(portal.wayland, session->restoreSelections(), session->multiple());
        if (!restored.empty()) {
          startCaptures(restored);
          return;
        }
        runChooser();
      }

      void attachRequest(const std::string& handle) {
        std::weak_ptr<StartOperation> weak = shared_from_this();
        request = std::make_shared<Request>(portal.connection, handle, [weak]() {
          if (auto op = weak.lock()) {
            op->cancel();
          }
        });
      }

      void deferRequestDestroy() {
        if (!request) {
          return;
        }
        auto doomed = std::move(request);
        portal.loop.addTimer(0, [doomed = std::move(doomed)]() mutable { doomed.reset(); });
      }

      void finish(uint32_t response, PortalResults results = {}) {
        if (done) {
          return;
        }
        done = true;
        if (timeoutTimer != 0) {
          const int timer = timeoutTimer;
          timeoutTimer = 0;
          portal.loop.removeTimer(timer);
        }
        if (response != 0 && session) {
          session->clearStreams();
        }
        result.returnResults(response, results);
        deferRequestDestroy();
        process.reset();
        pending.clear();
        // Prevent erase from destroying `this` while we're in finish().
        auto prevent = shared_from_this();
        portal.inflight.erase(prevent);
      }

      void cancel() {
        if (done) {
          return;
        }
        if (process) {
          process->terminate();
        }
        if (session) {
          session->clearStreams();
        }
        finish(1, {});
      }

      void runChooser() {
        if (portal.config.screencast.chooserCmd.empty()) {
          std::fprintf(stderr, "screencast: no chooser command configured; cancelling\n");
          finish(1, {});
          return;
        }

        auto self = shared_from_this();
        std::string input = portal.wayland.buildChooserJson(session->sourceTypes(), session->multiple());
        input.push_back('\n');
        process = AsyncProcess::start(
            portal.loop, portal.config.screencast.chooserCmd, std::move(input), [self](ProcessResult child) {
              if (self->done) {
                return;
              }
              if (child.commandNotFound()) {
                std::fprintf(
                    stderr, "screencast: chooser command not found (%s); falling back to first output\n",
                    self->portal.config.screencast.chooserCmd.c_str()
                );
                self->fallbackFirstOutput();
                return;
              }
              if (!child.success()) {
                std::fprintf(
                    stderr, "screencast: chooser exited unsuccessfully (status=%d, signal=%d)\n", child.exitStatus,
                    child.termSignal
                );
                self->finish(1, {});
                return;
              }

              auto selections = parseChooserSelections(self->portal.wayland, child.output, self->session->multiple());
              if (selections.empty()) {
                std::fprintf(
                    stderr, "screencast: chooser returned no valid selections: %s\n", firstLine(child.output).c_str()
                );
                self->finish(1, {});
                return;
              }
              self->startCaptures(std::move(selections));
            }
        );
      }

      void fallbackFirstOutput() {
        const auto& outputs = portal.wayland.outputs();
        if (outputs.empty()) {
          finish(2, {});
          return;
        }
        startCaptures({selectionForOutput(outputs.front())});
      }

      void startCaptures(std::vector<Session::Selection> selections) {
        if (selections.empty()) {
          finish(1, {});
          return;
        }
        if (!session->multiple() && selections.size() > 1) {
          selections.resize(1);
        }
        session->setSelections(selections);

        pending.clear();
        pending.reserve(selections.size());
        auto self = shared_from_this();
        std::weak_ptr<StartOperation> weakSelf = self;
        CaptureCursorMode cursorMode = CaptureCursorMode::Hidden;
        if (session->cursorMode() == kCursorEmbedded) {
          cursorMode = CaptureCursorMode::Embedded;
        } else if (session->cursorMode() == kCursorMetadata) {
          if (portal.pipewire.supportsCursorMetadata()) {
            cursorMode = CaptureCursorMode::Metadata;
          } else {
            std::fprintf(stderr, "screencast: PipeWire 1.4.8 is required for cursor metadata; embedding cursor\n");
            cursorMode = CaptureCursorMode::Embedded;
          }
        }

        for (const Session::Selection& selection : selections) {
          const size_t index = pending.size();
          pending.push_back(PendingCapture{selection, nullptr, {}, false});
          ConstraintsCallback callback = [weakSelf, index](const CaptureConstraints& constraints) {
            auto self = weakSelf.lock();
            if (!self || self->done) {
              return;
            }
            self->constraintsReady(index, constraints);
          };

          if (selection.kind == Session::SourceKind::Monitor) {
            pending[index].capture =
                portal.wayland.createOutputCapture(selection.output, cursorMode, std::move(callback));
          } else {
            pending[index].capture =
                portal.wayland.createToplevelCapture(selection.identifier, cursorMode, std::move(callback));
          }

          if (!pending[index].capture) {
            std::fprintf(
                stderr, "screencast: unable to create capture for %s\n",
                selection.kind == Session::SourceKind::Monitor ? selection.output.c_str() : selection.identifier.c_str()
            );
            finish(2, {});
            return;
          }
        }

        timeoutTimer = portal.loop.addTimer(5000, [weakSelf]() {
          auto self = weakSelf.lock();
          if (!self || self->done) {
            return;
          }
          self->timeoutTimer = 0;
          std::fprintf(stderr, "screencast: timed out waiting for capture constraints\n");
          self->finish(2, {});
        });
        maybeCompleteCaptures();
      }

      void constraintsReady(size_t index, const CaptureConstraints& constraints) {
        if (done || index >= pending.size()) {
          return;
        }
        pending[index].constraints = constraints;
        pending[index].hasConstraints = true;
        maybeCompleteCaptures();
      }

      void maybeCompleteCaptures() {
        if (done || pending.empty()) {
          return;
        }
        for (const PendingCapture& item : pending) {
          if (!item.capture || !item.hasConstraints) {
            return;
          }
        }

        if (timeoutTimer != 0) {
          const int timer = timeoutTimer;
          timeoutTimer = 0;
          portal.loop.removeTimer(timer);
        }

        for (PendingCapture& item : pending) {
          uint32_t width = item.constraints.bufferWidth;
          uint32_t height = item.constraints.bufferHeight;
          if (width == 0 && item.selection.width > 0) {
            width = static_cast<uint32_t>(item.selection.width);
          }
          if (height == 0 && item.selection.height > 0) {
            height = static_cast<uint32_t>(item.selection.height);
          }
          if (width == 0 || height == 0) {
            std::fprintf(stderr, "screencast: capture reported empty buffer size\n");
            finish(2, {});
            return;
          }

          auto stream = portal.pipewire.createStream(
              width, height, item.constraints, static_cast<uint32_t>(std::max(0, portal.config.screencast.maxFps)),
              item.capture->hasCursorMetadata()
          );
          if (!stream) {
            std::fprintf(stderr, "screencast: unable to create PipeWire stream\n");
            finish(2, {});
            return;
          }

          // Clear constraintsCb before transfer to break the reference cycle
          // (callback may hold a weak_ptr whose closure captured other state).
          item.capture->constraintsCb = nullptr;

          const std::string sessionPath = session->path();
          Impl* portalPtr = &portal;
          if (!session->addStream(
                  portal.loop, portal.wayland, std::move(item.capture), std::move(stream), item.constraints,
                  item.selection, static_cast<uint32_t>(std::max(0, portal.config.screencast.maxFps)),
                  [portalPtr, sessionPath]() {
                    const auto it = portalPtr->sessions.find(sessionPath);
                    if (it != portalPtr->sessions.end()) {
                      it->second->closeByBackend();
                    }
                  }
              )) {
            finish(2, {});
            return;
          }
        }

        // Process pending PipeWire events so stream node IDs are resolved.
        portal.pipewire.processPending();
        finish(0, buildResults());
      }

      PortalResults buildResults() {
        PortalResults results;
        std::vector<StreamTuple> streams;
        for (const Session::StreamResult& stream : session->streamResults()) {
          PortalResults properties;
          properties.emplace("size", sdbus::Variant{sdbus::Struct<int32_t, int32_t>{stream.width, stream.height}});
          properties.emplace("source_type", sdbus::Variant{stream.sourceType});
          if (stream.sourceType == kSourceMonitor) {
            properties.emplace("position", sdbus::Variant{sdbus::Struct<int32_t, int32_t>{stream.x, stream.y}});
          }
          if (!stream.mappingId.empty()) {
            properties.emplace("mapping_id", sdbus::Variant{stream.mappingId});
          }
          streams.emplace_back(stream.nodeId, std::move(properties));
        }
        results.emplace("streams", sdbus::Variant{streams});

        if (session->persistMode() > 0) {
          const std::string token = makeUuid();
          results.emplace("persist_mode", sdbus::Variant{session->persistMode()});
          results.emplace("restore_data", session->restoreDataVariant(token));
          if (session->persistMode() == 1) {
            portal.memoryRestores[token] = session->selections();
          } else if (session->persistMode() == 2) {
            savePersistentRestore(token, session->selections());
          }
        }

        return results;
      }
    };

    std::set<std::shared_ptr<StartOperation>> inflight;

    void start(PortalResponse&& result, const std::string& handle, const std::string& sessionPath) {
      const auto sessionIt = sessions.find(sessionPath);
      if (sessionIt == sessions.end() || sessionIt->second->closed()) {
        auto request = std::make_shared<Request>(connection, handle, []() {});
        result.returnResults(uint32_t{2}, PortalResults{});
        loop.addTimer(0, [request = std::move(request)]() mutable { request.reset(); });
        return;
      }

      auto op = std::make_shared<StartOperation>(*this, std::move(result), sessionIt->second);
      inflight.insert(op);
      op->start(handle);
    }
  };

  ScreenCastPortal::ScreenCastPortal(
      Loop& loop, sdbus::IConnection& connection, sdbus::IObject& object, const Config& config, WaylandContext& wayland,
      PipeWireContext& pipewire
  )
      : m_impl(std::make_unique<Impl>(loop, connection, object, config, wayland, pipewire)) {}

  ScreenCastPortal::~ScreenCastPortal() = default;

  void ScreenCastPortal::onConfigChanged(const Config&, const Config& newCfg) { m_impl->config = newCfg; }

} // namespace xdpu
