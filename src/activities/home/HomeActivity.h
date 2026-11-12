#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  bool hasBookOrbit = false;
  // The home "library" slot (index 2) shows Plugins while the plugin system is
  // enabled, otherwise OPDS when a server is configured. NONE hides the slot.
  // NetworkModeSelectionActivity lists OPDS in exactly the other cases.
  HomeMenuItem slotItem() const {
    if (hasPlugins) return HomeMenuItem::PLUGINS;
    return hasOpdsServers ? HomeMenuItem::OPDS_BROWSER : HomeMenuItem::NONE;
  }
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Output polarity the snapshot was taken in (night mode stores images pre-inverted).
  bool coverBufferInverted = false;
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, HomeMenuItem slot, bool hasBookOrbit) {
    if (hasBookOrbit) {
      switch (item) {
        case HomeMenuItem::BOOKORBIT_CONTINUE:
          return 0;
        case HomeMenuItem::BOOKORBIT_SEARCH:
          return 1;
        case HomeMenuItem::BOOKORBIT_LIBRARIES:
          return 2;
        case HomeMenuItem::BOOKORBIT_SYNC:
          return 3;
        case HomeMenuItem::MORE:
        case HomeMenuItem::FILE_BROWSER:
        case HomeMenuItem::LIBRARY:
        case HomeMenuItem::PLUGINS:
        case HomeMenuItem::OPDS_BROWSER:
        case HomeMenuItem::BOOKORBIT:
        case HomeMenuItem::FILE_TRANSFER:
        case HomeMenuItem::SETTINGS_MENU:
          return 4;
        default:
          return 0;
      }
    }
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (slot != HomeMenuItem::NONE && item == slot) return i;
    if (item == HomeMenuItem::PLUGINS) return 0;
    if (slot != HomeMenuItem::NONE) ++i;
    // File Transfer also opens the OPDS browser, so it returns there without its slot.
    if (item == HomeMenuItem::FILE_TRANSFER || item == HomeMenuItem::OPDS_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, HomeMenuItem slot, bool hasBookOrbit) {
    if (hasBookOrbit) {
      switch (idx) {
        case 0:
          return HomeMenuItem::BOOKORBIT_CONTINUE;
        case 1:
          return HomeMenuItem::BOOKORBIT_SEARCH;
        case 2:
          return HomeMenuItem::BOOKORBIT_LIBRARIES;
        case 3:
          return HomeMenuItem::BOOKORBIT_SYNC;
        case 4:
          return HomeMenuItem::MORE;
        default:
          return HomeMenuItem::NONE;
      }
    }
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (slot != HomeMenuItem::NONE && idx == i++) return slot;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onRecentBookSelected(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();
  void onBookOrbitContinueOpen();
  void onBookOrbitSearchOpen();
  void onBookOrbitLibrariesOpen();
  void onBookOrbitSyncOpen();
  void onMoreOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
