#include "picker/picker_window.h"

#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "picker/decoration.h"
#include "render/animation/animation_manager.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"
#include "ui/controls/separator.h"
#include "ui/controls/spacer.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <print>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <wayland-client.h>

using nlohmann::json;

namespace xdpu::picker {

  namespace {
    constexpr float kPagePaddingH = 24.0F;
    constexpr float kGridPaddingV = 16.0F;
    constexpr float kCardGap = 12.0F;
    constexpr float kCardMinWidth = 216.0F;
    constexpr std::size_t kMaxColumns = 3;
    constexpr float kCardPadding = 3.0F;
    constexpr float kCardBorder = 2.0F;
    constexpr float kPreviewHeight = 132.0F;
    constexpr float kLabelPaddingH = 9.0F;
    constexpr float kLabelPaddingV = 10.0F;
    constexpr float kCheckSize = 16.0F;

    std::string displayOrFallback(const std::string& value, const char* fallback) {
      return value.empty() ? std::string(fallback) : value;
    }

    void plain(Label& label) { label.setUseMarkup(false); }
  } // namespace

  // One selectable source: the preview slot over its labels. The whole card is one focusable input area.
  class SourceCard final : public Flex {
  public:
    SourceCard(
        bool monitor, const std::string& title, const std::string& subtitle, const std::string& detail,
        std::function<void()> pressed, std::function<void()> focused
    ) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setPadding(kCardPadding);
      setRadius(Style::scaledRadiusXl());

      auto slot = ui::column({
          .align = FlexAlign::Stretch,
          .justify = FlexJustify::Center,
          .fill = colorSpecFromRole(ColorRole::SurfaceContainerHighest),
          .radius = Style::scaledRadiusLg(),
          .clipChildren = true,
          .height = kPreviewHeight,
      });
      m_fallback = static_cast<Flex*>(slot->addChild(
          ui::column(
              {.align = FlexAlign::Center, .gap = Style::spaceSm},
              ui::glyph({
                  .glyph = monitor ? "device-desktop" : "app-window",
                  .glyphSize = 32.0F,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              }),
              ui::label({
                  .out = &m_caption,
                  .text = "Loading preview…",
                  .fontSize = Style::fontSizeCaption,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .configure = plain,
              })
          )
      ));
      m_image = static_cast<Image*>(slot->addChild(
          ui::image({
              .fit = ImageFit::Contain,
              .radius = Style::scaledRadiusLg(),
              .height = kPreviewHeight,
              .visible = false,
          })
      ));
      addChild(std::move(slot));

