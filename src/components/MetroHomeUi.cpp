#include "MetroHomeUi.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "components/UITheme.h"
#include "components/icons/metroIcons.h"
#include "fontIds.h"
#include "metroFontIds.h"
#include "util/BookProgress.h"

namespace {

// Open and return animation. The tiles turn on the left edge of the grid, one
// column after the other, so each frame moves a part of the grid a short way.
// Each frame costs one full panel refresh.
constexpr int ENTER_FRAMES = 2;  // turned frames; the normal tiles follow
constexpr int EXIT_FRAMES = 3;
// A column starts later by this share of its distance from the hinge.
constexpr int STAGGER_PERMILLE = 800;
// The whole move runs this much longer than one tile's turn, so the last column can finish.
constexpr int CLOCK_STRETCH_PERMILLE = 1600;
// A turned tile narrower than this share of its width is not drawn.
constexpr int MIN_TURNED_PERMILLE = 60;
constexpr int MIN_TURNED_WIDTH = 2;
// The hinge side of a tile seen edge on is shorter by this share of the tile height, at each end.
constexpr int TAPER_PERCENT = 10;

// Live tile flip: the tile turns on its horizontal middle line. One entry per
// frame without content: the height of the tile as seen, in percent, and
// whether the back is already coming up. 0 is the tile seen edge on. The frame
// after the last entry shows the new face. Each frame costs one panel refresh.
struct FlipBand {
  uint8_t heightPercent;
  bool opening;
};
constexpr FlipBand FLIP_BANDS[] = {{70, false}, {35, false}, {0, false}, {50, true}};
constexpr int FLIP_BAND_FRAMES = sizeof(FLIP_BANDS) / sizeof(FLIP_BANDS[0]);
// The far edge of a turned tile is narrower by this share of the tile width, at each end.
constexpr int FLIP_TAPER_PERCENT = 8;

// Grid: square cells, small tiles 1x1, medium 2x2, wide 4x2. The long side of
// the area gets six cells and the short side four.
constexpr int LONG_SIDE_CELLS = 6;
constexpr int SHORT_SIDE_CELLS = 4;
constexpr int GUTTER = 12;
constexpr int SIDE_MARGIN = 12;
constexpr int MIN_CELL = 16;

// Focus ring: drawn outside the selected tile, inside the gutter.
constexpr int RING_WIDTH = 4;
constexpr int RING_GAP = 3;
constexpr int RING_OUTSET = RING_WIDTH + RING_GAP;
static_assert(RING_OUTSET < GUTTER && RING_OUTSET <= SIDE_MARGIN, "the focus ring must fit between tiles");

constexpr int BORDER = 2;       // outline of a white tile, and of the progress bar
constexpr int INSET = 12;       // text inset on medium and wide tiles
constexpr int SMALL_INSET = 6;  // text inset on small tiles
constexpr int HERO_INSET = 16;  // side inset of the reading-progress face
constexpr int ICON_TITLE_GAP = 4;
constexpr int VALUE_GAP = 6;  // big number to the caption under it
constexpr int STACK_GAP = 8;  // between the two parts of a small value tile
constexpr int AUTHOR_GAP = 2;
constexpr int RECENT_LINE_GAP = 10;
constexpr int BAR_HEIGHT = 10;
constexpr int BAR_GAP = 14;
constexpr int CAPTION_GAP = 8;
// Selection mark in the top-right corner of the selected tile.
constexpr int CORNER_SIZE = 32;
constexpr int CHECK_WIDTH = 3;
// A plate under text on a patterned tile reaches this far past the text.
constexpr int PLATE_PAD_X = 5;
constexpr int PLATE_PAD_Y = 4;
// A tile takes the 96 px icon when it leaves this much room on each side.
constexpr int LARGE_ICON_PAD = 16;

// Battery glyph: a body outline with a terminal on its right side and a level bar inside.
constexpr int BATTERY_BODY_WIDTH = 40;
constexpr int BATTERY_BODY_HEIGHT = 22;
constexpr int BATTERY_OUTLINE = 3;
constexpr int BATTERY_TERMINAL_WIDTH = 4;
constexpr int BATTERY_TERMINAL_HEIGHT = 10;
constexpr int BATTERY_LEVEL_INSET = BATTERY_OUTLINE + 2;
constexpr int BATTERY_WIDTH = BATTERY_BODY_WIDTH + BATTERY_TERMINAL_WIDTH;

// Tried in this order: the theme's number fonts, then the reader fonts, each
// group largest first. Only the 14 pt reader font is in every build.
constexpr int BIG_FONTS[] = {METRO_NUMBER_LARGE_FONT_ID, METRO_NUMBER_FONT_ID, NOTOSANS_18_FONT_ID, NOTOSANS_16_FONT_ID,
                             NOTOSANS_14_FONT_ID};

// A tile tone is the number of pixels in 16 that carry ink: 0 is white paper
// with an outline, 16 is solid black, and the steps between are ordered dot
// patterns, the way a one-bit screen shows gray.
constexpr uint8_t INK_FULL = 16;
// A pattern this close to white or black is sparse enough for text with a thin
// outline. On the tones between, text gets a solid plate.
constexpr uint8_t INK_SPARSE = 2;

struct TileStyle {
  uint8_t ink;
  bool whiteInk;
};

// Tone table, one row per MetroHomeUi::TileId in declaration order. The tiles
// that can share a screen all have different tones.
constexpr TileStyle TILE_STYLES[] = {
    {16, true},  // Continue
    {4, false},  // Files
    {0, false},  // Library
    {10, true},  // SlotItem
    {14, true},  // Transfer
    {8, false},  // Settings
    {2, false},  // Battery
    {12, true},  // Clock
    {6, false},  // Date
    {12, true},  // BookCount: takes the place of the clock tile
    {6, false},  // Progress: takes the place of the date tile
};

// 4x4 ordered dither: a pixel carries ink when its entry is below the tone.
// Tied to screen coordinates, so two fills that touch line up.
constexpr uint8_t DITHER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

bool needsPlate(const TileStyle style) { return style.ink > INK_SPARSE && style.ink < INK_FULL - INK_SPARSE; }

enum class IconId : uint8_t { Folder, Library, Book, Settings, Transfer, Plugins, Catalog, Clock, Calendar };

struct IconSet {
  const freeink::Icon* small;
  const freeink::Icon* large;  // null when no tile shows this icon large
};

// One row per IconId. Only the sizes listed here are linked into the firmware.
constexpr IconSet ICONS[] = {
    {&icon_metro_folder_48, &icon_metro_folder_96},
    {&icon_metro_library_48, &icon_metro_library_96},
    {&icon_metro_book_48, &icon_metro_book_96},
    {&icon_metro_settings_48, nullptr},
    {&icon_metro_transfer_48, &icon_metro_transfer_96},
    {&icon_metro_plugins_48, &icon_metro_plugins_96},
    {&icon_metro_catalog_48, &icon_metro_catalog_96},
    {&icon_metro_clock_48, nullptr},
    {&icon_metro_calendar_48, nullptr},
};

// The large icon when the box leaves room around it, else the small one. A box
// smaller than the small icon still gets it; the tile clips the overflow.
const freeink::Icon& pickIcon(const IconId id, const int box) {
  const IconSet& set = ICONS[static_cast<size_t>(id)];
  return set.large != nullptr && box >= set.large->w + 2 * LARGE_ICON_PAD ? *set.large : *set.small;
}

// Limits all drawing to a rectangle while the object lives.
class ClipScope {
 public:
  ClipScope(const GfxRenderer& renderer, const int x, const int y, const int width, const int height)
      : renderer(renderer), saved(renderer.getClipRect()) {
    const int left = std::max(x, saved[0]);
    const int top = std::max(y, saved[1]);
    const int right = std::min(x + width, saved[0] + saved[2]);
    const int bottom = std::min(y + height, saved[1] + saved[3]);
    renderer.setClipRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
  }
  ~ClipScope() { renderer.setClipRect(saved[0], saved[1], saved[2], saved[3]); }
  ClipScope(const ClipScope&) = delete;
  ClipScope& operator=(const ClipScope&) = delete;

