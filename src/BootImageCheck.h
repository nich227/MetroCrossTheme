#pragma once

#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Checks that a slot holds a complete ESP app image: the segment table, the
// checksum byte and the SHA-256 that follows the image. An update that stopped
// half way leaves a good header over a broken body, so the header alone says
// nothing. Header-only and free of ESP calls, so that it can be tested on a host.
namespace boot_image {

constexpr size_t CHUNK_SIZE = 4096;

// read(offset, buffer, length) returns true when it filled the buffer.
// `scratch` must hold CHUNK_SIZE bytes. An image without the SHA-256 is
// refused: its one checksum byte is too weak to trust.
template <typename Read>
bool isComplete(Read&& read, const size_t slotSize, uint8_t* scratch) {
  constexpr size_t headerSize = 24;
  constexpr size_t segmentHeaderSize = 8;
  constexpr size_t shaSize = 32;
  constexpr size_t checksumAlign = 16;
  constexpr uint8_t imageMagic = 0xE9;
  constexpr uint8_t checksumSeed = 0xEF;
  constexpr uint8_t maxSegments = 16;  // ESP_IMAGE_MAX_SEGMENTS

  uint8_t header[headerSize];
  if (slotSize < headerSize || !read(size_t{0}, header, headerSize)) return false;
  const uint8_t segmentCount = header[1];
  const bool hashAppended = header[23] == 1;
  if (header[0] != imageMagic || segmentCount == 0 || segmentCount > maxSegments || !hashAppended) return false;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  mbedtls_sha256_update(&sha, header, headerSize);

  uint8_t checksum = checksumSeed;
  size_t pos = headerSize;
  bool ok = true;
  for (uint8_t segment = 0; ok && segment < segmentCount; ++segment) {
    uint8_t segmentHeader[segmentHeaderSize];
    ok = segmentHeaderSize <= slotSize - pos && read(pos, segmentHeader, segmentHeaderSize);
    if (!ok) break;
    mbedtls_sha256_update(&sha, segmentHeader, segmentHeaderSize);
    pos += segmentHeaderSize;

    uint32_t length = 0;
    std::memcpy(&length, segmentHeader + 4, sizeof(length));
    ok = length <= slotSize - pos;
    for (size_t remaining = length; ok && remaining > 0;) {
      const size_t want = std::min(CHUNK_SIZE, remaining);
      ok = read(pos, scratch, want);
      if (!ok) break;
      mbedtls_sha256_update(&sha, scratch, want);
      for (size_t i = 0; i < want; ++i) checksum ^= scratch[i];
      pos += want;
      remaining -= want;
    }
  }

  // Padding up to a multiple of 16 follows the last segment; its last byte is the checksum.
  size_t hashOffset = 0;
  if (ok) {
    hashOffset = (pos + checksumAlign) & ~(checksumAlign - 1);
    const size_t padLength = hashOffset - pos;
    uint8_t pad[checksumAlign];
    ok = hashOffset <= slotSize && shaSize <= slotSize - hashOffset && read(pos, pad, padLength);
    if (ok) {
      mbedtls_sha256_update(&sha, pad, padLength);
      ok = pad[padLength - 1] == checksum;
    }
  }

  uint8_t computed[shaSize];
  mbedtls_sha256_finish(&sha, computed);
  mbedtls_sha256_free(&sha);
  uint8_t stored[shaSize];
  return ok && read(hashOffset, stored, shaSize) && std::memcmp(computed, stored, shaSize) == 0;
}

}  // namespace boot_image
