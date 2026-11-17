#pragma once

#include "picker/previews.h"
#include "picker/request.h"
#include "picker/theme.h"
#include "render/scene/input_dispatcher.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AnimationManager;
class Button;
class Flex;
class Node;
class RenderContext;
class Renderer;
class ScrollView;
class ToplevelSurface;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

namespace xdpu::picker {

  class CardGrid;
  class SourceCard;

  // The share picker's toplevel: source cards with previews, grouped into Screens and Windows pages, and the
  // Cancel and Share buttons. The request is answered on stdout exactly once, by sharing, cancelling, or closing.
  class PickerWindow {
  public:
    PickerWindow(
        ShareRequest request, WaylandConnection& connection, RenderContext& renderer, AnimationManager& animations
    );
    ~PickerWindow();
    PickerWindow(const PickerWindow&) = delete;
    PickerWindow& operator=(const PickerWindow&) = delete;

    // True once the answer was written; the main loop then stops.
    bool done() const noexcept { return m_responding; }
    void cancel();
    void themeChanged(const UmbrielTheme& theme);
    void pointer(const PointerEvent& event);
    void keyboard(const KeyboardEvent& event);
    // Unmaps and destroys the window; call after the main loop, before stopPreviews().
    void closeSurface();
    // Joins the preview worker.
    void stopPreviews();

  private:
    enum class Kind : std::uint8_t { Monitor, Window };
    enum class PreviewState : std::uint8_t { Loading, Ready, Unavailable };

    struct Source {
      Kind kind = Kind::Monitor;
      // Index into the request's outputs or windows.
      std::size_t item = 0;
      std::string title;
      std::string subtitle;
      std::string detail;
      bool selected = false;
      PreviewState preview = PreviewState::Loading;
      std::optional<Thumbnail> thumbnail;
      // The card has not shown the current preview state yet.
      bool previewDirty = false;
      SourceCard* card = nullptr;
    };

    struct Page {
      Kind kind = Kind::Monitor;
      std::vector<std::size_t> sources;
      Node* body = nullptr;
      ScrollView* scroll = nullptr;
      CardGrid* grid = nullptr;
    };

    void prepare();
    void buildScene();
    std::unique_ptr<Node> buildPage(Page& page);
    void showPreviews(Renderer& renderer);
    void requestVisiblePreviews();
    void onPreviewResults();
    void selectPage(std::size_t page);
    void activate(std::size_t index);
    void moveFocus(std::size_t from, std::uint32_t sym);
    std::optional<std::size_t> focusedSource() const;
    void setFocusVisible(bool visible);
    void refreshCards();
    bool anySelected() const;
    void share();
    void respond(const std::string& response);

    ShareRequest m_request;
    WaylandConnection& m_connection;
    RenderContext& m_renderer;
    AnimationManager& m_animations;
    std::unique_ptr<ToplevelSurface> m_surface;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Node> m_root;
    Button* m_share = nullptr;

    std::vector<Source> m_sources;
    std::vector<Page> m_pages;
    std::size_t m_activePage = 0;
    std::unique_ptr<Previews> m_previews;
    std::vector<std::size_t> m_requestedPreviews;

    bool m_dynamic = false;
    bool m_responding = false;
    bool m_preparing = false;
    bool m_rebuildRequested = false;
    // Focus is drawn only after keyboard navigation, like GTK's :focus-visible.
    bool m_focusVisible = false;
    // Source to focus when the scene is next built.
    std::optional<std::size_t> m_restoreFocus;
  };

} // namespace xdpu::picker
