#include "BookOrbitActivity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "BookOrbitStore.h"
#include "CrossPointSettings.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

BookOrbitActivity::BookOrbitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : CatalogActivity("BookOrbit", renderer, mappedInput) {}

void BookOrbitActivity::onEnter() {
  CatalogActivity::onEnter();
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  checkAndConnectWifi();
}

void BookOrbitActivity::onExit() { CatalogActivity::onExit(); }

const char* BookOrbitActivity::headerTitle() const { return headerTitleStr.c_str(); }

int BookOrbitActivity::listCount() const { return state == State::BROWSING ? static_cast<int>(rowItems.size()) : 0; }

void BookOrbitActivity::startBrowse() {
  if (currentView == ShelfView::ROOT_MENU) {
    loadRootMenu();
  } else {
    loadShelf(currentView, currentPage);
  }
}

void BookOrbitActivity::retryBrowse() { startBrowse(); }

void BookOrbitActivity::onBackButton() {
  if (state == State::BROWSING && currentView != ShelfView::ROOT_MENU) {
    activeSearchQuery.clear();
    loadRootMenu();
    return;
  }
  finish();
}

void BookOrbitActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    activeSearchQuery.clear();
    loadRootMenu();
    return;
  }
  activeSearchQuery = query;
  currentView = ShelfView::SEARCH_RESULTS;
  loadShelf(currentView, 0);
}

