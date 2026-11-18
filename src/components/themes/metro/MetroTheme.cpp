#include "MetroTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>

#include "fontIds.h"
#include "metroFontIds.h"

// Internal constants
namespace {
constexpr int hintFontId = UI_10_FONT_ID;

// BaseTheme::drawHintLabel only draws black text; this is the same layout in white.
void drawWhiteHintLabel(const GfxRenderer& renderer, const char* label, const int x, const int boxWidth,
                        const int boxTop, const int boxHeight) {
  constexpr int textPadding = 4;
  const int maxTextWidth = boxWidth - (textPadding * 2);

  const int textHeight = renderer.getTextHeight(hintFontId);
  const int textWidth = renderer.getTextWidth(hintFontId, label);
  if (textWidth <= maxTextWidth) {
    const int textY = boxTop + std::max(1, (boxHeight - textHeight) / 2);
    renderer.drawText(hintFontId, x + (boxWidth - 1 - textWidth) / 2, textY, label, false);
    return;
  }

  // Lines step by the glyph height: the font's full line height would push
  // the second line below the bar.
  constexpr int lineGap = 2;
  const int step = textHeight + lineGap;
  const auto lines = renderer.wrappedText(hintFontId, label, maxTextWidth, 2);
  const int block = static_cast<int>(lines.size()) * step - lineGap;
  int lineY = boxTop + std::max(1, (boxHeight - block) / 2);
  for (const auto& line : lines) {
    const int lineWidth = renderer.getTextWidth(hintFontId, line.c_str());
    renderer.drawText(hintFontId, x + (boxWidth - 1 - lineWidth) / 2, lineY, line.c_str(), false);
    lineY += step;
  }
}

// The light fonts hold printable ASCII only. Largest first.
constexpr int pageTitleFonts[] = {METRO_NUMBER_LARGE_FONT_ID, METRO_NUMBER_FONT_ID};
constexpr size_t pageTitleBytes = 48;
// From the bottom of the header band up to the title's baseline: room for descenders.
constexpr int pageTitleBaselineUp = 14;

// Lowercase copy of an ASCII title. False when the title is empty, too long, or not plain ASCII.
bool lowercaseAscii(const char* title, char* out, const size_t capacity) {
  if (title == nullptr || title[0] == '\0') return false;
  size_t length = 0;
  for (; title[length] != '\0'; ++length) {
    const auto c = static_cast<unsigned char>(title[length]);
    if (c < 0x20 || c > 0x7E || length + 1 >= capacity) return false;
    out[length] = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  out[length] = '\0';
  return true;
}
}  // namespace

void MetroTheme::drawHeader(const GfxRenderer& renderer, const Rect rect, const char* title, const char* subtitle,
                            const bool backButton) const {
  const ThemeMetrics& metrics = MetroMetrics::values;
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  int maxWidth = rect.width - left - right - 2 * metrics.headerSidePadding;
  const bool hasSubtitle = subtitle != nullptr && subtitle[0] != '\0';
  const int subtitleWidth = hasSubtitle ? renderer.getTextWidth(SMALL_FONT_ID, subtitle) : 0;
  // The subtitle sits at the right end of the title row.
  if (hasSubtitle) maxWidth -= subtitleWidth + metrics.headerSidePadding;

  char text[pageTitleBytes];
  int fontId = 0;
  // Touch boards keep the stock header: its back button belongs to the title.
  if (!gpio.hasTouch() && rect.height >= metrics.headerHeight && lowercaseAscii(title, text, sizeof(text))) {
    const auto& fonts = renderer.getFontMap();
    for (const int candidate : pageTitleFonts) {
      if (fonts.count(candidate) != 0 && renderer.getTextWidth(candidate, text) <= maxWidth) {
        fontId = candidate;
        break;
      }
    }
  }
  if (fontId == 0) {
    BaseTheme::drawHeader(renderer, rect, title, subtitle, backButton);
    return;
  }

  // The status strip comes from the stock header. Without a title it draws no subtitle, so both are drawn here.
  BaseTheme::drawHeader(renderer, rect, nullptr, nullptr, backButton);
  const int baseline = rect.y + rect.height - pageTitleBaselineUp;
  renderer.drawText(fontId, rect.x + left + metrics.headerSidePadding, baseline - renderer.getFontAscenderSize(fontId),
                    text, true);
  if (hasSubtitle) {
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - right - metrics.headerSidePadding - subtitleWidth,
                      baseline - renderer.getFontAscenderSize(SMALL_FONT_ID), subtitle, true);
  }
}

void MetroTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                 const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  constexpr int barHeight = MetroMetrics::values.buttonHintsHeight;
  const int barY = renderer.getScreenHeight() - barHeight;
  constexpr int buttonWidth = 106;
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = pageWidth >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  // In a relative gray pass a black fill writes zero bits, which keeps the bar
  // and labels from the monochrome pass; the labels must not be drawn again.
  renderer.fillRect(0, barY, pageWidth, barHeight, true);
  const bool grayscale = renderer.getRenderMode() != GfxRenderer::BW && !renderer.grayPlanesAreAbsolute();
  if (!grayscale) {
    for (int i = 0; i < 4; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        drawWhiteHintLabel(renderer, labels[i], buttonPositions[i], buttonWidth, barY, barHeight);
      }
    }
  }

  renderer.setOrientation(origOrientation);
}

Rect MetroTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const ThemeMetrics& metrics = MetroMetrics::values;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  // Scale y position proportionally to screen height
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, popupFontFamily);
  const int w = textWidth + metrics.popupMarginX * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + metrics.popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  // The white frame keeps the block apart from dark content behind it.
  renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, false);
  renderer.fillRect(x, y, w, h, true);

  const int textY = y + metrics.popupMarginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, x + metrics.popupMarginX, textY, message, false, popupFontFamily);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}
