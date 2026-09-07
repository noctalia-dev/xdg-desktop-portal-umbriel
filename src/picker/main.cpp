#include "picker/previews.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;

namespace {

  struct OutputItem {
    std::string name;
    std::string description;
    int width = 0;
    int height = 0;
  };

  struct WindowItem {
    std::string identifier;
    std::string appId;
    std::string title;
  };

  enum class RowKind {
    Monitor = 1,
    Window = 2,
  };

  struct Palette {
    std::string background;
    std::string textPrimary;
    std::string textMuted;
    std::string accentPrimary;
    std::string accentSecondary;
    std::string warning;
    std::string error;
    int cornerRadius = 0;

    bool operator==(const Palette&) const = default;
  };

  // Card preview slot, in logical pixels; thumbnails are scaled to fit it.
  constexpr int kPreviewWidth = 210;
  constexpr int kPreviewHeight = 132;

  struct PreviewCard {
    xdpu::PreviewSource source;
    GtkWidget* stack = nullptr;
  };

  struct AppState {
    bool multiple = false;
    bool showMonitors = false;
    bool showWindows = false;
    bool responding = false;
    bool updatingSelection = false;
    std::vector<OutputItem> outputs;
    std::vector<WindowItem> windows;
    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* outputList = nullptr;
    GtkWidget* windowList = nullptr;
    GtkWidget* shareButton = nullptr;
    GtkWidget* selectionLabel = nullptr;
    std::vector<PreviewCard> previewCards;
    std::unique_ptr<xdpu::Previews> previews;
    GtkCssProvider* paletteProvider = nullptr;
    GdkDisplay* display = nullptr;
    int paletteFd = -1;
    guint paletteWatch = 0;
    std::string paletteBuffer;
    std::optional<Palette> palette;
  };

  constexpr size_t kMaxPaletteMessageSize = 65536;

  void closeFd(int fd) {
    if (fd >= 0) {
      while (close(fd) < 0 && errno == EINTR) {
      }
    }
  }

  std::optional<std::string> validatedColor(const json& colors, const char* key) {
    const auto value = colors.find(key);
    if (value == colors.end() || !value->is_string()) {
      return std::nullopt;
    }

    GdkRGBA parsed{};
    const std::string input = value->get<std::string>();
    if (!gdk_rgba_parse(&parsed, input.c_str())) {
      return std::nullopt;
    }

    char* canonical = gdk_rgba_to_string(&parsed);
    std::string result(canonical);
    g_free(canonical);
    return result;
  }

  std::optional<Palette> parseThemeEvent(std::string_view response) {
    try {
      const json envelope = json::parse(response);
      if (envelope.value("event", "") != "theme") {
        return std::nullopt;
      }
      const auto values = envelope.find("data");
      if (values == envelope.end() || !values->is_object()) {
        return std::nullopt;
      }

      const auto background = validatedColor(*values, "background");
      const auto textPrimary = validatedColor(*values, "text_primary");
      const auto textMuted = validatedColor(*values, "text_muted");
      const auto accentPrimary = validatedColor(*values, "accent_primary");
      const auto accentSecondary = validatedColor(*values, "accent_secondary");
      const auto warning = validatedColor(*values, "warning");
      const auto error = validatedColor(*values, "error");
      const auto cornerRadius = values->find("corner_radius");
      if (!background
          || !textPrimary
          || !textMuted
          || !accentPrimary
          || !accentSecondary
          || !warning
          || !error
          || cornerRadius == values->end()
          || !cornerRadius->is_number_integer()) {
        return std::nullopt;
      }
      const int radius = cornerRadius->get<int>();
      if (radius < 0 || radius > 500) {
        return std::nullopt;
      }

      return Palette{
          .background = *background,
          .textPrimary = *textPrimary,
          .textMuted = *textMuted,
          .accentPrimary = *accentPrimary,
          .accentSecondary = *accentSecondary,
          .warning = *warning,
          .error = *error,
          .cornerRadius = radius,
      };
    } catch (const json::exception&) {
      return std::nullopt;
    }
  }

