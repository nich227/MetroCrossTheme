#!/bin/bash

# Generates the Metro theme's Segoe UI headers (segoeui_*.h in ../builtinFonts).
#
# Segoe UI is proprietary. Its font files and the generated headers must stay
# out of the repository: the font files are ignored by builtinFonts/source/.gitignore
# and the headers by the root .gitignore. Never publish them or a firmware
# binary built with them. Without the headers the firmware still
# builds and the Metro theme uses the Ubuntu UI fonts (see metroFonts.h).
#
# Put your own licensed copies here (TrueType, not WOFF):
#   ../builtinFonts/source/SegoeUI/SegoeUI-Regular.ttf
#   ../builtinFonts/source/SegoeUI/SegoeUI-Semibold.ttf
#   ../builtinFonts/source/SegoeUI/SegoeUI-Light.ttf
# A WOFF or WOFF2 file converts with fontTools (WOFF2 also needs brotli):
#   python -c "from fontTools.ttLib import TTFont; f = TTFont('in.woff2'); f.flavor = None; f.save('out.ttf')"

set -e

cd "$(dirname "$0")"

PYTHON="${PYTHON:-python}"
SOURCE_DIR="../builtinFonts/source"
OUTPUT_DIR="../builtinFonts"
SEGOE_REGULAR="$SOURCE_DIR/SegoeUI/SegoeUI-Regular.ttf"
SEGOE_SEMIBOLD="$SOURCE_DIR/SegoeUI/SegoeUI-Semibold.ttf"
SEGOE_LIGHT="$SOURCE_DIR/SegoeUI/SegoeUI-Light.ttf"

missing=0
for font in "$SEGOE_REGULAR" "$SEGOE_SEMIBOLD" "$SEGOE_LIGHT"; do
  if [ ! -f "$font" ]; then
    echo "Missing font file: lib/EpdFont/scripts/$font" >&2
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  echo "" >&2
  echo "Segoe UI is not part of this repository, so no Metro font headers were generated." >&2
  echo "The firmware builds without them: the Metro theme then uses the Ubuntu UI fonts." >&2
  echo "To build with Segoe UI, copy your own SegoeUI-Regular.ttf, SegoeUI-Semibold.ttf and SegoeUI-Light.ttf" >&2
  echo "into lib/EpdFont/builtinFonts/source/SegoeUI/ and run this script again." >&2
  exit 1
fi

# Headers are written here first, so a failed run never leaves a partial header
# for the firmware build to pick up.
WORK_DIR="${TMPDIR:-/tmp}"
WORK_DIR="${WORK_DIR%/}/convert-metro-fonts"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
trap 'rm -rf "$WORK_DIR"' EXIT

# fontconvert.py adds the legacy kern table and the GPOS kern feature together.
# Segoe UI Regular has both, so the pairs they share would count twice. Convert
# a copy with the kern table only: it is the complete list of pairs (the script
# does not read the contextual GPOS lookups), and it is all the Semibold file has.
REGULAR="$WORK_DIR/SegoeUI-Regular.ttf"
"$PYTHON" - "$SEGOE_REGULAR" "$REGULAR" <<'EOF'
import sys
from fontTools.ttLib import TTFont

font = TTFont(sys.argv[1])
if "kern" in font and "GPOS" in font:
    del font["GPOS"]
font.save(sys.argv[2])
EOF