 private:
  const GfxRenderer& renderer;
  const std::array<int, 4> saved;
};

void fillShade(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
               const uint8_t ink) {
  if (width <= 0 || height <= 0) return;
  if (ink == 0) {
    renderer.fillRect(x, y, width, height, true);
    renderer.fillRect(x + BORDER, y + BORDER, width - 2 * BORDER, height - 2 * BORDER, false);
    return;
  }
  // Start from the nearer solid, then set the pixels that differ from it.
  const bool mostlyInk = ink > INK_FULL / 2;
  renderer.fillRect(x, y, width, height, mostlyInk);
  if (ink >= INK_FULL) return;
  // drawPixel reports every pixel outside the panel, so the loops stay inside it.
  const auto clip = renderer.getClipRect();
  const int left = std::max({x, 0, clip[0]});
  const int top = std::max({y, 0, clip[1]});
  const int right = std::min({x + width, renderer.getScreenWidth(), clip[0] + clip[2]});
  const int bottom = std::min({y + height, renderer.getScreenHeight(), clip[1] + clip[3]});
  for (int py = top; py < bottom; ++py) {
    const uint8_t* row = DITHER[py & 3];
    for (int px = left; px < right; ++px) {
      const bool inked = row[px & 3] < ink;
      if (inked != mostlyInk) renderer.drawPixel(px, py, inked);
    }
  }
}

// Draws the ink of an icon in black or white and leaves the other pixels alone.
void blitIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const bool black) {
  // drawPixel reports every pixel outside the panel, so the loops stay inside it.
  const auto clip = renderer.getClipRect();
  const int firstCol = std::max({0, -x, clip[0] - x});
  const int firstRow = std::max({0, -y, clip[1] - y});
  const int endCol = std::min({static_cast<int>(icon.w), renderer.getScreenWidth() - x, clip[0] + clip[2] - x});
  const int endRow = std::min({static_cast<int>(icon.h), renderer.getScreenHeight() - y, clip[1] + clip[3] - y});
  const int rowBytes = (icon.w + 7) / 8;
  for (int row = firstRow; row < endRow; ++row) {
    const uint8_t* bits = icon.bits + row * rowBytes;
    for (int col = firstCol; col < endCol; ++col) {
      const uint8_t byte = bits[col >> 3];
      if (byte == 0xFF) {
        col |= 7;  // no ink in this byte
        continue;
      }
      if (((byte >> (7 - (col & 7))) & 1) == 0) renderer.drawPixel(x + col, y + row, black);
    }
  }
}

// Pattern dots next to an icon fray its outline. A halo in the other ink keeps
// them away: two pixels on the dense middle patterns, one on the others.
int haloWidth(const uint8_t ink) {
  if (ink == 0 || ink >= INK_FULL) return 0;
  return ink >= 6 && ink <= 10 ? 2 : 1;
}

void drawTileIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y,
                  const TileStyle style) {
  const int halo = haloWidth(style.ink);
  for (int reach = 1; reach <= halo; ++reach) {
    for (int dy = -reach; dy <= reach; dy += reach) {
      for (int dx = -reach; dx <= reach; dx += reach) {
        if (dx != 0 || dy != 0) blitIcon(renderer, icon, x + dx, y + dy, style.whiteInk);
      }
    }
  }
  blitIcon(renderer, icon, x, y, !style.whiteInk);
}

// Height of a digit above the baseline. Numbers are placed by their ink, not
// by the line box, which has much more room above the digits than below.
int digitRise(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style) {
  const auto& fonts = renderer.getFontMap();
  const auto it = fonts.find(fontId);
  if (it != fonts.end()) {
    if (const EpdGlyph* glyph = it->second.getGlyph('0', style)) return glyph->top;
  }
  return renderer.getFontAscenderSize(fontId) * 7 / 10;
}

// A solid block in the tile's paper color. Text on a patterned tile stands on it.
void drawPlate(const GfxRenderer& renderer, const TileStyle style, const int x, const int y, const int width,
               const int height) {
  renderer.fillRect(x - PLATE_PAD_X, y - PLATE_PAD_Y, width + 2 * PLATE_PAD_X, height + 2 * PLATE_PAD_Y,
                    style.whiteInk);
}

// Text in the tile's ink. A dot pattern frays the edges of glyphs: on a sparse
// pattern a one pixel outline in the other ink goes down first, and on a dense
// one a plate, unless the caller has put a plate under the text already.
void drawTileText(const GfxRenderer& renderer, const TileStyle style, const int fontId, const int x, const int y,
                  const char* text, const EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR,
                  const bool plated = false) {
  const bool black = !style.whiteInk;
  if (needsPlate(style)) {
    if (!plated) {
      // From the top of the capitals to the bottom of the descenders.
      const int rise = digitRise(renderer, fontId, fontStyle);
      drawPlate(renderer, style, x, y + renderer.getFontAscenderSize(fontId) - rise,
                renderer.getTextWidth(fontId, text, fontStyle), rise + rise * 3 / 10);
    }
  } else if (style.ink != 0 && style.ink < INK_FULL) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx != 0 || dy != 0) renderer.drawText(fontId, x + dx, y + dy, text, !black, fontStyle);
      }
    }
  }
  renderer.drawText(fontId, x, y, text, black, fontStyle);
}

// Draws text with the top of its digits at `top`.
void drawNumber(const GfxRenderer& renderer, const TileStyle style, const int fontId, const int x, const int top,
                const char* text, const EpdFontFamily::Style fontStyle, const bool plated = false) {
  const int rise = digitRise(renderer, fontId, fontStyle);
  if (needsPlate(style) && !plated) {
    drawPlate(renderer, style, x, top, renderer.getTextWidth(fontId, text, fontStyle), rise);
  }
  drawTileText(renderer, style, fontId, x, top + rise - renderer.getFontAscenderSize(fontId), text, fontStyle, true);
}

// Largest number font that is loaded and fits. For printable ASCII only.
int pickBigFont(const GfxRenderer& renderer, const char* text, const int maxWidth) {
  const auto& fonts = renderer.getFontMap();
  for (const int fontId : BIG_FONTS) {
    if (fonts.count(fontId) != 0 && renderer.getTextWidth(fontId, text, EpdFontFamily::BOLD) <= maxWidth) {
      return fontId;
    }
  }
  return UI_12_FONT_ID;
}

int labelFont(const GfxRenderer& renderer, const char* text, const int maxWidth) {
  return renderer.getTextWidth(UI_10_FONT_ID, text) <= maxWidth ? UI_10_FONT_ID : SMALL_FONT_ID;
}

