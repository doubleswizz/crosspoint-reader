#pragma once

#include <string>
#include <vector>

#include "activities/CatalogActivity.h"
#include "network/BookOrbitClient.h"

class BookOrbitActivity final : public CatalogActivity {
 public:
  enum class ShelfView {
    ROOT_MENU,
    CURRENTLY_READING,
    WANT_TO_READ,
    RECENTLY_ADDED,
    ALL_BOOKS,
    LIBRARY_BOOKS,
    SEARCH_RESULTS
  };

  explicit BookOrbitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override { return true; }
  std::string searchPrefill() const override { return activeSearchQuery; }
  void performSearch(const std::string& query) override;
  void downloadFinished(bool cancelled) override;
  void onBackButton() override;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void buildBrowsingScreen(UiScreen& screen);
  void activateIndex(int index) override;
  void drawChrome() override {}
  void drawFooter() override;
  const char* headerTitle() const override;

 private:
  ShelfView currentView = ShelfView::ROOT_MENU;
  std::vector<BookOrbitLibrary> libraries;
  int activeLibraryId = 0;
  std::string activeLibraryName;
  std::string headerTitleStr = "BookOrbit";

  int currentPage = 0;
  int totalBooks = 0;
  static constexpr int PAGE_SIZE = 25;
  bool hasPrevPageRow = false;
  bool hasNextPageRow = false;

  std::vector<BookOrbitBook> books;
  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowValues;

  int pendingDownloadIndex = -1;
  std::string activeSearchQuery;

  void loadRootMenu();
  void loadShelf(ShelfView view, int page = 0);
  void rebuildRowItems();
  void handleBookSelected(size_t index);
};
