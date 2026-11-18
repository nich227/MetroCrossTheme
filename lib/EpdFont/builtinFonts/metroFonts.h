#pragma once

// Segoe UI glyph data for the Metro theme. Segoe UI is proprietary, so these
// headers are not in the repository: lib/EpdFont/scripts/convert-metro-fonts.sh
// generates them from a local copy of the fonts. CROSSPOINT_METRO_FONTS is 1
// when all of them are present. Define it as 0 to build without them.
// clang-format off
#ifndef CROSSPOINT_METRO_FONTS
#if __has_include("segoeui_8_regular.h") && __has_include("segoeui_10_regular.h") && \
    __has_include("segoeui_10_semibold.h") && __has_include("segoeui_12_regular.h") && \
    __has_include("segoeui_12_semibold.h") && __has_include("segoeui_18_numbers.h") && \
    __has_include("segoeui_24_numbers.h")
#define CROSSPOINT_METRO_FONTS 1
#else
#define CROSSPOINT_METRO_FONTS 0
#pragma message("Segoe UI headers not found: the Metro theme uses the Ubuntu fonts (see convert-metro-fonts.sh)")
#endif
#endif
// clang-format on

#if CROSSPOINT_METRO_FONTS
#include "segoeui_10_regular.h"
#include "segoeui_10_semibold.h"
#include "segoeui_12_regular.h"
#include "segoeui_12_semibold.h"
#include "segoeui_18_numbers.h"
#include "segoeui_24_numbers.h"
#include "segoeui_8_regular.h"
#endif