// A label in the font labelFont() picked. One that is too long even for the small font is shortened.
void drawLabel(const GfxRenderer& renderer, const TileStyle style, const int fontId, const int x, const int y,
               const char* text, const int maxWidth) {
  if (renderer.getTextWidth(fontId, text) <= maxWidth) {
    drawTileText(renderer, style, fontId, x, y, text);
    return;
  }
  // Rare: a long translation. The shortened copy lives for this call only.
  drawTileText(renderer, style, fontId, x, y,
               renderer.truncatedText(fontId, text, maxWidth, EpdFontFamily::REGULAR).c_str());
}

// Empty space on the left of the first glyph. Subtract it to line the ink of
// two fonts up on one edge. Zero for text that does not start with ASCII.
int inkLeft(const GfxRenderer& renderer, const int fontId, const char* text, const EpdFontFamily::Style style) {
  const auto first = static_cast<unsigned char>(text[0]);
  if (first < 0x21 || first > 0x7E) return 0;
  const auto& fonts = renderer.getFontMap();
  const auto it = fonts.find(fontId);
  if (it == fonts.end()) return 0;
  const EpdGlyph* glyph = it->second.getGlyph(first, style);
  return glyph != nullptr ? glyph->left : 0;
}

int centeredX(const GfxRenderer& renderer, const Rect rect, const int fontId, const char* text,
              const EpdFontFamily::Style style) {
  return rect.x + (rect.width - renderer.getTextWidth(fontId, text, style)) / 2;
}

// Icon in the middle, optional title at the bottom-left.
void drawIconFace(const GfxRenderer& renderer, const Rect rect, const TileStyle style, const IconId iconId,
                  const char* title) {
  int titleTop = rect.y + rect.height;
  if (title != nullptr) {
    const int fontId = labelFont(renderer, title, rect.width - 2 * INSET);
    const int top = titleTop - INSET - renderer.getLineHeight(fontId);
    // A tile too short for both keeps the icon, like a small tile.
    if (top - rect.y >= ICONS[static_cast<size_t>(iconId)].small->h) {
      titleTop = top;
      drawLabel(renderer, style, fontId, rect.x + INSET, titleTop, title, rect.width - 2 * INSET);
    }
  }
  const freeink::Icon& icon = pickIcon(iconId, std::min(rect.width, titleTop - rect.y));
  int iconY = rect.y + (rect.height - icon.h) / 2;
  // Centered on the whole tile, unless the icon would then touch the title.
  if (iconY + icon.h + ICON_TITLE_GAP > titleTop) iconY = rect.y + (titleTop - rect.y - icon.h) / 2;
  drawTileIcon(renderer, icon, rect.x + (rect.width - icon.w) / 2, iconY, style);
}

// Big number with a caption under it, left aligned; the optional title stays at the bottom-left.
void drawValueFace(const GfxRenderer& renderer, const Rect rect, const TileStyle style, const char* value,
                   const char* caption, const char* title) {
  const int x = rect.x + INSET;
  const int textWidth = rect.width - 2 * INSET;
  int bottom = rect.y + rect.height - INSET;
  if (title != nullptr) {
    const int fontId = labelFont(renderer, title, textWidth);
    bottom -= renderer.getLineHeight(fontId);
    drawLabel(renderer, style, fontId, x, bottom, title, textWidth);
  }
  const int valueFont = pickBigFont(renderer, value, textWidth);
  const int captionFont = labelFont(renderer, caption, textWidth);
  const int rise = digitRise(renderer, valueFont, EpdFontFamily::BOLD);
  const int blockHeight = rise + VALUE_GAP + renderer.getLineHeight(captionFont);
  const int top = rect.y + std::max(INSET, (bottom - rect.y - blockHeight) / 2);
  drawNumber(renderer, style, valueFont, x - inkLeft(renderer, valueFont, value, EpdFontFamily::BOLD), top, value,
             EpdFontFamily::BOLD);
  drawLabel(renderer, style, captionFont, x, top + rise + VALUE_GAP, caption, textWidth);
}

// Small tile: a short value in big digits, with an optional short second line, both centered.
void drawStackedValue(const GfxRenderer& renderer, const Rect rect, const TileStyle style, const char* value,
                      const char* caption) {
  const bool hasCaption = caption != nullptr && caption[0] != '\0';
  // The wider inset keeps a four-character value clear of the tile edges.
  const int fontId = pickBigFont(renderer, value, rect.width - 2 * INSET);
  const int rise = digitRise(renderer, fontId, EpdFontFamily::BOLD);
  const int captionFont = hasCaption ? labelFont(renderer, caption, rect.width - 2 * SMALL_INSET) : UI_10_FONT_ID;
  // Rare: a translation too long even for the small font. The shortened copy lives for this call only.
  std::string shortened;
  if (hasCaption && renderer.getTextWidth(captionFont, caption) > rect.width - 2 * SMALL_INSET) {
    shortened = renderer.truncatedText(captionFont, caption, rect.width - 2 * SMALL_INSET, EpdFontFamily::REGULAR);
    caption = shortened.c_str();
  }
  const int captionRise = hasCaption ? digitRise(renderer, captionFont, EpdFontFamily::REGULAR) : 0;
  const int stackHeight = rise + (hasCaption ? STACK_GAP + captionRise : 0);
  const int top = rect.y + (rect.height - stackHeight) / 2;
  const int valueX = centeredX(renderer, rect, fontId, value, EpdFontFamily::BOLD);
  const int captionX = hasCaption ? centeredX(renderer, rect, captionFont, caption, EpdFontFamily::REGULAR) : valueX;
  if (needsPlate(style)) {
    // One plate under both lines, as wide as the wider one.
    const int left = std::min(valueX, captionX);
    drawPlate(renderer, style, left, top, rect.x + rect.width - left - (left - rect.x), stackHeight);
  }
  drawNumber(renderer, style, fontId, valueX, top, value, EpdFontFamily::BOLD, true);
  if (hasCaption) {
    drawNumber(renderer, style, captionFont, captionX, top + rise + STACK_GAP, caption, EpdFontFamily::REGULAR, true);
  }
}

// A frame of the given thickness whose outer edge is `outset` pixels outside the rectangle.
void drawFrame(const GfxRenderer& renderer, const Rect rect, const int outset, const int thickness, const bool black) {
  const int x = rect.x - outset;
  const int y = rect.y - outset;
  const int width = rect.width + 2 * outset;
  const int height = rect.height + 2 * outset;
  renderer.fillRect(x, y, width, thickness, black);
  renderer.fillRect(x, y + height - thickness, width, thickness, black);
  renderer.fillRect(x, y, thickness, height, black);
  renderer.fillRect(x + width - thickness, y, thickness, height, black);
}

