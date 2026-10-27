// Receives actual portal PipeWire frames in a private test session. Each armed
// expectation requires a later frame with the requested dimensions and pixels.
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <map>
#include <memory>
#include <pipewire/pipewire.h>
#include <print>
#include <sdbus-c++/sdbus-c++.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
  constexpr char kService[] = "org.freedesktop.impl.portal.desktop.umbriel";
  constexpr char kInterface[] = "org.freedesktop.impl.portal.ScreenCast";
  using Dict = std::map<std::string, sdbus::Variant>;
  using Stream = sdbus::Struct<uint32_t, Dict>;

  class Client {
  public:
    Client() {
      m_bus = sdbus::createSessionBusConnection();
      m_portal = sdbus::createProxy(
          *m_bus, sdbus::ServiceName{kService}, sdbus::ObjectPath{"/org/freedesktop/portal/desktop"}
      );
      m_session =
          sdbus::ObjectPath{"/org/freedesktop/portal/desktop/session/transform_test/s" + std::to_string(getpid())};
      const std::string request = "/org/freedesktop/portal/desktop/request/transform_test/r" + std::to_string(getpid());
      uint32_t response = 2;
      Dict results;
      m_portal->callMethod("CreateSession")
          .onInterface(kInterface)
          .withArguments(
              sdbus::ObjectPath{request + "create"}, m_session, std::string("org.umbriel.TransformTest"), Dict{}
          )
          .storeResultsTo(response, results);
      require(response == 0, "CreateSession failed");
      const Dict options{{"types", sdbus::Variant(uint32_t{3})}, {"cursor_mode", sdbus::Variant(uint32_t{1})}};
      m_portal->callMethod("SelectSources")
          .onInterface(kInterface)
          .withArguments(
              sdbus::ObjectPath{request + "select"}, m_session, std::string("org.umbriel.TransformTest"), options
          )
          .storeResultsTo(response, results);
      require(response == 0, "SelectSources failed");
      m_portal->callMethod("Start")
          .onInterface(kInterface)
          .withArguments(
              sdbus::ObjectPath{request + "start"}, m_session, std::string("org.umbriel.TransformTest"), std::string{},
              Dict{}
          )
          .storeResultsTo(response, results);
      require(response == 0 && results.contains("streams"), "Start failed");
      const auto streams = results.at("streams").get<std::vector<Stream>>();
      require(streams.size() == 1, "expected one portal stream");
      m_sourceNode = streams.front().get<0>();

      pw_init(nullptr, nullptr);
      m_loop = pw_main_loop_new(nullptr);
      require(m_loop != nullptr, "cannot create PipeWire loop");
      pw_properties* properties = pw_properties_new(
          PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_NODE_NAME,
          "umbriel-screencast-test", PW_KEY_NODE_WANT_DRIVER, "true", nullptr
      );
      static const pw_stream_events events{
          .version = PW_VERSION_STREAM_EVENTS,
          .destroy = nullptr,
          .state_changed = stateChanged,
          .control_info = nullptr,
          .io_changed = nullptr,
          .param_changed = formatChanged,
          .add_buffer = nullptr,
          .remove_buffer = nullptr,
          .process = process,
          .drained = nullptr,
          .command = nullptr,
          .trigger_done = nullptr,
      };
      m_stream =
          pw_stream_new_simple(pw_main_loop_get_loop(m_loop), "umbriel-screencast-test", properties, &events, this);
      require(m_stream != nullptr, "cannot create PipeWire consumer");
      std::array<uint8_t, 1024> storage{};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
      const spa_rectangle preferred{640, 360}, minimum{1, 1}, maximum{8192, 8192};
      const spa_fraction rate{30, 1}, minRate{0, 1}, maxRate{120, 1};
      const spa_pod* params[]{static_cast<const spa_pod*>(spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
          SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
          SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(2, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
          SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
          SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &minRate, &maxRate)
      ))};
      require(
          pw_stream_connect(m_stream, PW_DIRECTION_INPUT, m_sourceNode, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) >= 0,
          "cannot connect PipeWire consumer"
      );
      const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
      require(flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0, "cannot configure stdin");
      m_inputSource = pw_loop_add_io(pw_main_loop_get_loop(m_loop), STDIN_FILENO, SPA_IO_IN, false, inputReady, this);
      require(m_inputSource != nullptr, "cannot register stdin");
      std::println("ready node={}", m_sourceNode);
    }

    ~Client() {
      if (m_inputSource)
        pw_loop_destroy_source(pw_main_loop_get_loop(m_loop), m_inputSource);
      if (m_stream)
        pw_stream_destroy(m_stream);
      if (m_loop)
        pw_main_loop_destroy(m_loop);
      try {
        if (m_bus && !m_session.empty()) {
          auto session = sdbus::createProxy(*m_bus, sdbus::ServiceName{kService}, m_session);
          session->callMethod("Close").onInterface("org.freedesktop.impl.portal.Session");
        }
      } catch (const std::exception&) {
      }
      pw_deinit();
    }

    int run() {
      pw_main_loop_run(m_loop);
      return m_failed ? 1 : 0;
    }

  private:
    static void require(bool condition, const char* message) {
      if (!condition)
        throw std::runtime_error(message);
    }
    void fail(const std::string& message) {
      std::println(stderr, "screencast-test-client: {}", message);
      m_failed = true;
      pw_main_loop_quit(m_loop);
    }
    static void stateChanged(void* data, pw_stream_state, pw_stream_state state, const char* error) {
      auto& self = *static_cast<Client*>(data);
      std::println("state {}", pw_stream_state_as_string(state));
      if (state == PW_STREAM_STATE_ERROR)
        self.fail(error ? error : "PipeWire stream error");
    }
    static void formatChanged(void* data, uint32_t id, const spa_pod* param) {
      auto& self = *static_cast<Client*>(data);
      if (id != SPA_PARAM_Format || !param)
        return;
      if (spa_format_video_raw_parse(param, &self.m_info) < 0) {
        self.fail("cannot parse negotiated video format");
        return;
      }
      std::println("format width={} height={}", self.m_info.size.width, self.m_info.size.height);
      std::array<uint8_t, 1024> storage{};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
      const spa_pod* params[]{
          static_cast<const spa_pod*>(spa_pod_builder_add_object(
              &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
              SPA_POD_CHOICE_RANGE_Int(4, 2, 8), SPA_PARAM_BUFFERS_dataType,
              SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))
          )),
          static_cast<const spa_pod*>(spa_pod_builder_add_object(
              &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
              SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header))
          ))
      };
      if (pw_stream_update_params(self.m_stream, params, 2) < 0)
        self.fail("cannot negotiate capture buffers");
    }
    static void process(void* data) {
      auto& self = *static_cast<Client*>(data);
      pw_buffer* buffer = pw_stream_dequeue_buffer(self.m_stream);
      if (!buffer)
        return;
      spa_buffer* spaBuffer = buffer->buffer;
      if (spaBuffer && spaBuffer->n_datas > 0) {
        const spa_data& pixels = spaBuffer->datas[0];
        const uint32_t width = self.m_info.size.width, height = self.m_info.size.height;
        if (pixels.data
            && pixels.chunk
            && pixels.chunk->size
            && width
            && height
            && pixels.chunk->stride > 0
            && (pixels.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) == 0) {
          const auto stride = static_cast<uint32_t>(pixels.chunk->stride);
          const uint64_t required = static_cast<uint64_t>(stride) * (height - 1) + static_cast<uint64_t>(width) * 4;
          if (static_cast<uint64_t>(pixels.chunk->offset) + required > pixels.maxsize) {
            self.fail("video frame exceeds mapped buffer");
          } else {
            const auto* bytes = static_cast<const uint8_t*>(pixels.data) + pixels.chunk->offset;
            std::array<uint32_t, 4> actual{};
            for (uint32_t i = 0; i < actual.size(); ++i) {
              const uint32_t x = ((i & 1U) ? 3 : 1) * width / 4;
              const uint32_t y = ((i & 2U) ? 3 : 1) * height / 4;
              const uint8_t* pixel = bytes + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
              actual[i] = (static_cast<uint32_t>(pixel[2]) << 16) | (static_cast<uint32_t>(pixel[1]) << 8) | pixel[0];
            }
            ++self.m_frames;
            std::println(
                "frame number={} width={} height={} pixels={:06x},{:06x},{:06x},{:06x}", self.m_frames, width, height,
                actual[0], actual[1], actual[2], actual[3]
            );
            if (!self.m_expectation.empty()
                && self.m_frames > self.m_afterFrame
                && width == self.m_width
                && height == self.m_height
                && actual == self.m_pixels) {
              std::println("matched label={} node={} frame={}", self.m_expectation, self.m_sourceNode, self.m_frames);
              self.m_expectation.clear();
            }
          }
        }
      }
      pw_stream_queue_buffer(self.m_stream, buffer);
    }
    static void inputReady(void* data, int fd, uint32_t) {
      auto& self = *static_cast<Client*>(data);
      char bytes[2048];
      const ssize_t count = read(fd, bytes, sizeof(bytes));
      if (count == 0) {
        pw_main_loop_quit(self.m_loop);
        return;
      }
      if (count < 0) {
        if (errno != EAGAIN && errno != EINTR)
          self.fail("cannot read commands");
        return;
      }
      self.m_input.append(bytes, static_cast<size_t>(count));
      size_t newline;
      while ((newline = self.m_input.find('\n')) != std::string::npos) {
        const std::string line = self.m_input.substr(0, newline);
        self.m_input.erase(0, newline + 1);
        std::istringstream command(line);
        std::string action;
        command >> action;
        if (action == "quit") {
          pw_main_loop_quit(self.m_loop);
        } else if (action == "expect") {
          command >> self.m_expectation >> self.m_width >> self.m_height >> std::hex;
          for (uint32_t& pixel : self.m_pixels)
            command >> pixel;
          if (command.fail() || self.m_expectation.empty() || !self.m_width || !self.m_height) {
            self.fail("invalid expectation: " + line);
            return;
          }
          self.m_afterFrame = self.m_frames;
          std::println("expecting label={} after={}", self.m_expectation, self.m_afterFrame);
        } else {
          self.fail("unknown command: " + line);
          return;
        }
      }
    }

    std::unique_ptr<sdbus::IConnection> m_bus;
    std::unique_ptr<sdbus::IProxy> m_portal;
    sdbus::ObjectPath m_session;
    pw_main_loop* m_loop = nullptr;
    pw_stream* m_stream = nullptr;
    spa_source* m_inputSource = nullptr;
    spa_video_info_raw m_info{};
    uint32_t m_sourceNode = 0;
    uint64_t m_frames = 0, m_afterFrame = 0;
    std::string m_input, m_expectation;
    uint32_t m_width = 0, m_height = 0;
    std::array<uint32_t, 4> m_pixels{};
    bool m_failed = false;
  };
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  try {
    Client client;
    return client.run();
  } catch (const std::exception& error) {
    std::println(stderr, "screencast-test-client: {}", error.what());
    return 1;
  }
}
