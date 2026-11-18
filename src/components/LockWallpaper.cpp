#include "LockWallpaper.h"

#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

namespace {

// The look of the pattern. scripts/preview_lock_wallpaper.py reads these values from this file.
// Bump CACHE_VERSION after any change here so cached files are rebuilt.
constexpr int CACHE_VERSION = 3;
constexpr int CELL_COLUMNS = 12;   // squares across the screen width
constexpr int MIN_CELL_SIZE = 24;  // pixels
constexpr int PATCH_SIZE = 3;      // patch edge, in triangle pairs along a diagonal
constexpr uint32_t HASH_SEED = 0x2F6B8A31;

// Tones: 0 black, 1 black/dark, 2 dark gray, 3 dark/light, 4 light gray, 5 light/white, 6 white.
// Even tones are the panel's four gray levels. An odd tone is a one-pixel checkerboard of the
// two levels next to it, which the eye reads as a gray between them. Triangles have no outline,
// so neighbours of one tone join into larger facets.
constexpr int TONE_COUNT = 7;
constexpr uint8_t PATCH_TONE_WEIGHTS[TONE_COUNT] = {0, 1, 3, 5, 6, 5, 3};
// A patch tone stays in this range after its triangle shift.
constexpr int PATCH_TONE_MIN = 1;
constexpr int PATCH_TONE_MAX = 6;
// Each triangle moves its patch tone by one of these, picked with equal chance.
constexpr int8_t TRIANGLE_SHIFTS[] = {-1, 0, 0, 0, 1};
// One triangle in ACCENT_ONE_IN ignores its patch and takes a tone from ACCENT_TONE_WEIGHTS.
constexpr int ACCENT_ONE_IN = 14;
constexpr uint8_t ACCENT_TONE_WEIGHTS[TONE_COUNT] = {3, 2, 2, 0, 0, 0, 3};

constexpr int COORD_BIAS = 1024;  // keeps patch coordinates positive before division
constexpr uint32_t PATCH_SALT = 0x9E3779B9;

constexpr size_t BMP_HEADER_SIZE = 70;  // file header + info header + four palette entries
constexpr uint8_t GRAY_PALETTE[16] = {0, 0, 0, 0, 85, 85, 85, 0, 170, 170, 170, 0, 255, 255, 255, 0};

enum Triangle : uint8_t { TOP = 0, RIGHT = 1, BOTTOM = 2, LEFT = 3, TRIANGLE_COUNT = 4 };

struct Grid {
  int cell;     // square size in pixels, always even
  int offsetX;  // grid coordinate of screen pixel (0, 0); grid lines sit at multiples of `cell`
  int offsetY;
};

Grid makeGrid(const int width, const int height) {
  // An even size puts both diagonals on one center pixel.
  int cell = (width + CELL_COLUMNS) / (2 * CELL_COLUMNS) * 2;
  if (cell < MIN_CELL_SIZE) cell = MIN_CELL_SIZE;
  // Split the leftover pixels evenly so cut cells match on opposite edges.
  return {cell, cell - (width % cell) / 2, cell - (height % cell) / 2};
}

uint32_t mix(uint32_t h) {
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}

uint32_t hash3(const uint32_t a, const uint32_t b, const uint32_t c) { return mix(mix(mix(HASH_SEED ^ a) + b) + c); }

constexpr uint32_t weightSum(const uint8_t* weights) {
  uint32_t sum = 0;
  for (int i = 0; i < TONE_COUNT; ++i) sum += weights[i];
  return sum;
}
static_assert(weightSum(PATCH_TONE_WEIGHTS) > 0 && weightSum(ACCENT_TONE_WEIGHTS) > 0);

int pickTone(const uint8_t* weights, const uint32_t total, const uint32_t h) {
  uint32_t r = h % total;
  for (int tone = 0; tone < TONE_COUNT - 1; ++tone) {
    if (r < weights[tone]) return tone;
    r -= weights[tone];
  }
  return TONE_COUNT - 1;
}

int triangleTone(const int cellX, const int cellY, const int triangle) {
  const uint32_t own =
      hash3(static_cast<uint32_t>(cellX), static_cast<uint32_t>(cellY), static_cast<uint32_t>(triangle));
  if (own % ACCENT_ONE_IN == 0) return pickTone(ACCENT_TONE_WEIGHTS, weightSum(ACCENT_TONE_WEIGHTS), own >> 8);

  // Diagonal coordinates of the triangle. Two triangles that share a square side get the same
  // pair, so patches are diamonds bounded by the diagonal lines.
  const int a = cellX + cellY + (triangle == RIGHT || triangle == BOTTOM ? 1 : 0);
  const int b = cellX - cellY + (triangle == TOP || triangle == RIGHT ? 1 : 0) + COORD_BIAS;
  const uint32_t patch =
      hash3(static_cast<uint32_t>(a / PATCH_SIZE), static_cast<uint32_t>(b / PATCH_SIZE), PATCH_SALT);

  constexpr uint32_t shiftCount = sizeof(TRIANGLE_SHIFTS) / sizeof(TRIANGLE_SHIFTS[0]);
  int tone =
      pickTone(PATCH_TONE_WEIGHTS, weightSum(PATCH_TONE_WEIGHTS), patch) + TRIANGLE_SHIFTS[(own >> 8) % shiftCount];
  if (tone < PATCH_TONE_MIN) tone = PATCH_TONE_MIN;
  if (tone > PATCH_TONE_MAX) tone = PATCH_TONE_MAX;
  return tone;
}

// gx, gy: grid coordinates; lx, ly: the same pixel relative to its cell's top-left corner.
uint8_t pixelLevel(const Grid& grid, const int cellX, const int cellY, const int lx, const int ly, const int gx,
                   const int gy, int* tones) {
  const int triangle = ly < lx ? (lx + ly < grid.cell ? TOP : RIGHT) : (lx + ly < grid.cell ? LEFT : BOTTOM);
  if (tones[triangle] < 0) tones[triangle] = triangleTone(cellX, cellY, triangle);
  const int tone = tones[triangle];
  // A mixed tone puts its lighter level on every other pixel, in a checkerboard.
  return static_cast<uint8_t>((tone >> 1) + (tone & (gx + gy) & 1));
}

void resetTones(int* tones) {
  for (int i = 0; i < TRIANGLE_COUNT; ++i) tones[i] = -1;
}

// Packs one image row as 2 bits per pixel, leftmost pixel in the high bits.
void fillRow(const Grid& grid, const int y, const int width, uint8_t* row, const size_t rowBytes) {
  std::memset(row, 0, rowBytes);
  const int gy = y + grid.offsetY;
  const int cellY = gy / grid.cell;
  const int ly = gy % grid.cell;
  int cellX = grid.offsetX / grid.cell;
  int lx = grid.offsetX % grid.cell;
  int tones[TRIANGLE_COUNT];
  resetTones(tones);
  for (int x = 0; x < width; ++x) {
    const uint8_t level = pixelLevel(grid, cellX, cellY, lx, ly, x + grid.offsetX, gy, tones);
    row[x >> 2] |= static_cast<uint8_t>(level << (6 - 2 * (x & 3)));
    if (++lx == grid.cell) {
      lx = 0;
      ++cellX;
      resetTones(tones);
    }
  }
}

// The exact header of a valid cache file: top-down, 2 bits per pixel, the four native grays.
void buildHeader(const int width, const int height, const size_t rowBytes, uint8_t* out) {
  BmpHeader header;
  createBmpHeader(&header, width, height, BmpRowOrder::TopDown);
  header.infoHeader.biBitCount = 2;
  header.infoHeader.biClrUsed = header.infoHeader.biClrImportant = 4;
  header.infoHeader.biSizeImage = static_cast<uint32_t>(rowBytes) * height;
  header.fileHeader.bfOffBits = BMP_HEADER_SIZE;
  header.fileHeader.bfSize = BMP_HEADER_SIZE + header.infoHeader.biSizeImage;
  constexpr size_t fixedSize = sizeof(header) - sizeof(header.colors);
  static_assert(fixedSize + sizeof(GRAY_PALETTE) == BMP_HEADER_SIZE);
  std::memcpy(out, &header, fixedSize);
  std::memcpy(out + fixedSize, GRAY_PALETTE, sizeof(GRAY_PALETTE));
}

bool isValidFile(const char* path, const uint8_t* header, const size_t fileSize) {
  if (!Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("LWP", path, file)) return false;
  uint8_t found[BMP_HEADER_SIZE];
  return file.size() == fileSize && file.read(found, sizeof(found)) == static_cast<int>(sizeof(found)) &&
         std::memcmp(found, header, sizeof(found)) == 0;
}

bool writeFile(const char* path, const uint8_t* header, const int width, const int height, const size_t rowBytes) {
  char tmpPath[LockWallpaper::PATH_SIZE + 4];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  auto row = makeUniqueNoThrow<uint8_t[]>(rowBytes);
  if (!row) {
    LOG_ERR("LWP", "OOM: wallpaper row (%u bytes)", static_cast<unsigned>(rowBytes));
    return false;
  }

  HalFile out;
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.openFileForWrite("LWP", tmpPath, out)) {
    LOG_ERR("LWP", "Cannot create %s", tmpPath);
    return false;
  }