void drawBattery(const GfxRenderer& renderer, const Rect rect, const TileStyle style) {
  const unsigned percent = std::min<unsigned>(powerManager.getBatteryPercentage(), 100);
  char text[8];
  snprintf(text, sizeof(text), "%u%%", percent);

  const int fontId = pickBigFont(renderer, text, rect.width - 2 * SMALL_INSET);
  const int rise = digitRise(renderer, fontId, EpdFontFamily::BOLD);
  const int stackHeight = BATTERY_BODY_HEIGHT + STACK_GAP + rise;
  if (rect.width < BATTERY_WIDTH + 2 * SMALL_INSET || rect.height < stackHeight + 2 * SMALL_INSET) {
    drawStackedValue(renderer, rect, style, text, nullptr);
    return;
  }
  const bool black = !style.whiteInk;
  const int top = rect.y + (rect.height - stackHeight) / 2;
  const int bodyX = rect.x + (rect.width - BATTERY_WIDTH) / 2;
  drawFrame(renderer, Rect{bodyX, top, BATTERY_BODY_WIDTH, BATTERY_BODY_HEIGHT}, 0, BATTERY_OUTLINE, black);
  renderer.fillRect(bodyX + BATTERY_BODY_WIDTH, top + (BATTERY_BODY_HEIGHT - BATTERY_TERMINAL_HEIGHT) / 2,
                    BATTERY_TERMINAL_WIDTH, BATTERY_TERMINAL_HEIGHT, black);
  const int levelWidth = (BATTERY_BODY_WIDTH - 2 * BATTERY_LEVEL_INSET) * static_cast<int>(percent) / 100;
  if (levelWidth > 0) {
    renderer.fillRect(bodyX + BATTERY_LEVEL_INSET, top + BATTERY_LEVEL_INSET, levelWidth,
                      BATTERY_BODY_HEIGHT - 2 * BATTERY_LEVEL_INSET, black);
  }
  drawNumber(renderer, style, fontId, centeredX(renderer, rect, fontId, text, EpdFontFamily::BOLD),
             top + BATTERY_BODY_HEIGHT + STACK_GAP, text, EpdFontFamily::BOLD);
}

void drawClock(const GfxRenderer& renderer, const Rect rect, const TileStyle style) {
  char text[12];
  if (!halClock.formatTime(text, sizeof(text), SETTINGS.clockFormat == 1)) {
    drawIconFace(renderer, rect, style, IconId::Clock, nullptr);
    return;
  }
  // 12-hour time reads "H:MM AM"; the AM/PM part goes on its own line.
  char* suffix = strchr(text, ' ');
  if (suffix != nullptr) *suffix++ = '\0';
  drawStackedValue(renderer, rect, style, text, suffix);
}

// Indexed by tm_wday.
constexpr StrId WEEKDAYS[] = {StrId::STR_METRO_SUN, StrId::STR_METRO_MON, StrId::STR_METRO_TUE, StrId::STR_METRO_WED,
                              StrId::STR_METRO_THU, StrId::STR_METRO_FRI, StrId::STR_METRO_SAT};

void drawDate(const GfxRenderer& renderer, const Rect rect, const TileStyle style) {
  struct tm now{};
  if (!halClock.localTime(now)) {
    drawIconFace(renderer, rect, style, IconId::Calendar, nullptr);
    return;
  }
  char day[8];
  snprintf(day, sizeof(day), "%d", now.tm_mday);
  const char* weekday = I18n::getInstance().get(WEEKDAYS[std::clamp(now.tm_wday, 0, 6)]);

  // Short weekday and big day number on one baseline. A weekday too long for that goes under the number.
  const int fontId = pickBigFont(renderer, day, rect.width - 2 * SMALL_INSET);
  const int dayWidth = renderer.getTextWidth(fontId, day, EpdFontFamily::BOLD);
  const int weekdayWidth = renderer.getTextWidth(UI_10_FONT_ID, weekday);
  const int pairWidth = weekdayWidth + CAPTION_GAP + dayWidth;
  if (pairWidth > rect.width - 2 * SMALL_INSET) {
    drawStackedValue(renderer, rect, style, day, weekday);
    return;
  }
  const int rise = digitRise(renderer, fontId, EpdFontFamily::BOLD);
  const int top = rect.y + (rect.height - rise) / 2;
  const int x = rect.x + (rect.width - pairWidth) / 2;
  if (needsPlate(style)) drawPlate(renderer, style, x, top, pairWidth, rise);
  drawTileText(renderer, style, UI_10_FONT_ID, x, top + rise - renderer.getFontAscenderSize(UI_10_FONT_ID), weekday,
               EpdFontFamily::REGULAR, true);
  drawNumber(renderer, style, fontId, x + weekdayWidth + CAPTION_GAP, top, day, EpdFontFamily::BOLD, true);
}

void drawFocusRing(const GfxRenderer& renderer, const Rect rect) {
  // White first, so the gap between tile and ring is clear whatever was drawn there.
  drawFrame(renderer, rect, RING_OUTSET, RING_OUTSET, false);
  drawFrame(renderer, rect, RING_OUTSET, RING_WIDTH, true);
}

// The corner mark of a selected Windows 8 tile: a triangle in the tile's ink
// with a check in the tile's shade. It shows the selection on a black tile,
// where the ring alone is a thin white line.
void drawSelectionCorner(const GfxRenderer& renderer, const Rect rect, const TileStyle style) {
  const int size = std::min({CORNER_SIZE, rect.width / 2, rect.height / 2});
  const bool black = !style.whiteInk;
  const int right = rect.x + rect.width;
  for (int row = 0; row < size; ++row) renderer.fillRect(right - size + row, rect.y + row, size - row, 1, black);
  // Check: a short stroke down, then a long stroke up to the right.
  const int startX = right - size * 15 / 32;
  const int startY = rect.y + size * 7 / 32;
  const int turnX = right - size * 12 / 32;
  const int turnY = rect.y + size * 10 / 32;
  const int endX = right - size * 5 / 32;
  const int endY = rect.y + size * 3 / 32;
  renderer.drawLine(startX, startY, turnX, turnY, CHECK_WIDTH, !black);
  renderer.drawLine(turnX, turnY, endX, endY, CHECK_WIDTH, !black);
}

// Copies as much of `text` as fits, never cutting a UTF-8 sequence.
void copyUtf8(char* dst, const size_t capacity, const char* text) {
  size_t length = strlen(text);
  if (length >= capacity) {
    length = capacity - 1;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
  }
  memcpy(dst, text, length);
  dst[length] = '\0';
}

void cacheTruncated(const GfxRenderer& renderer, char* dst, const size_t capacity, const int fontId, const char* text,
                    const int maxWidth, const EpdFontFamily::Style style) {
  copyUtf8(dst, capacity, renderer.truncatedText(fontId, text, maxWidth, style).c_str());
}

// The stored title, or the file name when the book has none.
const char* displayTitle(const RecentBook& book) {
  if (!book.title.empty()) return book.title.c_str();
  const size_t slash = book.path.find_last_of('/');
  return book.path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

}  // namespace

MetroHomeUi::MetroHomeUi(GfxRenderer& renderer) : renderer(renderer) {
  static_assert(sizeof(TILE_STYLES) / sizeof(TILE_STYLES[0]) == static_cast<size_t>(TileId::Progress) + 1,
                "TILE_STYLES needs one row per TileId");
  static_assert(sizeof(ICONS) / sizeof(ICONS[0]) == static_cast<size_t>(IconId::Calendar) + 1,
                "ICONS needs one row per IconId");
}