      auto labels = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .paddingV = kLabelPaddingV, .paddingH = kLabelPaddingH}
      );
      labels->addChild(
          ui::row(
              {.align = FlexAlign::Start, .gap = Style::spaceSm},
              ui::label({
                  .out = &m_title,
                  .text = title,
                  .fontWeight = FontWeight::Bold,
                  .maxLines = 2,
                  .ellipsize = TextEllipsize::End,
                  .flexGrow = 1.0F,
                  .configure = plain,
              }),
              ui::glyph({
                  .out = &m_check,
                  .glyph = "check",
                  .glyphSize = kCheckSize,
                  .color = colorSpecFromRole(ColorRole::Primary),
                  .visible = false,
              })
          )
      );
      labels->addChild(
          ui::label({
              .out = &m_subtitle,
              .text = subtitle,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .configure = plain,
          })
      );
      if (!detail.empty()) {
        labels->addChild(
            ui::label({
                .out = &m_detail,
                .text = detail,
                .fontSize = Style::fontSizeCaption,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
                .configure = plain,
            })
        );
      }
      addChild(std::move(labels));

      auto area = std::make_unique<InputArea>();
      area->setParticipatesInLayout(false);
      area->setZIndex(1);
      area->setFocusable(true);
      area->setTabStop(true);
      area->setOnEnter([this](const InputArea::PointerData&) { applyState(); });
      area->setOnLeave([this] { applyState(); });
      area->setOnPress([pressed = std::move(pressed)](const InputArea::PointerData& data) {
        if (data.pressed && data.button == BTN_LEFT) {
          pressed();
        }
      });
      area->setOnFocusGain([this, focused = std::move(focused)] {
        applyState();
        focused();
      });
      area->setOnFocusLoss([this] { applyState(); });
      m_area = static_cast<InputArea*>(addChild(std::move(area)));
      applyState();
    }

    InputArea* area() const noexcept { return m_area; }

    void setState(bool selected, bool focusVisible) {
      m_selected = selected;
      m_focusVisible = focusVisible;
      applyState();
    }

    // Labels wrap and ellipsize within the cell the grid assigns.
    void fitWidth(float cellWidth) {
      const float inner = std::max(0.0F, cellWidth - 2.0F * (kCardPadding + kLabelPaddingH));
      if (inner == m_fittedWidth) {
        return;
      }
      m_fittedWidth = inner;
      m_title->setMaxWidth(std::max(0.0F, inner - kCheckSize - Style::spaceSm));
      m_subtitle->setMaxWidth(inner);
      if (m_detail != nullptr) {
        m_detail->setMaxWidth(inner);
      }
    }

    bool showThumbnail(Renderer& renderer, const Thumbnail& thumbnail) {
      if (!m_image->setSourceRaw(
              renderer, thumbnail.rgba.data(), thumbnail.rgba.size(), thumbnail.width, thumbnail.height,
              thumbnail.width * 4, PixmapFormat::RGBA
          )) {
        return false;
      }
      m_fallback->setVisible(false);
      m_image->setVisible(true);
      return true;
    }

    void showUnavailable() { m_caption->setText("Preview unavailable"); }

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      syncArea();
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      syncArea();
    }

  private:
    void syncArea() {
      m_area->setPosition(0.0F, 0.0F);
      m_area->setFrameSize(width(), height());
    }

    void applyState() {
      const bool hovered = m_area != nullptr && m_area->hovered();
      const bool focusRing = m_focusVisible && m_area != nullptr && m_area->focused();
      ColorRole fill = ColorRole::SurfaceContainer;
      ColorRole border = ColorRole::OutlineVariant;
      if (m_selected) {
        fill = ColorRole::PrimaryContainer;
        border = ColorRole::Primary;
      } else if (hovered || focusRing) {
        fill = ColorRole::SurfaceContainerHigh;
        border = ColorRole::Secondary;
      }
      if (focusRing || (hovered && !m_selected)) {
        border = ColorRole::Secondary;
      }
      setFill(colorSpecFromRole(fill));
      setBorder(colorSpecFromRole(border), kCardBorder);
      m_check->setVisible(m_selected);
    }

    InputArea* m_area = nullptr;
    Flex* m_fallback = nullptr;
    Label* m_caption = nullptr;
    Image* m_image = nullptr;
    Label* m_title = nullptr;
    Glyph* m_check = nullptr;
    Label* m_subtitle = nullptr;
    Label* m_detail = nullptr;
    bool m_selected = false;
    bool m_focusVisible = false;
    float m_fittedWidth = -1.0F;
  };

  // Equal-width columns (up to kMaxColumns, each at least kCardMinWidth wide when space allows), rows as tall as
  // their tallest card. The height follows the content so an enclosing ScrollView can scroll it.
  class CardGrid final : public Node {
  public:
    std::size_t columns() const noexcept { return m_columns; }

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      const float available = constraints.hasMaxWidth ? constraints.maxWidth : width();
      return constraints.constrain({.width = available, .height = place(renderer, available, false)});
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      setPosition(rect.x, rect.y);
      setSize(rect.width, place(renderer, rect.width, true));
    }

    void doLayout(Renderer& renderer) override { setSize(width(), place(renderer, width(), true)); }

  private:
    float place(Renderer& renderer, float width, bool arrange) {
      const float inner = std::max(0.0F, width - 2.0F * kPagePaddingH);
      const auto fit = static_cast<std::size_t>(std::floor((inner + kCardGap) / (kCardMinWidth + kCardGap)));
      m_columns = std::clamp<std::size_t>(fit, 1, kMaxColumns);
      const float cellWidth =
          std::max(0.0F, (inner - kCardGap * static_cast<float>(m_columns - 1)) / static_cast<float>(m_columns));

      std::vector<Node*> cards;
      for (const auto& child : children()) {
        if (child->visible()) {
          cards.push_back(child.get());
        }
      }

      float y = kGridPaddingV;
      for (std::size_t start = 0; start < cards.size(); start += m_columns) {
        const std::size_t end = std::min(cards.size(), start + m_columns);
        float rowHeight = 0.0F;
        for (std::size_t index = start; index < end; ++index) {
          static_cast<SourceCard*>(cards[index])->fitWidth(cellWidth);
          LayoutConstraints constraints;
          constraints.setExactWidth(cellWidth);
          rowHeight = std::max(rowHeight, cards[index]->measure(renderer, constraints).height);
        }
        if (arrange) {
          for (std::size_t index = start; index < end; ++index) {
            const float x = kPagePaddingH + static_cast<float>(index - start) * (cellWidth + kCardGap);
            cards[index]->arrange(renderer, {.x = x, .y = y, .width = cellWidth, .height = rowHeight});
          }
        }
        y += rowHeight + (end < cards.size() ? kCardGap : 0.0F);
      }
      return y + kGridPaddingV;
    }

    std::size_t m_columns = 1;
  };

  PickerWindow::PickerWindow(
      ShareRequest request, WaylandConnection& connection, RenderContext& renderer, AnimationManager& animations
  )
      : m_request(std::move(request)), m_connection(connection), m_renderer(renderer), m_animations(animations) {
    const auto addSources = [this](Kind kind, std::size_t count) {
      Page page;
      page.kind = kind;
      for (std::size_t item = 0; item < count; ++item) {
        Source source;
        source.kind = kind;
        source.item = item;
        if (kind == Kind::Monitor) {
          const auto& output = m_request.outputs[item];
          source.title = displayOrFallback(output.title, "Unnamed screen");
          source.subtitle = output.description;
          if (output.width > 0 && output.height > 0) {
            source.detail = std::format("{} × {}", output.width, output.height);
          }
        } else {
          const auto& window = m_request.windows[item];
          source.title = displayOrFallback(window.title, "Untitled window");
          source.subtitle = displayOrFallback(window.appId, "Application");
        }
        page.sources.push_back(m_sources.size());
        m_sources.push_back(std::move(source));
      }
      m_pages.push_back(std::move(page));
    };
    if (m_request.showMonitors) {
      addSources(Kind::Monitor, m_request.outputs.size());
    }
    if (m_request.showWindows) {
      addSources(Kind::Window, m_request.windows.size());
    }
    if (m_pages.size() == 2 && m_request.outputs.empty() && !m_request.windows.empty()) {
      m_activePage = 1;
    }

    // A single-source share most often targets the screen the user is on: preselect and focus the compositor's
    // focused output so Enter shares it. Without focus info only a lone screen is unambiguous. Multiple-selection
    // requests always start empty, as a preselection there would have to be undone.
    if (!m_request.multiple && m_request.showMonitors) {
      const auto focused = std::ranges::find_if(m_request.outputs, &OutputItem::focused);
      std::optional<std::size_t> item;
      if (focused != m_request.outputs.end()) {
        item = static_cast<std::size_t>(focused - m_request.outputs.begin());
      } else if (m_request.outputs.size() == 1) {
        item = 0;
      }
      if (item) {
        // Monitors come first in m_sources.
        m_sources[*item].selected = true;
        m_restoreFocus = item;
      }
    }

    m_surface = std::make_unique<ToplevelSurface>(m_connection);
    m_surface->setRenderContext(&m_renderer);
    m_surface->setAnimationManager(&m_animations);
    requestServerSideDecoration(*m_surface, m_connection.extension<DecorationManager>());
    m_surface->setClosedCallback([this] { cancel(); });
    m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
    m_surface->setPrepareFrameCallback([this](bool, bool) { prepare(); });
    m_surface->setUpdateCallback([] {});
    m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_connection.setCursorShape(serial, shape);
    });
    if (!m_surface->initialize(
            nullptr,
            {.width = 760,
             .height = 560,
             .minWidth = 360,
             .minHeight = 320,
             .title = "Share a screen or window",
             .appId = "dev.noctalia.UmbrielSharePicker"}
        )) {
      throw std::runtime_error("cannot create the picker window");
    }

    if (!m_sources.empty()) {
      std::vector<PreviewSource> sources;
      sources.reserve(m_sources.size());
      for (const Source& source : m_sources) {
        sources.push_back({
            .monitor = source.kind == Kind::Monitor,
            .identifier = source.kind == Kind::Monitor ? m_request.outputs[source.item].name
                                                       : m_request.windows[source.item].identifier,
        });
      }
      // The worker posts to the main loop; DeferredCall::callLater is thread-safe.
      m_previews = std::make_unique<Previews>(std::move(sources), [this] {
        DeferredCall::callLater([this] { onPreviewResults(); });
      });
    }
  }

  PickerWindow::~PickerWindow() {
    closeSurface();
    stopPreviews();
  }

  void PickerWindow::prepare() {
    if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0) {
      return;
    }
    if (!m_renderer.makeCurrent(m_surface->renderTarget())) {
      return;
    }
    Renderer& renderer = m_surface->renderTarget().renderer();
    m_preparing = true;
    {
      UiPhaseScope phase(UiPhase::Layout);
      if (!m_root || m_rebuildRequested) {
        buildScene();
      }
      showPreviews(renderer);
      m_root->arrange(
          renderer,
          {.x = 0.0F,
           .y = 0.0F,
           .width = static_cast<float>(m_surface->width()),
           .height = static_cast<float>(m_surface->height())}
      );
    }
    m_preparing = false;
    requestVisiblePreviews();
  }

  void PickerWindow::buildScene() {
    if (!m_restoreFocus) {
      m_restoreFocus = focusedSource();
    }
    m_rebuildRequested = false;
    m_dispatcher.setSceneRoot(nullptr);
    m_surface->setSceneRoot(nullptr);
    for (Source& source : m_sources) {
      source.card = nullptr;
      source.previewDirty = source.preview != PreviewState::Loading;
    }
    m_share = nullptr;

    auto root = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Surface)});
    root->setAnimationManager(&m_animations);

    root->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceLg, .paddingV = 20.0F, .paddingH = kPagePaddingH},
            ui::glyph({.glyph = "cast", .glyphSize = 32.0F, .color = colorSpecFromRole(ColorRole::Primary)}),
            ui::column(
                {.align = FlexAlign::Stretch, .gap = 6.0F, .flexGrow = 1.0F},
                ui::label({
                    .text = "Choose what to share",
                    .fontSize = Style::fontSizeHeader,
                    .fontWeight = FontWeight::Bold,
                    .configure = plain,
                }),
                ui::label({
                    .text = m_request.multiple ? "Select one or more sources to share." : "Select a source to share.",
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .configure = plain,
                })
            )
        )
    );

    if (m_pages.size() == 2) {
      root->addChild(
          ui::row(
              {.align = FlexAlign::Center, .paddingH = kPagePaddingH},
              ui::segmented({
                  .options =
                      std::vector<ui::SegmentedOption>{
                          {.label = "Screens", .glyph = "device-desktop", .tooltip = {}},
                          {.label = "Windows", .glyph = "app-window", .tooltip = {}},
                      },
                  .selectedIndex = m_activePage,
                  .onChange = [this](std::size_t index) { selectPage(index); },
              })
          )
      );
    }

    for (std::size_t index = 0; index < m_pages.size(); ++index) {
      auto body = buildPage(m_pages[index]);
      body->setVisible(index == m_activePage);
      root->addChild(std::move(body));
    }
    if (m_pages.empty()) {
      root->addChild(
          ui::column(
              {.align = FlexAlign::Center, .justify = FlexJustify::Center, .flexGrow = 1.0F},
              ui::label({
                  .text = "No share source types requested",
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .configure = plain,
              })
          )
      );
    }

    root->addChild(ui::separator({}));
    auto footer =
        ui::row({.align = FlexAlign::Center, .gap = Style::spaceLg, .paddingV = 16.0F, .paddingH = kPagePaddingH});
    // Several sources stay fixed: one target action cannot say which stream to replace.
    if (!m_request.multiple) {
      footer->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm},
              ui::checkbox({.checked = m_dynamic, .onChange = [this](bool checked) { m_dynamic = checked; }}),
              ui::label({.text = "Allow changing the source with Umbriel actions", .configure = plain})
          )
      );
    }
    footer->addChild(ui::spacer());
    footer->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = 10.0F},
            ui::button({
                .text = "Cancel",
                .glyph = "x",
                .variant = ButtonVariant::Default,
                .onClick = [this] { cancel(); },
            }),
            ui::button({
                .out = &m_share,
                .text = "Share",
                .glyph = "screen-share",
                .enabled = anySelected(),
                .variant = ButtonVariant::Primary,
                .onClick = [this] { share(); },
            })
        )
    );
    root->addChild(std::move(footer));

    m_root = std::move(root);
    m_surface->setSceneRoot(m_root.get());
    m_dispatcher.setSceneRoot(m_root.get());
    m_requestedPreviews.clear();
    refreshCards();
    if (m_restoreFocus && m_sources[*m_restoreFocus].card != nullptr) {
      m_dispatcher.setFocus(m_sources[*m_restoreFocus].card->area());
    }
    m_restoreFocus.reset();
  }

  std::unique_ptr<Node> PickerWindow::buildPage(Page& page) {
    page.scroll = nullptr;
    page.grid = nullptr;
    if (page.sources.empty()) {
      auto empty = ui::column(
          {.align = FlexAlign::Center, .paddingV = 48.0F, .flexGrow = 1.0F},
          ui::label({
              .text = page.kind == Kind::Monitor ? "No screens available" : "No windows available",
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = plain,
          })
      );
      page.body = empty.get();
      return empty;
    }

    auto scroll = ui::scrollView({
        .out = &page.scroll,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fillWidth = true,
        .flexGrow = 1.0F,
        .onScrollChanged = [this](float) {
          if (!m_preparing && m_surface) {
            m_surface->requestLayout();
          }
        },
    });
    scroll->content()->setAlign(FlexAlign::Stretch);
    auto grid = std::make_unique<CardGrid>();
    for (std::size_t index : page.sources) {
      Source& source = m_sources[index];
      auto card = std::make_unique<SourceCard>(
          source.kind == Kind::Monitor, source.title, source.subtitle, source.detail,
          [this, index] {
            setFocusVisible(false);
            if (m_sources[index].card != nullptr) {
              m_dispatcher.setFocus(m_sources[index].card->area());
            }
            activate(index);
          },
          [this, index, scrollView = page.scroll] {
            if (m_sources[index].card != nullptr) {
              scrollView->requestScrollIntoView(m_sources[index].card);
            }
          }
      );
      source.card = card.get();
      grid->addChild(std::move(card));
    }
    page.grid = static_cast<CardGrid*>(scroll->content()->addChild(std::move(grid)));
    page.body = scroll.get();
    return scroll;
  }

  void PickerWindow::showPreviews(Renderer& renderer) {
    for (Source& source : m_sources) {
      if (!source.previewDirty || source.card == nullptr) {
        continue;
      }
      source.previewDirty = false;
      if (source.preview == PreviewState::Ready && !source.card->showThumbnail(renderer, *source.thumbnail)) {
        source.preview = PreviewState::Unavailable;
        source.thumbnail.reset();
      }
      if (source.preview == PreviewState::Unavailable) {
        source.card->showUnavailable();
      }
    }
  }

  void PickerWindow::requestVisiblePreviews() {
    if (!m_previews || m_responding || m_activePage >= m_pages.size()) {
      return;
    }
    const Page& page = m_pages[m_activePage];
    if (page.scroll == nullptr) {
      return;
    }
    float viewX = 0.0F;
    float viewY = 0.0F;
    Node::absolutePosition(page.scroll, viewX, viewY);
    const float viewBottom = viewY + page.scroll->height();
    std::vector<std::size_t> visible;
    for (std::size_t index : page.sources) {
      const SourceCard* card = m_sources[index].card;
      if (card == nullptr || card->height() <= 0.0F) {
        continue;
      }
      float cardX = 0.0F;
      float cardY = 0.0F;
      Node::absolutePosition(card, cardX, cardY);
      if (cardY < viewBottom && cardY + card->height() > viewY) {
        visible.push_back(index);
      }
    }
    if (visible != m_requestedPreviews) {
      m_requestedPreviews = visible;
      m_previews->request(visible);
    }
  }

  // Posted from the capture worker; drains every result queued since the last run.
  void PickerWindow::onPreviewResults() {
    if (!m_previews || m_responding) {
      return;
    }
    for (auto& result : m_previews->takeResults()) {
      Source& source = m_sources[result.index];
      source.preview = result.thumbnail ? PreviewState::Ready : PreviewState::Unavailable;
      source.thumbnail = std::move(result.thumbnail);
      source.previewDirty = true;
    }
    // The worker is idle again: the next layout hands it whatever scrolled into view meanwhile.
    m_requestedPreviews.clear();
    m_surface->requestLayout();
  }

  void PickerWindow::selectPage(std::size_t page) {
    if (page >= m_pages.size() || page == m_activePage) {
      return;
    }
    m_activePage = page;
    for (std::size_t index = 0; index < m_pages.size(); ++index) {
      m_pages[index].body->setVisible(index == m_activePage);
    }
    m_requestedPreviews.clear();
    m_surface->requestLayout();
  }

  void PickerWindow::activate(std::size_t index) {
    if (m_request.multiple) {
      m_sources[index].selected = !m_sources[index].selected;
    } else {
      for (std::size_t other = 0; other < m_sources.size(); ++other) {
        m_sources[other].selected = other == index;
      }
    }
    refreshCards();
    m_surface->requestLayout();
  }

  void PickerWindow::moveFocus(std::size_t from, std::uint32_t sym) {
    const auto pageIt =
        std::ranges::find_if(m_pages, [from](const Page& page) { return std::ranges::contains(page.sources, from); });
    if (pageIt == m_pages.end() || pageIt->grid == nullptr) {
      return;
    }
    const auto& sources = pageIt->sources;
    const auto position = static_cast<long>(std::ranges::find(sources, from) - sources.begin());
    const auto columns = static_cast<long>(pageIt->grid->columns());
    long target = position;
    if (KeySymbol::isLeft(sym)) {
      target -= 1;
    } else if (KeySymbol::isRight(sym)) {
      target += 1;
    } else if (KeySymbol::isUp(sym)) {
      target -= columns;
    } else if (KeySymbol::isDown(sym)) {
      target += columns;
    }
    if (target < 0 || target >= static_cast<long>(sources.size()) || target == position) {
      return;
    }
    const std::size_t index = sources[static_cast<std::size_t>(target)];
    m_dispatcher.setFocus(m_sources[index].card->area());
    // In single selection, selection follows focus, as in a GtkFlowBox.
    if (!m_request.multiple) {
      activate(index);
    }
  }

  std::optional<std::size_t> PickerWindow::focusedSource() const {
    const InputArea* focused = m_dispatcher.focusedArea();
    if (focused == nullptr) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < m_sources.size(); ++index) {
      if (m_sources[index].card != nullptr && m_sources[index].card->area() == focused) {
        return index;
      }
    }
    return std::nullopt;
  }

  void PickerWindow::setFocusVisible(bool visible) {
    if (m_focusVisible != visible) {
      m_focusVisible = visible;
      refreshCards();
    }
  }

  void PickerWindow::refreshCards() {
    for (const Source& source : m_sources) {
      if (source.card != nullptr) {
        source.card->setState(source.selected, m_focusVisible);
      }
    }
    if (m_share != nullptr) {
      m_share->setEnabled(anySelected());
    }
  }

  bool PickerWindow::anySelected() const { return std::ranges::any_of(m_sources, &Source::selected); }

  void PickerWindow::themeChanged(const UmbrielTheme& theme) {
    if (m_responding) {
      return;
    }
    // Colors follow paletteChanged() on their own; radii are read when controls are built.
    if (applyThemeGlobals(theme)) {
      m_rebuildRequested = true;
    }
    m_surface->requestLayout();
  }

  void PickerWindow::pointer(const PointerEvent& event) {
    if (!m_surface || event.surface != m_surface->wlSurface()) {
      return;
    }
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_dispatcher.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_dispatcher.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_dispatcher.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      if (event.pressed) {
        setFocusVisible(false);
      }
      m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
      break;
    case PointerEvent::Type::Axis:
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      break;
    }
    if (m_surface) {
      m_surface->requestUpdate();
    }
  }

  void PickerWindow::keyboard(const KeyboardEvent& event) {
    if (!m_surface || m_responding) {
      return;
    }
    if (event.pressed && !event.preedit) {
      if (!event.repeat && KeySymbol::isEscape(event.sym)) {
        cancel();
        return;
      }
      setFocusVisible(true);
      const auto focused = focusedSource();
      const bool plainKey = event.modifiers == 0;
      if (plainKey
          && !event.repeat
          && KeySymbol::isEnter(event.sym)
          && (focused || m_dispatcher.focusedArea() == nullptr)) {
        if (anySelected()) {
          share();
        }
        return;
      }
      if (focused && plainKey && !event.repeat && KeySymbol::isSpace(event.sym)) {
        activate(*focused);
        return;
      }
      if (focused
          && plainKey
          && (KeySymbol::isLeft(event.sym)
              || KeySymbol::isRight(event.sym)
              || KeySymbol::isUp(event.sym)
              || KeySymbol::isDown(event.sym))) {
        moveFocus(*focused, event.sym);
        m_surface->requestUpdate();
        return;
      }
    }
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    if (m_surface) {
      m_surface->requestUpdate();
    }
  }

  void PickerWindow::share() {
    json selections = json::array();
    const auto append = [&](Kind kind) {
      for (const Source& source : m_sources) {
        if (!source.selected || source.kind != kind || (!m_request.multiple && !selections.empty())) {
          continue;
        }
        if (kind == Kind::Monitor) {
          selections.push_back({{"kind", "monitor"}, {"output", m_request.outputs[source.item].name}});
        } else {
          selections.push_back({{"kind", "window"}, {"identifier", m_request.windows[source.item].identifier}});
        }
      }
    };
    append(Kind::Monitor);
    append(Kind::Window);
    json response{{"selections", std::move(selections)}};
    if (!m_request.multiple) {
      response["dynamic"] = m_dynamic;
    }
    respond(response.dump());
  }

  void PickerWindow::cancel() { respond(json{{"selections", json::array()}}.dump()); }

  void PickerWindow::respond(const std::string& response) {
    if (m_responding) {
      return;
    }
    m_responding = true;
    if (m_previews) {
      m_previews->stop();
    }
    std::println("{}", response);
    std::fflush(stdout);
  }

  void PickerWindow::closeSurface() {
    if (!m_surface) {
      return;
    }
    // Images release their textures through the surface's renderer, so the scene goes first, in its GL context.
    m_renderer.makeCurrent(m_surface->renderTarget());
    m_dispatcher.setSceneRoot(nullptr);
    m_surface->setSceneRoot(nullptr);
    m_root.reset();
    for (Source& source : m_sources) {
      source.card = nullptr;
    }
    for (Page& page : m_pages) {
      page = Page{.kind = page.kind, .sources = std::move(page.sources)};
    }
    m_share = nullptr;
    m_surface.reset();
  }

  void PickerWindow::stopPreviews() { m_previews.reset(); }

} // namespace xdpu::picker
