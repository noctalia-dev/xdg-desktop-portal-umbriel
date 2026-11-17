#include "picker/theme.h"
#include "render/core/color.h"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "{}", message);
      std::exit(EXIT_FAILURE);
    }
  }

  // A theme event as Umbriel's IPC sends it, including nested tables the picker ignores.
  std::string themeEvent(std::string_view accentPrimary = "\"#E53935FF\"", std::string_view radius = "10") {
    return std::string(R"({"event":"theme","data":{"background":"#15162EFF","text_primary":"#ECE9F4FF",)")
        + R"("text_muted":"#A5A7C6FF","accent_primary":)"
        + std::string(accentPrimary)
        + R"(,"accent_secondary":"#B3B8EDFF","warning":"#A0DDC0FF","error":"#ED8296FF",)"
        + R"("insert_hint":"#FFFFFF80","border":{"focused":"#FFFFFFFF"},"overview":{"background_tint":"#000000FF"},)"
        + R"("corner_radius":)"
        + std::string(radius)
        + "}}";
  }
} // namespace

int main() {
  using namespace xdpu::picker;

  const auto theme = parseThemeEvent(themeEvent());
  require(theme.has_value(), "a complete theme event did not parse");
  require(theme->cornerRadius == 10, "corner_radius was not read");
  const Palette palette = paletteFromTheme(*theme);
  require(palette.primary == hex("#E53935FF"), "accent_primary did not become the primary role");
  require(palette.surface == hex("#15162EFF"), "background did not become the surface role");
  require(palette.onSurface == hex("#ECE9F4FF"), "text_primary did not become the on-surface role");
  require(palette.onSurfaceVariant == hex("#A5A7C6FF"), "text_muted did not become the on-surface-variant role");
  require(palette.hover == hex("#B3B8EDFF"), "accent_secondary did not become the hover role");

  require(!parseThemeEvent(R"({"event":"windows","data":{}})"), "a non-theme event was accepted");
  require(
      !parseThemeEvent(
          R"({"event":"theme","data":{"background":"#000000FF","text_primary":"#FFFFFFFF","text_muted":"#888888FF",)"
          R"("accent_primary":"#FF0000FF","accent_secondary":"#00FF00FF","warning":"#FFFF00FF","corner_radius":4}})"
      ),
      "a theme without the error color was accepted"
  );
  require(!parseThemeEvent(themeEvent("\"red\"")), "a named color was accepted");
  require(!parseThemeEvent(themeEvent("\"#12345\"")), "a hex color with five digits was accepted");
  require(!parseThemeEvent(themeEvent("\"#E53935FF\"", "-1")), "a negative corner radius was accepted");
  require(!parseThemeEvent(themeEvent("\"#E53935FF\"", "501")), "an oversized corner radius was accepted");
  require(!parseThemeEvent(themeEvent("\"#E53935FF\"", "10.5")), "a fractional corner radius was accepted");
  require(!parseThemeEvent(R"({"event":"theme","data":[]})"), "a theme without a data object was accepted");
  require(!parseThemeEvent(R"({"event":"theme")"), "malformed JSON was accepted");

  require(radiusScale(12) == 1.0F, "Umbriel's 12 px radius should match the library's extra-large radius");
  require(radiusScale(0) == 0.0F, "a zero radius should square every corner");
  return EXIT_SUCCESS;
}