void MetroHomeUi::begin(const std::vector<RecentBook>& books, const Slot slotItem, const Rect area) {
  slot = slotItem;
  hasBook = !books.empty();
  coverBroken = false;
  flipStep = 0;
  flipTile = -1;
  flipCursor = -1;
  transition = Transition::None;
  transitionFrame = 0;
  exitSelection = -1;
  progress = -1;
  loadLibraryCount();
  opdsCount = slot == Slot::OPDS ? static_cast<uint8_t>(std::min<size_t>(OPDS_STORE.getCount(), UINT8_MAX)) : 0;

  layout(area, static_cast<int>(books.size()));

  bookPath.clear();
  coverThumbPath.clear();
  if (hasBook) {
    bookPath = books.front().path;
    if (!books.front().coverBmpPath.empty()) {
      coverThumbPath = UITheme::getCoverThumbPath(books.front().coverBmpPath, coverThumbHeight());
    }
  }
  cacheTexts(books);
}

void MetroHomeUi::layout(const Rect area, const int menuBase) {
  tileCount = 0;
  coverWidth = 0;
  gridLeft = 0;
  gridWidth = 0;

  const int left = std::max(area.x, 0);
  const int top = std::max(area.y, 0);
  const int width = std::min(area.x + area.width, renderer.getScreenWidth()) - left;
  const int height = std::min(area.y + area.height, renderer.getScreenHeight()) - top;
  const bool portrait = width <= height;
  const int cols = portrait ? SHORT_SIDE_CELLS : LONG_SIDE_CELLS;
  const int rows = portrait ? LONG_SIDE_CELLS : SHORT_SIDE_CELLS;
  // The side margins hold the focus ring of the outer columns. Below the last
  // row the ring needs room of its own, or it would leave the area.
  const int cell = std::min((width - 2 * SIDE_MARGIN - (cols - 1) * GUTTER) / cols,
                            (height - RING_OUTSET - (rows - 1) * GUTTER) / rows);
  if (cell < MIN_CELL) {
    LOG_ERR("METRO", "Tile area %dx%d is too small", width, height);
    return;
  }
  const int pitch = cell + GUTTER;
  const int originX = left + (width - (cols * pitch - GUTTER)) / 2;
  // Room left over goes above and below the grid in equal parts.
  const int originY = top + std::max(0, (height - RING_OUTSET - (rows * pitch - GUTTER)) / 2);
  gridLeft = static_cast<int16_t>(originX);
  gridWidth = static_cast<int16_t>(cols * pitch - GUTTER);

  // One bit per cell. Each tile takes the first free place in reading order.
  uint8_t occupied[LONG_SIDE_CELLS] = {};
  const auto place = [&](const TileId id, const int spanX, const int spanY, const int action) {
    if (tileCount >= MAX_TILES) return false;
    for (int row = 0; row + spanY <= rows; ++row) {
      for (int col = 0; col + spanX <= cols; ++col) {
        const auto mask = static_cast<uint8_t>(((1u << spanX) - 1u) << col);
        bool isFree = true;
        for (int r = row; r < row + spanY; ++r) isFree = isFree && (occupied[r] & mask) == 0;
        if (!isFree) continue;
        for (int r = row; r < row + spanY; ++r) occupied[r] |= mask;
        Tile& tile = tiles[tileCount++];
        tile.x = static_cast<int16_t>(originX + col * pitch);
        tile.y = static_cast<int16_t>(originY + row * pitch);
        tile.width = static_cast<int16_t>(spanX * pitch - GUTTER);
        tile.height = static_cast<int16_t>(spanY * pitch - GUTTER);
        tile.id = id;
        tile.action = static_cast<int8_t>(action);
        tile.face = 0;
        return true;
      }
    }
    if (action >= 0) LOG_ERR("METRO", "No room for tile %d", static_cast<int>(id));
    return false;
  };

  const bool hasSlot = slot != Slot::NONE;
  const int transferAction = menuBase + 2 + (hasSlot ? 1 : 0);
  place(TileId::Continue, 4, 2, hasBook ? 0 : -1);
  place(TileId::Files, 2, 2, menuBase);
  place(TileId::Library, 2, 2, menuBase + 1);
  if (hasSlot) {
    place(TileId::SlotItem, 2, 2, menuBase + 2);
    place(TileId::Transfer, 1, 1, transferAction);
  } else {
    place(TileId::Transfer, 2, 2, transferAction);
  }
  place(TileId::Settings, 1, 1, transferAction + 1);
  place(TileId::Battery, 1, 1, -1);
  if (halClock.isAvailable()) {
    place(TileId::Clock, 1, 1, -1);
    place(TileId::Date, 1, 1, -1);
  }
  // Cells still free (a board without a clock) show library and reading
  // figures. Both tiles show an icon until they have a figure, so no cell stays empty.
  place(TileId::BookCount, 1, 1, -1);
  place(TileId::Progress, 1, 1, -1);

  if (const Tile* hero = findTile(TileId::Continue)) {
    coverWidth = static_cast<int16_t>(std::min(hero->height * 3 / 5, hero->width * 2 / 5));
  }
}

void MetroHomeUi::cacheTexts(const std::vector<RecentBook>& books) {
  titleLineCount = 0;
  recentCount = 0;
  author[0] = '\0';
  titleOneLine[0] = '\0';

  const Tile* hero = findTile(TileId::Continue);
  if (hasBook && hero != nullptr) {
    const RecentBook& book = books.front();
    const char* title = displayTitle(book);
    const int textWidth = hero->width - coverWidth - 2 * INSET;
    // Title lines that fit above the author line and the footer.
    const int room = hero->height - 2 * INSET - renderer.getLineHeight(SMALL_FONT_ID) -
                     renderer.getLineHeight(UI_10_FONT_ID) - AUTHOR_GAP;
    const int maxLines = std::clamp(room / std::max(1, renderer.getLineHeight(UI_12_FONT_ID)), 1, TITLE_LINES);
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, title, textWidth, maxLines, EpdFontFamily::BOLD);
    for (const auto& line : lines) {
      if (titleLineCount >= TITLE_LINES) break;
      copyUtf8(titleLines[titleLineCount++], LINE_BYTES, line.c_str());
    }
    cacheTruncated(renderer, author, LINE_BYTES, UI_10_FONT_ID, book.author.c_str(), textWidth, EpdFontFamily::REGULAR);
    cacheTruncated(renderer, titleOneLine, LINE_BYTES, UI_12_FONT_ID, title, hero->width - 2 * HERO_INSET,
                   EpdFontFamily::REGULAR);
  }

  const Tile* libraryTile = findTile(TileId::Library);
  const auto& recents = RECENT_BOOKS.getBooks();
  if (libraryTile == nullptr || recents.size() < 2) return;
  for (const RecentBook& book : recents) {
    if (recentCount >= RECENT_LINES) break;
    if (hasBook && book.path == bookPath) continue;
    const char* title = displayTitle(book);
    if (title[0] == '\0') continue;
    cacheTruncated(renderer, recentTitles[recentCount++], LINE_BYTES, UI_10_FONT_ID, title,
                   libraryTile->width - 2 * INSET, EpdFontFamily::REGULAR);
  }
}

const MetroHomeUi::Tile* MetroHomeUi::findTile(const TileId id) const {
  for (int i = 0; i < tileCount; ++i) {
    if (tiles[i].id == id) return &tiles[i];
  }
  return nullptr;
}

void MetroHomeUi::setSelection(const int index) { selected = index; }

