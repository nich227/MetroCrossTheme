#pragma once

#include <cstddef>
#include <cstdint>

// Procedural triangle-mosaic wallpaper for the Lock Screen sleep mode, cached on the SD card as a
// 2-bit BMP per screen size.
namespace LockWallpaper {

constexpr size_t PATH_SIZE = 56;

// Writes the cache path for this screen size to pathOut (at least PATH_SIZE bytes) and creates the
// file when it is missing or not the expected BMP. Returns false when no valid file is available.
bool ensureFile(int width, int height, char* pathOut, size_t pathOutSize);

}  // namespace LockWallpaper
