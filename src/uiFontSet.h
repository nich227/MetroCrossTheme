#pragma once

// Selects the fonts drawn for SMALL_FONT_ID, UI_10_FONT_ID and UI_12_FONT_ID:
// Segoe UI when enabled, Ubuntu otherwise. Call from the main loop task only.
// A build without the Segoe UI headers always draws Ubuntu.
void useMetroUiFonts(bool enabled);