int MetroHomeUi::tileAt(const int x, const int y) const {
  // Half the gutter belongs to each neighbour, so a tap between two tiles still hits one.
  constexpr int reach = GUTTER / 2;
  for (int i = 0; i < tileCount; ++i) {
    const Tile& tile = tiles[i];
    if (tile.action < 0) continue;
    if (x >= tile.x - reach && x < tile.x + tile.width + reach && y >= tile.y - reach &&
        y < tile.y + tile.height + reach) {
      return tile.action;
    }
  }
  return -1;
}

int MetroHomeUi::coverThumbHeight() const {
  const Tile* hero = findTile(TileId::Continue);
  return hero != nullptr ? hero->height : 2 * MIN_CELL + GUTTER;
}

void MetroHomeUi::loadLibraryCount() {
  libraryCount = -1;
  // The index object is too large for the task stack.
  auto index = makeUniqueNoThrow<library::LibraryIndexFile>();
  if (!index) {
    LOG_ERR("METRO", "OOM: library index (%u bytes)", static_cast<unsigned>(sizeof(library::LibraryIndexFile)));
    return;
  }
  // Opening reads the header only. A missing index is not rebuilt here.
  if (index->open(library::libraryIndexPath())) {
    libraryCount = index->bookCount();
  } else {
    LOG_DBG("METRO", "No library index, the book count stays hidden");
  }
}

void MetroHomeUi::loadLiveData() {
  if (!hasBook) return;
  const int percent = loadBookProgress(bookPath);
  progress = percent >= 0 && percent <= 100 ? static_cast<int8_t>(percent) : -1;
}

uint8_t MetroHomeUi::nextFace(const Tile& tile) const {
  bool available[MAX_FACES] = {true, false, false};
  switch (tile.id) {
    case TileId::Continue:
      available[1] = hasBook && progress >= 0;
      break;
    case TileId::Library:
      available[1] = libraryCount >= 0;
      available[2] = recentCount > 0;
      break;
    case TileId::SlotItem:
      available[1] = slot == Slot::OPDS && opdsCount > 0;
      break;
    default:
      break;
  }
  for (int step = 1; step < MAX_FACES; ++step) {
    const int face = (tile.face + step) % MAX_FACES;
    if (available[face]) return static_cast<uint8_t>(face);
  }
  return tile.face;
}

bool MetroHomeUi::startFlip() {
  finishFlip();
  for (int step = 1; step <= tileCount; ++step) {
    const int index = (flipCursor + step) % tileCount;
    if (nextFace(tiles[index]) == tiles[index].face) continue;
    flipTile = static_cast<int8_t>(index);
    flipCursor = flipTile;
    flipStep = 1;
    return true;
  }
  return false;
}

bool MetroHomeUi::flipActive() const { return flipStep != 0; }

void MetroHomeUi::finishFlip() {
  if (flipStep == 0) return;
  // The face changes behind the last band frame; do it now if that has not happened.
  if (flipStep <= FLIP_BAND_FRAMES) tiles[flipTile].face = nextFace(tiles[flipTile]);
  flipStep = 0;
  flipTile = -1;
}

bool MetroHomeUi::advanceFlip() {
  if (flipStep == 0) return false;
  if (flipStep <= FLIP_BAND_FRAMES) {
    if (flipStep == FLIP_BAND_FRAMES) tiles[flipTile].face = nextFace(tiles[flipTile]);
    ++flipStep;
    return true;
  }
  // The new face was displayed.
  flipStep = 0;
  flipTile = -1;
  return false;
}

void MetroHomeUi::startEnter() {
  if (tileCount == 0) return;
  transition = Transition::Enter;
  transitionFrame = 0;
  exitSelection = -1;
}

void MetroHomeUi::startExit(const int index) {
  if (tileCount == 0 || (transition == Transition::Exit && exitSelection == index)) return;
  finishFlip();
  selected = index;
  exitSelection = static_cast<int8_t>(index);
  transition = Transition::Exit;
  transitionFrame = 0;
}

bool MetroHomeUi::transitionActive() const { return transition != Transition::None; }

bool MetroHomeUi::exitActive() const { return transition == Transition::Exit; }

void MetroHomeUi::endTransition() {
  transition = Transition::None;
  transitionFrame = 0;
  exitSelection = -1;
}

bool MetroHomeUi::advanceTransition() {
  switch (transition) {
    case Transition::None:
      return false;
    case Transition::Enter:
      if (transitionFrame < ENTER_FRAMES) {
        ++transitionFrame;
        return true;
      }
      endTransition();
      return false;
    case Transition::Exit:
      if (transitionFrame + 1 < EXIT_FRAMES) {
        ++transitionFrame;
        return true;
      }
      return false;
  }
  return false;
}

int MetroHomeUi::turnedPermille(const Tile& tile) const {
  if (transition == Transition::None) return 1000;
  const bool entering = transition == Transition::Enter;
  const int frames = entering ? ENTER_FRAMES : EXIT_FRAMES;
  if (transitionFrame >= frames || gridWidth <= 0) return 1000;
  // Enter is exit played backwards. In every frame a tile is then at least as
  // wide as the tiles on its left, so no tile ends up on top of another.
  const int step = entering ? frames - 1 - transitionFrame : transitionFrame;
  // The clock of the whole move, then the tile's own: a column further from the hinge starts later.
  const int clock = (step + 1) * CLOCK_STRETCH_PERMILLE / (frames + 1);
  const int delay = (tile.x - gridLeft) * STAGGER_PERMILLE / gridWidth;
  return 1000 - std::clamp(clock - delay, 0, 1000);
}

void MetroHomeUi::render() {
  if (transition == Transition::None) {
    for (int i = 0; i < tileCount; ++i) drawTile(i);
    return;
  }
  // The moving tiles go down first: on their way to the left edge they pass
  // behind the tile that stays.
  int staying = -1;
  for (int i = 0; i < tileCount; ++i) {
    if (transition == Transition::Exit && tiles[i].action >= 0 && tiles[i].action == exitSelection) {
      staying = i;
      continue;
    }
    const int permille = turnedPermille(tiles[i]);
    if (permille >= 1000) {
      drawTile(i);
    } else if (permille >= MIN_TURNED_PERMILLE) {
      drawTurned(tiles[i], permille);
    }
  }
  if (staying >= 0) drawTile(staying);
}

void MetroHomeUi::drawTurned(const Tile& tile, const int permille) {
  const int x = gridLeft + (tile.x - gridLeft) * permille / 1000;
  const int width = std::max(MIN_TURNED_WIDTH, tile.width * permille / 1000);
  const uint8_t ink = TILE_STYLES[static_cast<size_t>(tile.id)].ink;
  fillShade(renderer, x, tile.y, width, tile.height, ink);

  // The hinge side is further from the eye, so it is shorter: cut a wedge off the top and the bottom.
  const int taper = tile.height * TAPER_PERCENT * (1000 - permille) / 100000;
  if (taper < 2) return;
  const int bottom = tile.y + tile.height - 1;
  for (int row = 0; row < taper; ++row) {
    const int cut = width * (taper - row) / taper;
    renderer.fillRect(x, tile.y + row, cut, 1, false);
    renderer.fillRect(x, bottom - row, cut, 1, false);
  }
  if (ink == 0) {
    // The wedges took the outline with them.
    const int right = x + width - 1;
    renderer.drawLine(x, tile.y + taper, right, tile.y, BORDER, true);
    renderer.drawLine(x, bottom - taper - BORDER + 1, right, bottom - BORDER + 1, BORDER, true);
  }
}