void BookOrbitActivity::loadRootMenu() {
  currentView = ShelfView::ROOT_MENU;
  currentPage = 0;
  totalBooks = 0;
  hasPrevPageRow = false;
  hasNextPageRow = false;
  headerTitleStr = "BookOrbit";

  {
    RenderLock lock(*this);
    state = State::LOADING;
    statusMessage = "Connecting to BookOrbit...";
  }
  requestUpdate(true);

  std::string err;
  BookOrbitClient::syncPendingProgress();
  if (!BookOrbitClient::fetchLibraries(libraries, err)) {
    {
      RenderLock lock(*this);
      errorMessage = err.empty() ? "Failed to connect to BookOrbit" : err;
      state = State::ERROR;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    books.clear();
    rebuildRowItems();
    state = State::BROWSING;
    nav.reset();
  }
  requestUpdate();
}

void BookOrbitActivity::loadShelf(const ShelfView view, const int page) {
  currentView = view;
  currentPage = page;

  switch (view) {
    case ShelfView::ROOT_MENU:
      loadRootMenu();
      return;
    case ShelfView::CURRENTLY_READING:
      headerTitleStr = "Continue Reading";
      break;
    case ShelfView::WANT_TO_READ:
      headerTitleStr = "Want to Read";
      break;
    case ShelfView::RECENTLY_ADDED:
      headerTitleStr = "Recently Added";
      break;
    case ShelfView::ALL_BOOKS:
      headerTitleStr = "All Books";
      break;
    case ShelfView::LIBRARY_BOOKS:
      headerTitleStr = activeLibraryName.empty() ? "Library" : activeLibraryName;
      break;
    case ShelfView::SEARCH_RESULTS:
      headerTitleStr = "\"" + activeSearchQuery + "\"";
      break;
  }

  {
    RenderLock lock(*this);
    state = State::LOADING;
    statusMessage = "Fetching books...";
  }
  requestUpdate(true);

  std::string shelfQuery;
  switch (view) {
    case ShelfView::CURRENTLY_READING:
      shelfQuery = "continue-reading";
      break;
    case ShelfView::WANT_TO_READ:
      shelfQuery = "want-to-read";
      break;
    case ShelfView::RECENTLY_ADDED:
      shelfQuery = "recently-added";
      break;
    case ShelfView::ALL_BOOKS:
      shelfQuery = "";
      break;
    case ShelfView::LIBRARY_BOOKS:
      shelfQuery = "library:" + std::to_string(activeLibraryId);
      break;
    case ShelfView::SEARCH_RESULTS:
      shelfQuery = "search:" + activeSearchQuery;
      break;
    case ShelfView::ROOT_MENU:
      break;
  }

  std::string err;
  std::vector<BookOrbitBook> fetchedBooks;
  int fetchedTotal = 0;
  const bool ok = BookOrbitClient::fetchBooks(shelfQuery, fetchedBooks, err, page, PAGE_SIZE, &fetchedTotal);

  if (!ok) {
    {
      RenderLock lock(*this);
      errorMessage = err.empty() ? "Failed to fetch books from BookOrbit" : err;
      state = State::ERROR;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    books = std::move(fetchedBooks);
    totalBooks = fetchedTotal;
    rebuildRowItems();
    state = State::BROWSING;
    nav.reset();
  }
  requestUpdate();
}

void BookOrbitActivity::rebuildRowItems() {
  rowItems.clear();
  rowLabels.clear();
  rowValues.clear();
  hasPrevPageRow = false;
  hasNextPageRow = false;

  if (currentView == ShelfView::ROOT_MENU) {
    const size_t totalRows = 4 + libraries.size();
    rowItems.reserve(totalRows);
    rowLabels.reserve(totalRows);
    rowValues.reserve(totalRows);

    auto addMenuRow = [&](std::string label, std::string val) {
      const int16_t idx = static_cast<int16_t>(rowItems.size());
      rowLabels.push_back(std::move(label));
      rowValues.push_back(std::move(val));
      fui::ListItem item{};
      item.label = rowLabels.back().c_str();
      item.value = rowValues.back().empty() ? nullptr : rowValues.back().c_str();
      item.actionValue = idx;
      rowItems.push_back(item);
    };

    addMenuRow("Continue Reading", "Shelf >");
    addMenuRow("Recently Added", "Shelf >");
    addMenuRow("Want to Read", "Shelf >");
    addMenuRow("All Books", "Catalog >");

    for (const auto& lib : libraries) {
      std::string countStr = std::to_string(lib.bookCount) + " >";
      addMenuRow("Library: " + lib.name, std::move(countStr));
    }
    return;
  }

  const bool isPaginated = (currentView == ShelfView::ALL_BOOKS || currentView == ShelfView::LIBRARY_BOOKS ||
                            currentView == ShelfView::SEARCH_RESULTS);
  hasPrevPageRow = isPaginated && currentPage > 0;
  hasNextPageRow = isPaginated && ((currentPage + 1) * PAGE_SIZE < totalBooks);

  const size_t totalRows = books.size() + (hasPrevPageRow ? 1 : 0) + (hasNextPageRow ? 1 : 0);
  rowItems.reserve(totalRows);
  rowLabels.reserve(totalRows);
  rowValues.reserve(totalRows);

  auto addRow = [&](std::string label, std::string val) {
    const int16_t idx = static_cast<int16_t>(rowItems.size());
    rowLabels.push_back(std::move(label));
    rowValues.push_back(std::move(val));
    fui::ListItem item{};
    item.label = rowLabels.back().c_str();
    item.value = rowValues.back().empty() ? nullptr : rowValues.back().c_str();
    item.actionValue = idx;
    rowItems.push_back(item);
  };

  if (hasPrevPageRow) {
    const int totalPages = (totalBooks + PAGE_SIZE - 1) / PAGE_SIZE;
    addRow("<< Previous Page", "Page " + std::to_string(currentPage) + "/" + std::to_string(totalPages));
  }

  for (size_t i = 0; i < books.size(); i++) {
    const auto& book = books[i];

    std::string label = book.title;
    if (!book.author.empty()) {
      label += " - " + book.author;
    }

    std::string val;
    if (book.isDownloaded) {
      val = "[Saved] ";
    }
    if (book.progress > 0.0f) {
      char pct[16];
      snprintf(pct, sizeof(pct), "%.0f%%", book.progress * 100.0f);
      val += pct;
    } else if (!book.isDownloaded) {
      val = "Cloud";
    }
    addRow(std::move(label), std::move(val));
  }

  if (hasNextPageRow) {
    const int totalPages = (totalBooks + PAGE_SIZE - 1) / PAGE_SIZE;
    addRow("Next Page >>", "Page " + std::to_string(currentPage + 2) + "/" + std::to_string(totalPages));
  }
}

void BookOrbitActivity::buildBrowsingScreen(UiScreen& screen) {
  if (rowItems.empty()) {
    screen.centeredText("No EPUB books found in this shelf", screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void BookOrbitActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, headerTitle());
  if (buildStatusScreen(screen)) return;
  buildBrowsingScreen(screen);
}

void BookOrbitActivity::activateIndex(int index) {
  if (state != State::BROWSING) return;
  app.clearTapFlash();
  if (index < 0 || index >= static_cast<int>(rowItems.size())) return;

  if (currentView == ShelfView::ROOT_MENU) {
    if (index == 0) {
      loadShelf(ShelfView::CURRENTLY_READING, 0);
    } else if (index == 1) {
      loadShelf(ShelfView::RECENTLY_ADDED, 0);
    } else if (index == 2) {
      loadShelf(ShelfView::WANT_TO_READ, 0);
    } else if (index == 3) {
      loadShelf(ShelfView::ALL_BOOKS, 0);
    } else {
      const size_t libIdx = static_cast<size_t>(index - 4);
      if (libIdx < libraries.size()) {
        activeLibraryId = libraries[libIdx].id;
        activeLibraryName = libraries[libIdx].name;
        loadShelf(ShelfView::LIBRARY_BOOKS, 0);
      }
    }
    return;
  }

  if (hasPrevPageRow && index == 0) {
    loadShelf(currentView, currentPage - 1);
    return;
  }
  if (hasNextPageRow && index == static_cast<int>(rowItems.size()) - 1) {
    loadShelf(currentView, currentPage + 1);
    return;
  }

  const int bookIdx = index - (hasPrevPageRow ? 1 : 0);
  if (bookIdx >= 0 && bookIdx < static_cast<int>(books.size())) {
    handleBookSelected(static_cast<size_t>(bookIdx));
  }
}

void BookOrbitActivity::handleBookSelected(size_t index) {
  if (index >= books.size()) return;
  auto& book = books[index];

  // If already downloaded, immediately launch into reader!
  if (book.isDownloaded && !book.localFilePath.empty()) {
    activityManager.goToReader(book.localFilePath);
    return;
  }

  // Not downloaded: prompt user to download
  pendingDownloadIndex = static_cast<int>(index);
  const std::string confirmText = "Download \"" + book.title + "\" to device?";

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Download Book", confirmText, "Cancel", "Download"),
      [this, index](const ActivityResult& result) {
        if (result.isCancelled) return;

        auto& b = books[index];
        beginDownload(b.title);

        std::string token;
        std::string authErr;
        if (!BookOrbitClient::ensureAuthenticated(token, authErr)) {
          finishDownload(HttpDownloader::UNAUTHORIZED);
          return;
        }

        const std::string folder = BookOrbitClient::getDownloadDir();
        if (!Storage.exists(folder.c_str())) {
          Storage.mkdir(folder.c_str());
        }

        const auto format = static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat);
        const std::string filename = folder + "/" + opdsBookFilename(b.author, b.title, format);

        std::string downloadEndpoint;
        if (!b.fileId.empty()) {
          downloadEndpoint = "/api/v1/books/files/" + b.fileId + "/download";
        } else {
          downloadEndpoint = "/api/v1/books/" + b.id + "/download";
        }

        std::string baseUrl = BOOKORBIT_STORE.getServerUrl();
        while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
        const std::string url = baseUrl + downloadEndpoint;

        std::vector<HttpDownloader::Header> headers;
        headers.emplace_back("Authorization", "Bearer " + token);

        const auto dlResult = downloadFile(url, filename, "", "", headers);

        if (dlResult == HttpDownloader::OK) {
          const std::string metaPath = filename + ".meta.json";
          JsonDocument metaDoc;
          metaDoc["title"] = b.title;
          metaDoc["author"] = b.author;
          metaDoc["bookorbit"]["bookId"] = b.id;
          metaDoc["bookorbit"]["fileId"] = b.fileId;
          metaDoc["bookorbit"]["serverUrl"] = BOOKORBIT_STORE.getServerUrl();
          metaDoc["progress"] = b.progress;
          metaDoc["pendingSync"] = false;

          HalFile metaFile;
          if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
            serializeJson(metaDoc, metaFile);
            metaFile.close();
          }

          b.isDownloaded = true;
          b.localFilePath = filename;
        }

        finishDownload(dlResult);
      });
}

void BookOrbitActivity::downloadFinished(const bool cancelled) {
  if (cancelled || pendingDownloadIndex < 0 || pendingDownloadIndex >= static_cast<int>(books.size())) {
    state = State::BROWSING;
    requestUpdate();
    return;
  }

  auto& book = books[pendingDownloadIndex];
  BookOrbitClient::checkLocalAvailability(book);

  if (book.isDownloaded && !book.localFilePath.empty()) {
    activityManager.goToReader(book.localFilePath);
  } else {
    state = State::BROWSING;
    rebuildRowItems();
    requestUpdate();
  }
}

void BookOrbitActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      const char* confirmLabel = (currentView == ShelfView::ROOT_MENU) ? "Open" : "Open/Download";
      const char* upLabel = (hasSearch() && nav.selected == 0) ? "Search" : "Up";
      labels = mappedInput.mapLabels("Back", confirmLabel, upLabel, "Down");
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels("Cancel", "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels("Back", "Retry", "", "");
      break;
    default:
      labels = mappedInput.mapLabels("Back", "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
