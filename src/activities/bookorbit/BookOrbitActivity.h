#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/CatalogActivity.h"
#include "network/BookOrbitClient.h"

class BookOrbitActivity final : public CatalogActivity {
 public:
  enum class ShelfView {
    ROOT_MENU,
    LIBRARIES_LIST,
    CURRENTLY_READING,
    WANT_TO_READ,
    RECENTLY_ADDED,
    ALL_BOOKS,
    LIBRARY_BOOKS,
    SEARCH_RESULTS,
    SERIES_BOOKS
  };

  explicit BookOrbitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  static constexpr freeink::ui::ActionId ACTION_PAGE = ACTION_USER + 2;

  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override { return true; }
  std::string searchPrefill() const override { return activeSearchQuery; }
  void performSearch(const std::string& query) override;
  void downloadFinished(bool cancelled) override;
  void onBackButton() override;
  bool handleCustomInput() override;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void buildHomeTilesScreen(UiScreen& screen);
  void buildBrowsingScreen(UiScreen& screen);
  void buildPaginationBar(UiScreen& screen);
  void activateIndex(int index) override;
  void drawChrome() override {}
  void drawFooter() override;
  const char* headerTitle() const override;

 private:
  static void onPageEvent(const freeink::ui::ActionEvent& event, void* user);

  ShelfView currentView = ShelfView::ROOT_MENU;
  ShelfView parentBeforeSeriesView = ShelfView::ROOT_MENU;
  int parentPageBeforeSeries = 0;

  std::vector<BookOrbitLibrary> libraries;
  int activeLibraryId = 0;
  std::string activeLibraryName;
  int activeSeriesId = 0;
  std::string activeSeriesName;
  std::string headerTitleStr = "BookOrbit";

  int currentPage = 0;
  int totalBooks = 0;
  static constexpr int PAGE_SIZE = 25;

  std::vector<BookOrbitBook> books;
  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowSubtitles;
  std::vector<std::string> rowValues;

  int pendingDownloadIndex = -1;
  std::string activeSearchQuery;

  int lastSyncPushedCount = -1;
  int lastSyncPulledCount = -1;

  bool isPaginatedView() const;
  int totalPages() const;
  void ensureWifi(std::function<void()> onConnected);
  void loadRootMenu();
  void loadLibrariesList();
  void loadShelf(ShelfView view, int page = 0);
  void rebuildRowItems();
  void handleBookSelected(size_t index);
  void openBook(const std::string& path);
  void startProgressSync();
};
