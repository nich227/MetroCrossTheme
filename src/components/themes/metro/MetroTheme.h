#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Metro theme metrics: flat, square corners, solid black selection.
namespace MetroMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 10,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 84,
                                 .verticalSpacing = 12,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 48,
                                 .listWithSubtitleRowHeight = 68,
                                 .listRowGap = 0,
                                 .listRowRadius = 0,
                                 // No inset of its own: the selection bar runs between the
                                 // board's bezel insets, with the scroll track on its right, and
                                 // the row text lines up with the header title.
                                 .listInset = 0,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,  // invert fill
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = false,
                                 .headerSidePadding = 20,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 0,  // left
                                 .headerBatterySide = 0,
                                 .headerShowsClock = true,
                                 .headerClockCentered = false,
                                 // The fallback home is BaseTheme::drawButtonMenu, which lays its
                                 // rows out from BaseMetrics; the touch grid reads these two.
                                 .menuRowHeight = BaseMetrics::values.menuRowHeight,
                                 .menuSpacing = BaseMetrics::values.menuSpacing,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 48,
                                 .coverGridTabBarHeight = 72,
                                 .tabPillFullSlot = false,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 // Status band above the home tiles: the 20px battery strip from
                                 // topPadding, plus a 10px gap.
                                 .homeTopPadding = 40,
                                 // The fallback home is BaseTheme::drawRecentBookCover, which reads
                                 // BaseMetrics for the cover thumb size.
                                 .homeCoverHeight = BaseMetrics::values.homeCoverHeight,
                                 .homeCoverTileHeight = BaseMetrics::values.homeCoverTileHeight,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = BaseMetrics::values.homeMenuTopOffset,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 56,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = 0,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 20,
                                 .popupMarginY = 16,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = false,
                                 // The popup is a black block (see MetroTheme::drawPopup), so its
                                 // text and progress fill are white.
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = true,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 20,
                                 .optionPopupSelectionVPadding = 12,
                                 .optionPopupDialogSideMargin = 20,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 2,
                                 .textFieldCursorThickness = 4,
                                 .textFieldLineEndOffset = 0,
                                 .controlRadius = 0,
                                 .sheetRadius = 0,
                                 .capsuleRadius = 0};
}  // namespace MetroMetrics

class MetroTheme : public BaseTheme {
 public:
  // One black bar across the bottom edge with a white label over each front button.
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  // Black block with white text; fillPopupProgress() then draws a white bar on it.
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  bool showsFileIcons() const override { return true; }
  // Page title in the Metro way: large, light and lowercase, also when the
  // title is user content (a folder, a book, a headword). A title the light
  // font cannot show, or that does not fit, keeps the stock header.
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool backButton = true) const override;
};