# The same Arabic and Hebrew intervals as the Ubuntu UI fonts. Keep this list
# equal to ARABIC_INTERVALS in convert-builtin-fonts.sh, so that a string draws
# the same way in every theme.
ARABIC_INTERVALS=(
  --additional-intervals 0x060C,0x060C
  --additional-intervals 0x061B,0x061B
  --additional-intervals 0x061F,0x061F
  --additional-intervals 0x0621,0x0621
  --additional-intervals 0x0640,0x0640
  --additional-intervals 0x0654,0x0654
  --additional-intervals 0x0660,0x0669
  --additional-intervals 0x06BA,0x06BA
  --additional-intervals 0x06D4,0x06D4
  --additional-intervals 0x06D5,0x06D5
  --additional-intervals 0x06F0,0x06F9
  --additional-intervals 0xFB56,0xFB59
  --additional-intervals 0xFB66,0xFB69
  --additional-intervals 0xFB7A,0xFB7D
  --additional-intervals 0xFB88,0xFB95
  --additional-intervals 0xFB9E,0xFB9F
  --additional-intervals 0xFBA6,0xFBB1
  --additional-intervals 0xFBFC,0xFBFF
  --additional-intervals 0xFE80,0xFEFC
)
UI_INTERVALS=(--additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}")

# Segoe UI first, then the fallbacks of the Ubuntu UI fonts in the same order,
# then Ubuntu itself: every code point the Ubuntu UI fonts cover stays covered.
REGULAR_STACK=(
  "$REGULAR"
  "$SOURCE_DIR/NotoSans/NotoSans-Regular.ttf"
  "$SOURCE_DIR/NotoSansHebrew/NotoSansHebrew-Regular.ttf"
  "$SOURCE_DIR/NotoSansArabic/NotoSansArabic-Regular.ttf"
  "$SOURCE_DIR/Ubuntu/Ubuntu-Vietnamese-Regular.ttf"
  "$SOURCE_DIR/Ubuntu/Ubuntu-Regular.ttf"
)
SEMIBOLD_STACK=(
  "$SEGOE_SEMIBOLD"
  "$SOURCE_DIR/NotoSans/NotoSans-Bold.ttf"
  "$SOURCE_DIR/NotoSansHebrew/NotoSansHebrew-Bold.ttf"
  "$SOURCE_DIR/NotoSansArabic/NotoSansArabic-Bold.ttf"
  "$SOURCE_DIR/Ubuntu/Ubuntu-Vietnamese-Bold.ttf"
  "$SOURCE_DIR/Ubuntu/Ubuntu-Bold.ttf"
)

GENERATED=()

convert() {
  local font_name="$1"
  shift
  "$PYTHON" fontconvert.py "$font_name" "$@" > "$WORK_DIR/${font_name}.h"
  GENERATED+=("$font_name")
}

# Hinting. FreeType's default TrueType mode ignores Segoe UI's horizontal hints,
# which leaves stems of two different widths in 1-bit output.
# - 8 pt: Segoe UI's own hints, applied in full by interpreter version 35.
#   The auto-hinter damages the dots of "i", ":" and "." at this size.
# - 10 pt and larger: the auto-hinter. It gives the regular face 2 px stems and
#   the semibold face 3 px stems at both sizes. With version 35 the 10 pt
#   semibold comes out thinner than the 10 pt regular.
FREETYPE_PROPERTIES="truetype:interpreter-version=35" \
  convert segoeui_8_regular 8 "${REGULAR_STACK[@]}" "${UI_INTERVALS[@]}"

for size in 10 12; do
  convert "segoeui_${size}_regular" "$size" "${REGULAR_STACK[@]}" "${UI_INTERVALS[@]}" --force-autohint
  convert "segoeui_${size}_semibold" "$size" "${SEMIBOLD_STACK[@]}" "${UI_INTERVALS[@]}" --force-autohint
done

# Number fonts for the home tiles, in the light weight: printable ASCII only,
# to keep them small. Below 18 pt the light strokes drop to one pixel.
for size in 18 24; do
  convert "segoeui_${size}_numbers" "$size" "$SEGOE_LIGHT" \
    --no-default-intervals --additional-intervals 0x20,0x7E --force-autohint
done

for font_name in "${GENERATED[@]}"; do
  mv "$WORK_DIR/${font_name}.h" "$OUTPUT_DIR/${font_name}.h"
  echo "Generated $OUTPUT_DIR/${font_name}.h"
done

echo ""
echo "Do not commit these headers or the Segoe UI font files."
