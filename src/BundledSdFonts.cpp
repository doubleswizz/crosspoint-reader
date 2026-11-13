#include "BundledSdFonts.h"

#ifdef BUNDLED_SD_FONTS

#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <SdCardFontRegistry.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "BundledSdFontsData.generated.h"
#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "SdCardFontSystem.h"

namespace {

constexpr const char* STAMP_FILENAME = ".bundled_fonts_stamp";

bool stampMatchesCurrent(const char* writeRoot) {
  char stampPath[160];
  snprintf(stampPath, sizeof(stampPath), "%s/%s", writeRoot, STAMP_FILENAME);

  HalFile f;
  if (!Storage.openFileForRead("FONT", stampPath, f)) {
    return false;
  }

  const size_t expectedLen = strlen(kBundledFontsBundleHash);
  if (f.fileSize() != expectedLen) {
    f.close();
    return false;
  }

  char buf[64];
  if (expectedLen >= sizeof(buf)) {
    f.close();
    return false;
  }
  const int n = f.read(reinterpret_cast<uint8_t*>(buf), expectedLen);
  f.close();
  if (n != static_cast<int>(expectedLen)) {
    return false;
  }
  buf[expectedLen] = '\0';
  return strcmp(buf, kBundledFontsBundleHash) == 0;
}

bool writeStamp(const char* writeRoot) {
  if (!Storage.exists(writeRoot) && !Storage.mkdir(writeRoot)) {
    LOG_ERR("FONT", "Failed to create fonts root for stamp: %s", writeRoot);
    return false;
  }
  char stampPath[160];
  snprintf(stampPath, sizeof(stampPath), "%s/%s", writeRoot, STAMP_FILENAME);

  HalFile f;
  if (!Storage.openFileForWrite("FONT", stampPath, f)) {
    LOG_ERR("FONT", "Failed to write bundled fonts stamp: %s", stampPath);
    return false;
  }
  const size_t len = strlen(kBundledFontsBundleHash);
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(kBundledFontsBundleHash), len);
  f.close();
  return written == len;
}

bool fileMatchesSizeAndCrc(const char* path, uint32_t expectedSize, uint32_t expectedCrc) {
  HalFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  if (f.fileSize() != expectedSize) {
    f.close();
    return false;
  }

  uint8_t buf[512];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  f.close();
  return crc == expectedCrc;
}

bool extractBundledAsset(FontInstaller& installer, const BundledCpfontAsset& asset) {
  if (!installer.ensureFamilyDir(asset.family)) {
    return false;
  }

  char destPath[160];
  FontInstaller::buildFontPath(asset.family, asset.filename, destPath, sizeof(destPath));

  if (fileMatchesSizeAndCrc(destPath, asset.rawSize, asset.crc32)) {
    LOG_DBG("FONT", "Bundled %s already current", asset.filename);
    return true;
  }

  char tmpPath[176];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", destPath);
  Storage.remove(tmpPath);

  HalFile out;
  if (!Storage.openFileForWrite("FONT", tmpPath, out)) {
    LOG_ERR("FONT", "Failed to open %s for write", tmpPath);
    return false;
  }

  InflateStream inflate;
  if (!inflate.init(true)) {
    out.close();
    Storage.remove(tmpPath);
    return false;
  }
  inflate.setZlibWrapped();
  inflate.setSource(asset.zlibData, asset.zlibSize);

  constexpr size_t CHUNK_SIZE = 4096;
  std::unique_ptr<uint8_t[]> outBuf(new (std::nothrow) uint8_t[CHUNK_SIZE]);
  if (!outBuf) {
    LOG_ERR("FONT", "OOM allocating inflate buffer");
    inflate.deinit();
    out.close();
    Storage.remove(tmpPath);
    return false;
  }

  uint32_t totalWritten = 0;
  uint32_t crc = 0;
  bool ok = true;
  for (;;) {
    size_t produced = 0;
    const auto status = inflate.readAtMost(outBuf.get(), CHUNK_SIZE, &produced);
    if (produced > 0) {
      const size_t written = out.write(outBuf.get(), produced);
      if (written != produced) {
        LOG_ERR("FONT", "Short write extracting %s (%zu/%zu)", asset.filename, written, produced);
        ok = false;
        break;
      }
      crc = esp_rom_crc32_le(crc, outBuf.get(), static_cast<uint32_t>(produced));
      totalWritten += static_cast<uint32_t>(produced);
    }
    if (status == InflateStream::Status::Done) {
      break;
    }
    if (status == InflateStream::Status::Error) {
      LOG_ERR("FONT", "Inflate failed for %s", asset.filename);
      ok = false;
      break;
    }
  }

  inflate.deinit();
  out.close();

  if (!ok || totalWritten != asset.rawSize || crc != asset.crc32) {
    LOG_ERR("FONT", "Bundled font verify failed for %s (size=%u/%u crc=%08x/%08x)", asset.filename, totalWritten,
            asset.rawSize, crc, asset.crc32);
    Storage.remove(tmpPath);
    return false;
  }

  if (Storage.exists(destPath) && !Storage.remove(destPath)) {
    LOG_ERR("FONT", "Failed to replace existing font %s", destPath);
    Storage.remove(tmpPath);
    return false;
  }
  if (!Storage.rename(tmpPath, destPath)) {
    LOG_ERR("FONT", "Failed to rename %s -> %s", tmpPath, destPath);
    Storage.remove(tmpPath);
    return false;
  }

  if (!installer.validateCpfontFile(destPath)) {
    Storage.remove(destPath);
    return false;
  }

  LOG_INF("FONT", "Installed bundled %s (%u bytes)", destPath, totalWritten);
  return true;
}

}  // namespace

namespace BundledSdFonts {

void ensureInstalled() {
  const char* writeRoot = SdCardFontRegistry::defaultWriteRoot();
  if (stampMatchesCurrent(writeRoot) && SdCardFontRegistry::findFamilyRoot("Libron") != nullptr &&
      SdCardFontRegistry::findFamilyRoot("NotoSerif") != nullptr) {
    return;
  }

  const bool libronWasMissing = (SdCardFontRegistry::findFamilyRoot("Libron") == nullptr);
  LOG_INF("FONT", "Installing bundled SD fonts (Libron %s, NotoSerif %s)...", kBundledLibronTag, kBundledNotoSerifTag);

  FontInstaller installer(sdFontSystem.registry());
  bool allOk = true;
  for (const auto& asset : kBundledCpfontAssets) {
    if (!extractBundledAsset(installer, asset)) {
      allOk = false;
    }
  }

  if (allOk) {
    writeStamp(writeRoot);
    // If the user upgraded from an older firmware with no SD font set yet,
    // activate Libron automatically as the default reading font.
    if (libronWasMissing && SETTINGS.sdFontFamilyName[0] == '\0') {
      strncpy(SETTINGS.sdFontFamilyName, "Libron", sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      SETTINGS.saveToFile();
    }
  }
}

}  // namespace BundledSdFonts

#else

namespace BundledSdFonts {
void ensureInstalled() {}
}  // namespace BundledSdFonts

#endif