  std::string paletteSocketPath() {
    if (const char* configured = std::getenv("UMBRIEL_SOCKET"); configured != nullptr && configured[0] != '\0') {
      return configured;
    }
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0' && waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
      return std::string(runtimeDir) + "/umbriel-" + waylandDisplay + ".sock";
    }
    return {};
  }

  int openPaletteSubscription() {
    const std::string socketPath = paletteSocketPath();
    if (socketPath.empty()) {
      return -1;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return -1;
    }

    timeval timeout{.tv_sec = 0, .tv_usec = 250000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const size_t pathLength = socketPath.size();
    if (pathLength >= sizeof(address.sun_path)) {
      closeFd(fd);
      return -1;
    }
    std::memcpy(address.sun_path, socketPath.data(), pathLength);

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      closeFd(fd);
      return -1;
    }

    constexpr std::string_view request = "{\"cmd\":\"subscribe\",\"events\":[\"theme\"]}\n";
    size_t sent = 0;
    while (sent < request.size()) {
      const ssize_t size = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
      if (size < 0 && errno == EINTR) {
        continue;
      }
      if (size <= 0) {
        closeFd(fd);
        return -1;
      }
      sent += static_cast<size_t>(size);
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      closeFd(fd);
      return -1;
    }
    return fd;
  }

  const std::string& pickerStyleTemplate() {
    static const std::string style = [] {
      GError* error = nullptr;
      GBytes* bytes =
          g_resources_lookup_data("/dev/noctalia/umbriel/picker/style.css", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
      if (bytes == nullptr) {
        std::cerr
            << "umbriel-share-picker: unable to load style resource: "
            << (error != nullptr ? error->message : "unknown error")
            << '\n';
        g_clear_error(&error);
        return std::string{};
      }

      gsize size = 0;
      const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
      std::string result(data, size);
      g_bytes_unref(bytes);
      return result;
    }();
    return style;
  }

  void replaceAll(std::string& text, std::string_view token, std::string_view value) {
    size_t position = 0;
    while ((position = text.find(token, position)) != std::string::npos) {
      text.replace(position, token.size(), value.data(), value.size());
      position += value.size();
    }
  }

  std::string renderPickerStyle(const Palette& palette) {
    std::string css = pickerStyleTemplate();
    replaceAll(css, "@BACKGROUND@", palette.background);
    replaceAll(css, "@TEXT_PRIMARY@", palette.textPrimary);
    replaceAll(css, "@TEXT_MUTED@", palette.textMuted);
    replaceAll(css, "@ACCENT_PRIMARY@", palette.accentPrimary);
    replaceAll(css, "@ACCENT_SECONDARY@", palette.accentSecondary);
    replaceAll(css, "@RADIUS@", std::to_string(palette.cornerRadius) + "px");
    return css;
  }

  void applyPalette(AppState& state, const Palette& palette) {
    const std::string css = renderPickerStyle(palette);
    if (css.empty()) {
      return;
    }

    if (state.paletteProvider == nullptr) {
      state.paletteProvider = gtk_css_provider_new();
      gtk_style_context_add_provider_for_display(
          state.display, GTK_STYLE_PROVIDER(state.paletteProvider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
      );
    }
    gtk_css_provider_load_from_string(state.paletteProvider, css.c_str());
  }

  gboolean onPaletteEvent(gint fd, GIOCondition condition, gpointer userData) {
    auto* state = static_cast<AppState*>(userData);
    bool disconnected = (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0;

    char chunk[4096];
    while (true) {
      const ssize_t size = recv(fd, chunk, sizeof(chunk), 0);
      if (size > 0) {
        state->paletteBuffer.append(chunk, static_cast<size_t>(size));
        if (state->paletteBuffer.size() > kMaxPaletteMessageSize) {
          disconnected = true;
          break;
        }
        continue;
      }
      if (size == 0) {
        disconnected = true;
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        disconnected = true;
      }
      break;
    }

    size_t newline = 0;
    while ((newline = state->paletteBuffer.find('\n')) != std::string::npos) {
      const auto next = parseThemeEvent(std::string_view(state->paletteBuffer).substr(0, newline));
      state->paletteBuffer.erase(0, newline + 1);
      if (next && next != state->palette) {
        applyPalette(*state, *next);
        state->palette = next;
      }
    }

    if (disconnected) {
      closeFd(state->paletteFd);
      state->paletteFd = -1;
      state->paletteWatch = 0;
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  std::string readStdin() {
    std::string input;
    char buffer[4096];
    while (std::cin.good()) {
      std::cin.read(buffer, sizeof(buffer));
      input.append(buffer, static_cast<size_t>(std::cin.gcount()));
    }
    return input;
  }

  std::string jsonString(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  int jsonInt(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) {
      return 0;
    }
    return it->get<int>();
  }

  AppState parseRequest(const std::string& input) {
    AppState state;

    try {
      const json request = json::parse(input);
      if (!request.is_object()) {
        return state;
      }

      const auto multiple = request.find("multiple");
      state.multiple = multiple != request.end() && multiple->is_boolean() && multiple->get<bool>();

      const auto types = request.find("types");
      if (types != request.end() && types->is_array()) {
        for (const json& type : *types) {
          if (!type.is_string()) {
            continue;
          }
          const std::string value = type.get<std::string>();
          state.showMonitors = state.showMonitors || value == "monitor";
          state.showWindows = state.showWindows || value == "window";
        }
      }

      const auto outputs = request.find("outputs");
      if (outputs != request.end() && outputs->is_array()) {
        for (const json& output : *outputs) {
          if (!output.is_object()) {
            continue;
          }
          state.outputs.push_back({
              .name = jsonString(output, "name"),
              .description = jsonString(output, "description"),
              .width = jsonInt(output, "width"),
              .height = jsonInt(output, "height"),
          });
        }
      }

      const auto windows = request.find("windows");
      if (windows != request.end() && windows->is_array()) {
        for (const json& window : *windows) {
          if (!window.is_object()) {
            continue;
          }
          state.windows.push_back({
              .identifier = jsonString(window, "identifier"),
              .appId = jsonString(window, "app_id"),
              .title = jsonString(window, "title"),
          });
        }
      }
    } catch (const json::exception& error) {
      std::cerr << "umbriel-share-picker: invalid request JSON: " << error.what() << '\n';
    }

    return state;
  }

  void printResponse(const json& response) { std::cout << response.dump() << '\n' << std::flush; }

  void quitAfterResponse(AppState& state, const json& response) {
    if (state.responding) {
      return;
    }
    state.responding = true;
    if (state.previews) {
      state.previews->stop();
    }
    // Unmap before waiting for preview cleanup so the compositor can return focus.
    if (state.window != nullptr) {
      gtk_widget_set_visible(state.window, FALSE);
      gdk_display_flush(state.display);
    }
    printResponse(response);
    if (state.app != nullptr) {
      g_application_quit(G_APPLICATION(state.app));
    }
  }

  void cancel(AppState& state) { quitAfterResponse(state, json{{"selections", json::array()}}); }

  GList* selectedRows(GtkWidget* list) {
    if (list == nullptr) {
      return nullptr;
    }
    return gtk_flow_box_get_selected_children(GTK_FLOW_BOX(list));
  }

  bool hasSelection(GtkWidget* list) {
    GList* rows = selectedRows(list);
    const bool selected = rows != nullptr;
    g_list_free(rows);
    return selected;
  }

  void updateShareButton(AppState& state) {
    if (state.shareButton == nullptr) {
      return;
    }
    GList* screens = selectedRows(state.outputList);
    GList* windows = selectedRows(state.windowList);
    const guint count = g_list_length(screens) + g_list_length(windows);
    g_list_free(screens);
    g_list_free(windows);
    gtk_widget_set_sensitive(state.shareButton, count > 0);
    if (state.selectionLabel != nullptr) {
      const std::string text = count == 0
          ? "Nothing selected"
          : std::to_string(count) + (count == 1 ? " source selected" : " sources selected");
      gtk_label_set_text(GTK_LABEL(state.selectionLabel), text.c_str());
    }
  }

  void unselectList(GtkWidget* list) {
    if (list != nullptr) {
      gtk_flow_box_unselect_all(GTK_FLOW_BOX(list));
    }
  }

  void onSelectedRowsChanged(GtkFlowBox* list, gpointer userData) {
    auto* state = static_cast<AppState*>(userData);
    if (state->updatingSelection) {
      return;
    }

    if (!state->multiple && hasSelection(GTK_WIDGET(list))) {
      state->updatingSelection = true;
      unselectList(GTK_WIDGET(list) == state->outputList ? state->windowList : state->outputList);
      state->updatingSelection = false;
    }

    updateShareButton(*state);
  }

  std::string displayOrFallback(const std::string& value, const char* fallback) {
    return value.empty() ? std::string(fallback) : value;
  }

  GtkWidget* makeLabel(const std::string& text, bool bold, bool dim) {
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);

    if (bold) {
      char* escaped = g_markup_escape_text(text.c_str(), -1);
      const std::string markup = std::string("<b>") + escaped + "</b>";
      g_free(escaped);
      gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    } else {
      gtk_label_set_text(GTK_LABEL(label), text.c_str());
    }

    if (dim) {
      gtk_widget_add_css_class(label, "dim-label");
    }

    return label;
  }

  // A thumbnail is wider than the card, so the preview rides in a non-measured
  // overlay: the empty slot below it, not the captured texture, fixes the card size.
  GtkWidget* makePreview(AppState& state, RowKind kind, const std::string& identifier) {
    GtkWidget* frame = gtk_overlay_new();
    GtkWidget* slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(slot, kPreviewWidth, kPreviewHeight);
    gtk_overlay_set_child(GTK_OVERLAY(frame), slot);

    GtkWidget* stack = gtk_stack_new();
    gtk_widget_add_css_class(stack, "preview");
    gtk_widget_set_overflow(stack, GTK_OVERFLOW_HIDDEN);
    GtkWidget* fallback = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(fallback, GTK_ALIGN_CENTER);
    GtkWidget* icon = gtk_image_new_from_icon_name(
        kind == RowKind::Monitor ? "video-display-symbolic" : "application-x-executable-symbolic"
    );
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_append(GTK_BOX(fallback), icon);
    GtkWidget* label = gtk_label_new("Loading preview…");
    gtk_widget_add_css_class(label, "preview-caption");
    gtk_box_append(GTK_BOX(fallback), label);
    g_object_set_data(G_OBJECT(stack), "preview-label", label);
    gtk_stack_add_named(GTK_STACK(stack), fallback, "fallback");
    gtk_overlay_add_overlay(GTK_OVERLAY(frame), stack);

    state.previewCards.push_back({{kind == RowKind::Monitor, identifier}, stack});
    return frame;
  }

  void toggleSource(GtkFlowBoxChild* child) {
    auto* grid = GTK_FLOW_BOX(gtk_widget_get_parent(GTK_WIDGET(child)));
    const bool selected = gtk_flow_box_child_is_selected(child);
    gtk_widget_grab_focus(GTK_WIDGET(child));
    if (selected) {
      gtk_flow_box_unselect_child(grid, child);
    } else {
      gtk_flow_box_select_child(grid, child);
    }
  }

  GtkWidget* makeCard(
      AppState& state, RowKind kind, guint index, const std::string& identifier, const std::string& title,
      const std::string& subtitle, const std::string& detail
  ) {
    GtkWidget* row = gtk_flow_box_child_new();
    gtk_widget_add_css_class(row, "source-card");
    g_object_set_data(G_OBJECT(row), "xdpu-kind", GINT_TO_POINTER(static_cast<int>(kind)));
    g_object_set_data(G_OBJECT(row), "xdpu-index", GUINT_TO_POINTER(index));
    gtk_widget_set_tooltip_text(row, (title + "\n" + subtitle).c_str());

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), makePreview(state, kind, identifier));
    GtkWidget* check = gtk_image_new_from_icon_name("object-select-symbolic");
    gtk_widget_add_css_class(check, "selection-check");
    gtk_widget_set_halign(check, GTK_ALIGN_END);
    gtk_widget_set_valign(check, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), check);
    gtk_box_append(GTK_BOX(box), overlay);

    GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(labels, "card-labels");
    GtkWidget* titleLabel = makeLabel(title, true, false);
    GtkWidget* subtitleLabel = makeLabel(subtitle, false, true);
    gtk_label_set_max_width_chars(GTK_LABEL(titleLabel), 20);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitleLabel), 20);
    gtk_box_append(GTK_BOX(labels), titleLabel);
    gtk_box_append(GTK_BOX(labels), subtitleLabel);
    if (!detail.empty()) {
      GtkWidget* detailLabel = makeLabel(detail, false, true);
      gtk_widget_add_css_class(detailLabel, "source-detail");
      gtk_box_append(GTK_BOX(labels), detailLabel);
    }
    gtk_box_append(GTK_BOX(box), labels);
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(row), box);
    if (state.multiple) {
      GtkGesture* click = gtk_gesture_click_new();
      gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
      gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
      g_signal_connect(
          click, "pressed", G_CALLBACK(+[](GtkGestureClick* gesture, int, double, double, gpointer data) {
            gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
            toggleSource(GTK_FLOW_BOX_CHILD(data));
          }),
          row
      );
      gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));
    }
    return row;
  }

  GtkWidget* makePlaceholder(const char* text) {
    GtkWidget* label = gtk_label_new(text);
    gtk_widget_set_margin_top(label, 48);
    gtk_widget_set_margin_bottom(label, 48);
    gtk_widget_add_css_class(label, "dim-label");
    return label;
  }

  GtkWidget* makeSourceGrid(AppState& state, RowKind kind) {
    GtkWidget* list = gtk_flow_box_new();
    gtk_widget_add_css_class(list, "source-grid");
    gtk_widget_set_valign(list, GTK_ALIGN_START);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(list), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(list), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(list), 3);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(list), 12);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(list), 12);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(list), state.multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(list), TRUE);
    g_signal_connect(list, "selected-children-changed", G_CALLBACK(onSelectedRowsChanged), &state);

    if (kind == RowKind::Monitor) {
      for (size_t i = 0; i < state.outputs.size(); ++i) {
        const auto& output = state.outputs[i];
        const std::string detail = output.width > 0 && output.height > 0
            ? std::to_string(output.width) + " × " + std::to_string(output.height)
            : "";
        gtk_flow_box_insert(
            GTK_FLOW_BOX(list),
            makeCard(
                state, kind, static_cast<guint>(i), output.name, displayOrFallback(output.name, "Unnamed screen"),
                output.description, detail
            ),
            -1
        );
      }
    } else {
      for (size_t i = 0; i < state.windows.size(); ++i) {
        const auto& window = state.windows[i];
        gtk_flow_box_insert(
            GTK_FLOW_BOX(list),
            makeCard(
                state, kind, static_cast<guint>(i), window.identifier,
                displayOrFallback(window.title, "Untitled window"), displayOrFallback(window.appId, "Application"), ""
            ),
            -1
        );
      }
    }
    return list;
  }

  std::vector<size_t> visiblePreviews(const AppState& state) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < state.previewCards.size(); ++i) {
      GtkWidget* preview = state.previewCards[i].stack;
      if (!gtk_widget_get_mapped(preview)) {
        continue;
      }
      GtkWidget* scrolled = gtk_widget_get_ancestor(preview, GTK_TYPE_SCROLLED_WINDOW);
      graphene_rect_t bounds;
      if (scrolled != nullptr
          && gtk_widget_compute_bounds(preview, scrolled, &bounds)
          && bounds.origin.y < gtk_widget_get_height(scrolled)
          && bounds.origin.y + bounds.size.height > 0) {
        indices.push_back(i);
      }
    }
    return indices;
  }

  void requestVisiblePreviews(AppState& state) {
    if (state.previews) {
      state.previews->request(visiblePreviews(state));
    }
  }

  // Posted from the capture worker; drains every result queued since the last run.
  gboolean onPreviewResults(gpointer userData) {
    auto& state = *static_cast<AppState*>(userData);
    if (!state.previews) {
      return G_SOURCE_REMOVE;
    }
    for (auto& result : state.previews->takeResults()) {
      GtkWidget* stack = state.previewCards[result.index].stack;
      if (result.texture != nullptr) {
        GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(result.texture));
        g_object_unref(result.texture);
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_stack_add_named(GTK_STACK(stack), picture, "image");
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "image");
      } else {
        auto* label = GTK_LABEL(g_object_get_data(G_OBJECT(stack), "preview-label"));
        gtk_label_set_text(label, "Preview unavailable");
      }
    }
    // The worker is idle again: hand it whatever scrolled into view meanwhile.
    requestVisiblePreviews(state);
    return G_SOURCE_REMOVE;
  }

  GtkWidget* makeScrolledGrid(AppState& state, GtkWidget* list, const char* emptyText) {
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scrolled, TRUE);
    if (gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(list), 0) == nullptr) {
      // Parent the empty grid normally so selection queries remain valid.
      GtkWidget* empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_visible(list, FALSE);
      gtk_box_append(GTK_BOX(empty), list);
      gtk_box_append(GTK_BOX(empty), makePlaceholder(emptyText));
      list = empty;
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
    // "changed" covers the first allocation and every resize, "value-changed" scrolling.
    GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
    const auto onViewportChanged =
        G_CALLBACK(+[](GtkAdjustment*, gpointer data) { requestVisiblePreviews(*static_cast<AppState*>(data)); });
    g_signal_connect(adjustment, "changed", onViewportChanged, &state);
    g_signal_connect(adjustment, "value-changed", onViewportChanged, &state);
    return scrolled;
  }

  void appendSelectedRows(AppState& state, GtkWidget* list, json& selections) {
    GList* rows = selectedRows(list);
    for (GList* node = rows; node != nullptr; node = node->next) {
      auto* row = GTK_WIDGET(node->data);
      const auto kind = static_cast<RowKind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "xdpu-kind")));
      const guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "xdpu-index"));

      if (kind == RowKind::Monitor && index < state.outputs.size()) {
        selections.push_back({{"kind", "monitor"}, {"output", state.outputs[index].name}});
      } else if (kind == RowKind::Window && index < state.windows.size()) {
        selections.push_back({{"kind", "window"}, {"identifier", state.windows[index].identifier}});
      }

      if (!state.multiple && !selections.empty()) {
        break;
      }
    }
    g_list_free(rows);
  }

  void share(AppState& state) {
    json selections = json::array();
    appendSelectedRows(state, state.outputList, selections);
    if (state.multiple || selections.empty()) {
      appendSelectedRows(state, state.windowList, selections);
    }
    quitAfterResponse(state, json{{"selections", std::move(selections)}});
  }

  void onShareClicked(GtkButton*, gpointer userData) { share(*static_cast<AppState*>(userData)); }

  void onCancelClicked(GtkButton*, gpointer userData) { cancel(*static_cast<AppState*>(userData)); }

  gboolean onCloseRequest(GtkWindow*, gpointer userData) {
    cancel(*static_cast<AppState*>(userData));
    return TRUE;
  }

  gboolean onKeyPressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer userData) {
    auto* state = static_cast<AppState*>(userData);

    if (keyval == GDK_KEY_Escape) {
      cancel(*state);
      return TRUE;
    }

    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(state->window));
    if (state->multiple && keyval == GDK_KEY_space && focus != nullptr && GTK_IS_FLOW_BOX_CHILD(focus)) {
      toggleSource(GTK_FLOW_BOX_CHILD(focus));
      return TRUE;
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
      // Let focused controls (Cancel and the source tabs) handle their own activation.
      if (focus != nullptr && GTK_IS_BUTTON(focus)) {
        return FALSE;
      }
      if (state->shareButton != nullptr && gtk_widget_get_sensitive(state->shareButton)) {
        share(*state);
      }
      return TRUE;
    }

    return FALSE;
  }

  GtkWidget* makeContent(AppState& state) {
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(body, TRUE);
    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(heading, "picker-heading");
    GtkWidget* title = makeLabel("Choose what to share", true, false);
    gtk_widget_add_css_class(title, "picker-title");
    gtk_box_append(GTK_BOX(heading), title);
    GtkWidget* description =
        makeLabel(state.multiple ? "Select one or more sources to share." : "Select a source to share.", false, true);
    gtk_box_append(GTK_BOX(heading), description);
    gtk_box_append(GTK_BOX(content), heading);

    if (state.showMonitors) {
      state.outputList = makeSourceGrid(state, RowKind::Monitor);
    }
    if (state.showWindows) {
      state.windowList = makeSourceGrid(state, RowKind::Window);
    }

    if (state.showMonitors && state.showWindows) {
      GtkWidget* stack = gtk_stack_new();
      GtkWidget* switcher = gtk_stack_switcher_new();
      gtk_widget_set_vexpand(stack, TRUE);
      gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
      gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
      gtk_widget_add_css_class(switcher, "source-tabs");
      gtk_widget_remove_css_class(switcher, "linked");
      gtk_widget_set_halign(switcher, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(body), switcher);
      gtk_stack_add_titled(
          GTK_STACK(stack), makeScrolledGrid(state, state.outputList, "No screens available"), "screens", "Screens"
      );
      gtk_stack_add_titled(
          GTK_STACK(stack), makeScrolledGrid(state, state.windowList, "No windows available"), "windows", "Windows"
      );
      // Cards on the newly shown page become eligible for capture.
      g_signal_connect(
          stack, "notify::visible-child", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            requestVisiblePreviews(*static_cast<AppState*>(data));
          }),
          &state
      );
      if (state.outputs.empty() && !state.windows.empty()) {
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "windows");
      }
      gtk_box_append(GTK_BOX(body), stack);
    } else if (state.showMonitors) {
      gtk_box_append(GTK_BOX(body), makeScrolledGrid(state, state.outputList, "No screens available"));
    } else if (state.showWindows) {
      gtk_box_append(GTK_BOX(body), makeScrolledGrid(state, state.windowList, "No windows available"));
    } else {
      GtkWidget* placeholder = makePlaceholder("No share source types requested");
      gtk_widget_set_vexpand(placeholder, TRUE);
      gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
      gtk_box_append(GTK_BOX(body), placeholder);
    }

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(buttons, "picker-footer");
    state.selectionLabel = makeLabel("Nothing selected", false, true);
    gtk_widget_set_hexpand(state.selectionLabel, TRUE);
    gtk_box_append(GTK_BOX(buttons), state.selectionLabel);

    GtkWidget* cancelButton = gtk_button_new_with_label("Cancel");
    state.shareButton = gtk_button_new_with_label("Share");
    gtk_widget_add_css_class(state.shareButton, "suggested-action");
    gtk_widget_set_sensitive(state.shareButton, FALSE);

    g_signal_connect(cancelButton, "clicked", G_CALLBACK(onCancelClicked), &state);
    g_signal_connect(state.shareButton, "clicked", G_CALLBACK(onShareClicked), &state);

    gtk_box_append(GTK_BOX(buttons), cancelButton);
    gtk_box_append(GTK_BOX(buttons), state.shareButton);
    gtk_box_append(GTK_BOX(content), body);
    gtk_box_append(GTK_BOX(content), buttons);
    return content;
  }

  void onActivate(GtkApplication* app, gpointer userData) {
    auto* state = static_cast<AppState*>(userData);
    state->app = app;

    GtkWidget* window = gtk_application_window_new(app);
    state->window = window;
    state->display = gtk_widget_get_display(window);
    gtk_widget_add_css_class(window, "umbriel-picker");
    gtk_window_set_title(GTK_WINDOW(window), "Share a screen or window");
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 560);

    // No custom titlebar: Umbriel owns the window decorations and their visibility.
    gtk_window_set_child(GTK_WINDOW(window), makeContent(*state));
    gtk_window_set_default_widget(GTK_WINDOW(window), state->shareButton);

    GtkEventController* keyController = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keyController, GTK_PHASE_CAPTURE);
    g_signal_connect(keyController, "key-pressed", G_CALLBACK(onKeyPressed), state);
    gtk_widget_add_controller(window, keyController);

    g_signal_connect(window, "close-request", G_CALLBACK(onCloseRequest), state);
    state->paletteFd = openPaletteSubscription();
    if (state->paletteFd >= 0) {
      state->paletteWatch = g_unix_fd_add(
          state->paletteFd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL), onPaletteEvent, state
      );
    }
    // Use GTK theme colors until the compositor sends its current palette.
    if (!state->palette) {
      applyPalette(
          *state,
          Palette{
              .background = "@theme_bg_color",
              .textPrimary = "@theme_fg_color",
              .textMuted = "alpha(@theme_fg_color, 0.6)",
              .accentPrimary = "@theme_selected_bg_color",
              .accentSecondary = "@theme_selected_bg_color",
              .warning = "",
              .error = "",
              .cornerRadius = 8,
          }
      );
    }
    gtk_window_present(GTK_WINDOW(window));
    if (!state->previewCards.empty()) {
      std::vector<xdpu::PreviewSource> sources;
      sources.reserve(state->previewCards.size());
      for (const PreviewCard& card : state->previewCards) {
        sources.push_back(card.source);
      }
      state->previews =
          std::make_unique<xdpu::Previews>(std::move(sources), [state] { g_idle_add(onPreviewResults, state); });
      requestVisiblePreviews(*state);
    }
  }

} // namespace

int main(int argc, char** argv) {
  AppState state = parseRequest(readStdin());

  // GTK_CSD=0 asks the window manager for decorations; an explicit setting still wins.
  g_setenv("GTK_CSD", "0", FALSE);
  // Cairo keeps this small snapshot UI inexpensive; respect renderer overrides.
  g_setenv("GSK_RENDERER", "cairo", FALSE);
  gtk_init();

  GtkApplication* app = gtk_application_new("dev.noctalia.UmbrielSharePicker", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(onActivate), &state);
  const int status = g_application_run(G_APPLICATION(app), argc, argv);
  if (!state.responding) {
    cancel(state);
  }
  state.previews.reset();
  if (state.paletteWatch != 0) {
    g_source_remove(state.paletteWatch);
  }
  if (state.paletteFd >= 0) {
    closeFd(state.paletteFd);
  }
  if (state.paletteProvider != nullptr) {
    gtk_style_context_remove_provider_for_display(state.display, GTK_STYLE_PROVIDER(state.paletteProvider));
    g_object_unref(state.paletteProvider);
  }
  g_object_unref(app);

  (void)status;
  return 0;
}
