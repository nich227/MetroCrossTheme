#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Tile home screen of the Metro theme. HomeActivity owns one for the lifetime
// of the screen and drives selection, timing and display refreshes; this class
// owns the tile layout, the tile drawing, the live-tile face state and the
// frames of the open and return animation.
class MetroHomeUi final {
 public:
  // What the optional third menu entry opens; NONE hides it.
  enum class Slot : uint8_t { NONE, PLUGINS, OPDS };

  // Idle flip policy, applied by HomeActivity.
  static constexpr uint32_t FIRST_FLIP_DELAY_MS = 4000;
  static constexpr uint32_t FLIP_INTERVAL_MS = 7000;
  static constexpr uint32_t FLIP_JITTER_MS = 2000;
  // Flips stop after this many without user input, to bound battery use and
  // repeated driving of the panel's black pixels.
  static constexpr uint8_t MAX_IDLE_FLIPS = 12;

  explicit MetroHomeUi(GfxRenderer& renderer);

  // Builds the tile table and lays it out inside `area`. books[0], when
  // present, is the continue-reading tile. What the tiles show is copied.
  void begin(const std::vector<RecentBook>& books, Slot slot, Rect area);

  // Selection uses HomeActivity's flat index: the recent book (if any), then
  // File Browser, Library, the optional slot, File Transfer, Settings.
  void setSelection(int index);

  // Draws every tile into the framebuffer. Does not clear or refresh the display.
  void render();

  // Flat selection index of the tile under a logical point, or -1.
  int tileAt(int x, int y) const;

  // Height the continue-reading cover thumbnail must be generated at.
  int coverThumbHeight() const;

  // Live tiles. The functions below change state that render() reads: call
  // them from render(), or from loop() while holding a RenderLock.

  // Reads the reading progress shown on tiles. Loads book metadata from the SD
  // card; call once, after the first paint.
  void loadLiveData();
  // Starts flipping the next tile that has more than one face. False when no tile can flip.
  bool startFlip();
  bool flipActive() const;
  // Ends a running flip at once, on its destination face.
  void finishFlip();
  // Call after a frame was displayed. True while another frame must be drawn.
  bool advanceFlip();

  // Open and return animation: the tiles turn on the left edge of the grid as
  // plain blocks, one column after the other. One render() draws one frame.
  // The same rule as for live tiles applies to the functions that change state.

  // Tiles come in from the left, the exit played backwards: the turned frames, then the normal tiles.
  // Call from onEnter(), after begin() and before the first frame is requested.
  void startEnter();
  // Tiles leave to the left. The tile with this selection index stays in place
  // and takes the focus ring. Ends a running flip or enter animation. Does
  // nothing when that exit is already running, or when there are no tiles.
  // Call from render().
  void startExit(int index);
  // True from startEnter() until the normal tiles were displayed, and from
  // startExit() until endTransition(). No flip may start while this is true.
  // Call from render(), or from loop() while holding a RenderLock.
  bool transitionActive() const;
  bool exitActive() const;
  // Back to the normal tiles at once. Call from render().
  void endTransition();
  // Call from render() after a frame was displayed. True while another frame
  // must be drawn. An exit stays on its last frame until endTransition().
  bool advanceTransition();

 private:
  enum class TileId : uint8_t {
    Continue,
    Files,
    Library,
    SlotItem,
    Transfer,
    Settings,
    Battery,
    Clock,
    Date,
    BookCount,
    Progress
  };
  enum class Transition : uint8_t { None, Enter, Exit };

  struct Tile {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    TileId id;
    int8_t action;  // flat selection index, or -1 for a tile that cannot be selected
    uint8_t face;   // face shown now
  };

  static constexpr int MAX_TILES = 9;
  static constexpr int MAX_FACES = 3;
  static constexpr int TITLE_LINES = 3;
  static constexpr int RECENT_LINES = 3;
  static constexpr size_t LINE_BYTES = 96;

  void layout(Rect area, int menuBase);
  void loadLibraryCount();
  void cacheTexts(const std::vector<RecentBook>& books);
  const Tile* findTile(TileId id) const;
  // The next face that has data, or the current face when there is no other.
  uint8_t nextFace(const Tile& tile) const;
  // Width of a moving tile for the frame to draw, in parts per thousand of
  // its real width. 1000 draws the normal tile and 0 no tile.
  int turnedPermille(const Tile& tile) const;
  void drawTile(int index);
  void drawTurned(const Tile& tile, int permille);
  // One frame of a flip without content: the tile seen turned.
  void drawFlipBand(const Tile& tile, int frame);
  void drawFace(const Tile& tile);
  void drawContinue(const Tile& tile);
  void drawContinueProgress(const Tile& tile);
  void drawLibraryRecents(const Tile& tile);
  bool drawCover(Rect coverSlot);

  GfxRenderer& renderer;
  Tile tiles[MAX_TILES]{};
  uint8_t tileCount = 0;
  int selected = -1;
  Slot slot = Slot::NONE;
  bool hasBook = false;
  // Set when the cover thumbnail cannot be decoded, so it is not read again on every frame.
  bool coverBroken = false;
  int16_t coverWidth = 0;  // width of the cover slot on the continue-reading tile
  int16_t gridLeft = 0;    // left edge of the first column; turned tiles hinge here
  int16_t gridWidth = 0;   // first column to last column

  // 0: no flip. Else the frame to draw, from 1: the frames without content, then the new face.
  uint8_t flipStep = 0;
  int8_t flipTile = -1;    // index of the tile that is flipping, or -1
  int8_t flipCursor = -1;  // index of the tile that flipped last

  Transition transition = Transition::None;
  // Frame to draw. Enter has one frame more than its turned frames: the normal tiles.
  uint8_t transitionFrame = 0;
  int8_t exitSelection = -1;  // selection index of the tile that stays during an exit

  // Values for the back faces. A negative value, or zero servers, hides the face.
  int8_t progress = -1;
  int32_t libraryCount = -1;
  uint8_t opdsCount = 0;

  // Text is wrapped and shortened once in begin(), because doing it allocates.
  uint8_t titleLineCount = 0;
  uint8_t recentCount = 0;
  char titleLines[TITLE_LINES][LINE_BYTES]{};
  char author[LINE_BYTES]{};
  char titleOneLine[LINE_BYTES]{};
  char recentTitles[RECENT_LINES][LINE_BYTES]{};
  std::string bookPath;
  std::string coverThumbPath;
};