void MetroHomeUi::drawFlipBand(const Tile& tile, const int frame) {
  const FlipBand band = FLIP_BANDS[frame];
  const uint8_t ink = TILE_STYLES[static_cast<size_t>(tile.id)].ink;
  renderer.fillRect(tile.x, tile.y, tile.width, tile.height, false);
  if (band.heightPercent == 0) {
    // Edge on.
    const int edge = std::max(4, tile.height / 10);
    renderer.fillRect(tile.x, tile.y + (tile.height - edge) / 2, tile.width, edge, true);
    return;
  }
  const int height = tile.height * band.heightPercent / 100;
  const int top = tile.y + (tile.height - height) / 2;
  fillShade(renderer, tile.x, top, tile.width, height, ink);

  // The far edge is further from the eye, so it is narrower: the top edge
  // while the tile closes, the bottom edge while its back comes up.
  const int taper = tile.width * FLIP_TAPER_PERCENT * (100 - band.heightPercent) / 10000;
  if (taper < 2 || height < 2) return;
  const int farY = band.opening ? top + height - 1 : top;
  const int nearY = band.opening ? top : top + height - 1;
  const int right = tile.x + tile.width - 1;
  for (int row = 0; row < height; ++row) {
    const int fromNear = band.opening ? row : height - 1 - row;
    const int cut = taper * fromNear / (height - 1);
    if (cut <= 0) continue;
    renderer.fillRect(tile.x, top + row, cut, 1, false);
    renderer.fillRect(right - cut + 1, top + row, cut, 1, false);
  }
  if (ink == 0) {
    // The wedges took the side outlines with them.
    for (int i = 0; i < BORDER; ++i) {
      renderer.drawLine(tile.x + taper + i, farY, tile.x + i, nearY, true);
      renderer.drawLine(right - taper - i, farY, right - i, nearY, true);
    }
  }
}

void MetroHomeUi::drawTile(const int index) {
  const Tile& tile = tiles[index];
  const TileStyle style = TILE_STYLES[static_cast<size_t>(tile.id)];
  // 0 when the tile shows a face; else the number of the band frame, from 1.
  const int bandFrame = index == flipTile && flipStep <= FLIP_BAND_FRAMES ? flipStep : 0;

  if (bandFrame > 0) {
    drawFlipBand(tile, bandFrame - 1);
  } else {
    fillShade(renderer, tile.x, tile.y, tile.width, tile.height, style.ink);
    // Long text and oversized art stop at the tile edge instead of entering the gutter.
    const ClipScope clip(renderer, tile.x, tile.y, tile.width, tile.height);
    drawFace(tile);
  }

  if (tile.action >= 0 && tile.action == selected) {
    const Rect rect{tile.x, tile.y, tile.width, tile.height};
    drawFocusRing(renderer, rect);
    // A turning tile has no face to carry the mark.
    if (bandFrame == 0) drawSelectionCorner(renderer, rect, style);
  }
}

void MetroHomeUi::drawFace(const Tile& tile) {
  const Rect rect{tile.x, tile.y, tile.width, tile.height};
  const TileStyle style = TILE_STYLES[static_cast<size_t>(tile.id)];
  char value[12];
  switch (tile.id) {
    case TileId::Continue:
      if (tile.face == 1) {
        drawContinueProgress(tile);
      } else {
        drawContinue(tile);
      }
      break;
    case TileId::Files:
      drawIconFace(renderer, rect, style, IconId::Folder, tr(STR_BROWSE_FILES));
      break;
    case TileId::Library:
      if (tile.face == 1) {
        snprintf(value, sizeof(value), "%d", static_cast<int>(libraryCount));
        drawValueFace(renderer, rect, style, value, tr(STR_METRO_BOOKS), tr(STR_LIBRARY));
      } else if (tile.face == 2) {
        drawLibraryRecents(tile);
      } else {
        drawIconFace(renderer, rect, style, IconId::Library, tr(STR_LIBRARY));
      }
      break;
    case TileId::SlotItem:
      if (slot == Slot::PLUGINS) {
        drawIconFace(renderer, rect, style, IconId::Plugins, tr(STR_PLUGINS));
      } else if (tile.face == 1) {
        snprintf(value, sizeof(value), "%u", static_cast<unsigned>(opdsCount));
        drawValueFace(renderer, rect, style, value, tr(STR_OPDS_SERVERS), tr(STR_OPDS_BROWSER));
      } else {
        drawIconFace(renderer, rect, style, IconId::Catalog, tr(STR_OPDS_BROWSER));
      }
      break;
    case TileId::Transfer:
      // A medium tile with a title only when it stands in for the slot tile.
      drawIconFace(renderer, rect, style, IconId::Transfer, slot == Slot::NONE ? tr(STR_FILE_TRANSFER) : nullptr);
      break;
    case TileId::Settings:
      drawIconFace(renderer, rect, style, IconId::Settings, nullptr);
      break;
    case TileId::Battery:
      drawBattery(renderer, rect, style);
      break;
    case TileId::Clock:
      drawClock(renderer, rect, style);
      break;
    case TileId::Date:
      drawDate(renderer, rect, style);
      break;
    case TileId::BookCount:
      if (libraryCount >= 0) {
        snprintf(value, sizeof(value), "%d", static_cast<int>(libraryCount));
        drawStackedValue(renderer, rect, style, value, tr(STR_METRO_BOOKS));
      } else {
        drawIconFace(renderer, rect, style, IconId::Library, nullptr);
      }
      break;
    case TileId::Progress:
      if (progress >= 0) {
        snprintf(value, sizeof(value), "%d%%", static_cast<int>(progress));
        drawStackedValue(renderer, rect, style, value, tr(STR_METRO_READ));
      } else {
        drawIconFace(renderer, rect, style, IconId::Book, nullptr);
      }
      break;
  }
}