  const Grid grid = makeGrid(width, height);
  bool ok = out.write(header, BMP_HEADER_SIZE) == BMP_HEADER_SIZE;
  for (int y = 0; ok && y < height; ++y) {
    fillRow(grid, y, width, row.get(), rowBytes);
    ok = out.write(row.get(), rowBytes) == rowBytes;
  }
  // The file gets its final name only when it is complete.
  ok = out.close() && ok;
  ok = ok && Storage.replaceFile(tmpPath, path);
  if (!ok) {
    LOG_ERR("LWP", "Failed to write %s", path);
    Storage.remove(tmpPath);
  }
  return ok;
}

}  // namespace

bool LockWallpaper::ensureFile(const int width, const int height, char* pathOut, const size_t pathOutSize) {
  const int length =
      snprintf(pathOut, pathOutSize, "/.crosspoint/lock_wallpaper_v%d_%dx%d.bmp", CACHE_VERSION, width, height);
  if (width <= 0 || height <= 0 || length < 0 || static_cast<size_t>(length) >= pathOutSize ||
      static_cast<size_t>(length) >= PATH_SIZE) {
    LOG_ERR("LWP", "Bad wallpaper size %dx%d or path buffer", width, height);
    return false;
  }

  const size_t rowBytes = (static_cast<size_t>(width) * 2 + 31) / 32 * 4;
  uint8_t header[BMP_HEADER_SIZE];
  buildHeader(width, height, rowBytes, header);
  if (isValidFile(pathOut, header, BMP_HEADER_SIZE + rowBytes * height)) return true;
  return writeFile(pathOut, header, width, height, rowBytes);
}
