#pragma once

namespace BundledSdFonts {

// Ensures bundled SD card fonts (Libron, NotoSerif) are installed and up-to-date
// on the SD card before SdCardFontSystem discovers fonts. Fast no-op (~1ms)
// when the bundle stamp and font directories already match the current build.
// Should only be called after Storage.begin() and SETTINGS.loadFromFile().
void ensureInstalled();

}  // namespace BundledSdFonts