void MetroHomeUi::drawContinue(const Tile& tile) {
  const TileStyle style = TILE_STYLES[static_cast<size_t>(tile.id)];

  const Rect coverSlot{tile.x, tile.y, coverWidth, tile.height};
  if (!hasBook || !drawCover(coverSlot)) {
    const freeink::Icon& icon = pickIcon(IconId::Book, std::min(coverSlot.width, coverSlot.height));
    drawTileIcon(renderer, icon, coverSlot.x + (coverSlot.width - icon.w) / 2,
                 coverSlot.y + (coverSlot.height - icon.h) / 2, style);
  }

  const int textX = tile.x + coverWidth + INSET;
  if (!hasBook) {
    const char* text = tr(STR_NO_OPEN_BOOK);
    const int textWidth = tile.width - coverWidth - 2 * INSET;
    const int fontId =
        renderer.getTextWidth(UI_12_FONT_ID, text, EpdFontFamily::BOLD) <= textWidth ? UI_12_FONT_ID : UI_10_FONT_ID;
    const char* hint = tr(STR_METRO_PICK_BOOK);
    const bool showHint = renderer.getTextWidth(UI_10_FONT_ID, hint) <= textWidth;
    const int blockHeight = renderer.getLineHeight(fontId) + (showHint ? renderer.getLineHeight(UI_10_FONT_ID) : 0);
    const int top = tile.y + (tile.height - blockHeight) / 2;
    drawTileText(renderer, style, fontId, textX, top, text, EpdFontFamily::BOLD);
    if (showHint) drawTileText(renderer, style, UI_10_FONT_ID, textX, top + renderer.getLineHeight(fontId), hint);
    return;
  }

  const int footerTop = tile.y + tile.height - INSET - renderer.getLineHeight(SMALL_FONT_ID);
  int y = tile.y + INSET;
  for (int i = 0; i < titleLineCount; ++i) {
    drawTileText(renderer, style, UI_12_FONT_ID, textX, y, titleLines[i], EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
  y += AUTHOR_GAP;
  if (author[0] != '\0' && y + renderer.getLineHeight(UI_10_FONT_ID) <= footerTop) {
    drawTileText(renderer, style, UI_10_FONT_ID, textX, y, author);
  }
  const char* footer = tr(STR_CONTINUE_READING);
  drawTileText(renderer, style, SMALL_FONT_ID, textX, footerTop, footer);
  if (progress < 0) return;

  // Reading progress in the bottom-right corner, on the footer's baseline.
  char value[8];
  snprintf(value, sizeof(value), "%d%%", static_cast<int>(progress));
  const char* caption = tr(STR_METRO_READ);
  const int fontId = pickBigFont(renderer, value, tile.width);
  const int captionWidth = renderer.getTextWidth(UI_10_FONT_ID, caption);
  const int right = tile.x + tile.width - INSET;
  const int valueX = right - captionWidth - CAPTION_GAP - renderer.getTextWidth(fontId, value, EpdFontFamily::BOLD);
  // Not when it would run into the footer.
  if (valueX < textX + renderer.getTextWidth(SMALL_FONT_ID, footer) + INSET) return;
  const int baseline = footerTop + renderer.getFontAscenderSize(SMALL_FONT_ID);
  drawNumber(renderer, style, fontId, valueX, baseline - digitRise(renderer, fontId, EpdFontFamily::BOLD), value,
             EpdFontFamily::BOLD);
  drawTileText(renderer, style, UI_10_FONT_ID, right - captionWidth,
               baseline - renderer.getFontAscenderSize(UI_10_FONT_ID), caption);
}

void MetroHomeUi::drawContinueProgress(const Tile& tile) {
  const TileStyle style = TILE_STYLES[static_cast<size_t>(tile.id)];
  const bool black = !style.whiteInk;
  const int x = tile.x + HERO_INSET;
  const int width = tile.width - 2 * HERO_INSET;

  char value[8];
  snprintf(value, sizeof(value), "%d%%", static_cast<int>(progress));
  const int fontId = pickBigFont(renderer, value, width / 2);
  const int rise = digitRise(renderer, fontId, EpdFontFamily::BOLD);
  const int blockHeight = rise + BAR_GAP + BAR_HEIGHT + BAR_GAP + renderer.getLineHeight(UI_12_FONT_ID);
  int y = tile.y + std::max(INSET, (tile.height - blockHeight) / 2);

  // Number, bar and title start on one edge: the fonts' own side bearings differ.
  const int numberX = x - inkLeft(renderer, fontId, value, EpdFontFamily::BOLD);
  drawNumber(renderer, style, fontId, numberX, y, value, EpdFontFamily::BOLD);
  // The caption shares the number's baseline.
  const int captionX = numberX + renderer.getTextWidth(fontId, value, EpdFontFamily::BOLD) + CAPTION_GAP;
  drawTileText(renderer, style, UI_12_FONT_ID, captionX, y + rise - renderer.getFontAscenderSize(UI_12_FONT_ID),
               tr(STR_METRO_READ));
  y += rise + BAR_GAP;

  const int innerWidth = width - 2 * BORDER;
  renderer.fillRect(x, y, width, BAR_HEIGHT, black);
  renderer.fillRect(x + BORDER, y + BORDER, innerWidth, BAR_HEIGHT - 2 * BORDER, !black);
  renderer.fillRect(x + BORDER, y + BORDER, innerWidth * progress / 100, BAR_HEIGHT - 2 * BORDER, black);
  y += BAR_HEIGHT + BAR_GAP;

  drawTileText(renderer, style, UI_12_FONT_ID,
               x - inkLeft(renderer, UI_12_FONT_ID, titleOneLine, EpdFontFamily::REGULAR), y, titleOneLine);
}

void MetroHomeUi::drawLibraryRecents(const Tile& tile) {
  const TileStyle style = TILE_STYLES[static_cast<size_t>(tile.id)];
  const int x = tile.x + INSET;
  const int textWidth = tile.width - 2 * INSET;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // The tile keeps its name at the bottom-left on every face.
  const char* title = tr(STR_LIBRARY);
  const int titleFont = labelFont(renderer, title, textWidth);
  const int titleTop = tile.y + tile.height - INSET - renderer.getLineHeight(titleFont);
  drawLabel(renderer, style, titleFont, x, titleTop, title, textWidth);

  int y = tile.y + INSET;
  drawLabel(renderer, style, SMALL_FONT_ID, x, y, tr(STR_MENU_RECENT_BOOKS), textWidth);
  y += renderer.getLineHeight(SMALL_FONT_ID) + RECENT_LINE_GAP;
  for (int i = 0; i < recentCount && y + lineHeight <= titleTop; ++i) {
    drawTileText(renderer, style, UI_10_FONT_ID, x, y, recentTitles[i]);
    y += lineHeight + RECENT_LINE_GAP;
  }
}

bool MetroHomeUi::drawCover(const Rect coverSlot) {
  if (coverBroken || coverThumbPath.empty() || !Storage.exists(coverThumbPath.c_str())) return false;

  // File and decoder together are larger than the stack budget for locals.
  struct CoverReader {
    HalFile file;
    Bitmap bitmap{file};
  };
  auto reader = makeUniqueNoThrow<CoverReader>();
  if (!reader) {
    LOG_ERR("METRO", "OOM: cover reader (%u bytes)", static_cast<unsigned>(sizeof(CoverReader)));
    return false;
  }
  if (!Storage.openFileForRead("METRO", coverThumbPath, reader->file)) return false;
  if (reader->bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("METRO", "Cannot read cover thumbnail %s", coverThumbPath.c_str());
    coverBroken = true;
    return false;
  }

  // The bitmap draws its black pixels only, so the art needs white paper under it.
  const int artWidth = std::min(coverSlot.width, reader->bitmap.getWidth());
  const int artHeight = std::min(coverSlot.height, reader->bitmap.getHeight());
  const int artX = coverSlot.x + (coverSlot.width - artWidth) / 2;
  const int artY = coverSlot.y + (coverSlot.height - artHeight) / 2;
  renderer.fillRect(artX, artY, artWidth, artHeight, false);
  if (BaseTheme::drawCoverThumbFill(renderer, reader->bitmap, coverSlot)) return true;

  LOG_ERR("METRO", "Cannot draw cover thumbnail %s", coverThumbPath.c_str());
  coverBroken = true;
  fillShade(renderer, coverSlot.x, coverSlot.y, coverSlot.width, coverSlot.height,
            TILE_STYLES[static_cast<size_t>(TileId::Continue)].ink);
  return false;
}
