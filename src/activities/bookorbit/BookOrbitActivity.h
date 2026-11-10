#pragma once

#include <vector>

#include "activities/CatalogActivity.h"
#include "network/BookOrbitClient.h"

class BookOrbitActivity final : public CatalogActivity {
 public:
  enum class ShelfView { CURRENTLY_READING, WANT_TO_READ, ALL_BOOKS, SEARCH_RESULTS };

  explicit BookOrbitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;

 protected:
  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override { return true; }
  void performSearch(const std::string& query) override;
  void downloadFinished(bool cancelled) override;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void buildBrowsingScreen(UiScreen& screen);
  void activateIndex(int index) override;
  void drawFooter() override;
  const char* headerTitle() const override;

 private:
  ShelfView currentView = ShelfView::CURRENTLY_READING;
  std::vector<BookOrbitBook> books;
  std::vector<freeink::ui::ListItem> rowItems;
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowValues;

  int pendingDownloadIndex = -1;
  std::string activeSearchQuery;

  void loadShelf(ShelfView view);
  void rebuildRowItems();
  void handleBookSelected(size_t index);
};
