#include "BookOrbitActivity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
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
  if (state == State::LOADING) {
    statusMessage = "Connecting to BookOrbit...";
    requestUpdate();
  }
}

void BookOrbitActivity::onExit() { CatalogActivity::onExit(); }

const char* BookOrbitActivity::headerTitle() const {
  switch (currentView) {
    case ShelfView::CURRENTLY_READING:
      return "BookOrbit: Currently Reading";
    case ShelfView::WANT_TO_READ:
      return "BookOrbit: Want to Read";
    case ShelfView::ALL_BOOKS:
      return "BookOrbit: All Books";
    case ShelfView::SEARCH_RESULTS:
      return "BookOrbit: Search Results";
  }
  return "BookOrbit";
}

int BookOrbitActivity::listCount() const { return static_cast<int>(rowItems.size()); }

void BookOrbitActivity::startBrowse() { loadShelf(currentView); }

void BookOrbitActivity::retryBrowse() { loadShelf(currentView); }

void BookOrbitActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    currentView = ShelfView::CURRENTLY_READING;
    startBrowse();
    return;
  }
  activeSearchQuery = query;
  currentView = ShelfView::SEARCH_RESULTS;
  loadShelf(currentView);
}

void BookOrbitActivity::loadShelf(ShelfView view) {
  currentView = view;
  state = State::LOADING;
  statusMessage = "Fetching books...";
  requestUpdate();

  std::string shelfQuery;
  switch (view) {
    case ShelfView::CURRENTLY_READING:
      shelfQuery = "currently-reading";
      break;
    case ShelfView::WANT_TO_READ:
      shelfQuery = "want-to-read";
      break;
    case ShelfView::ALL_BOOKS:
      shelfQuery = "";
      break;
    case ShelfView::SEARCH_RESULTS:
      shelfQuery = "/api/v1/books?q=" + activeSearchQuery;
      break;
  }

  std::string err;
  const bool ok = BookOrbitClient::fetchBooks(shelfQuery, books, err);

  if (!ok) {
    errorMessage = err.empty() ? "Failed to fetch books from BookOrbit" : err;
    state = State::ERROR;
    requestUpdate();
    return;
  }

  rebuildRowItems();
  state = State::BROWSING;
  nav.selected = 0;
  requestUpdate();
}

void BookOrbitActivity::rebuildRowItems() {
  rowItems.clear();
  rowLabels.clear();
  rowValues.clear();

  rowItems.reserve(books.size());
  rowLabels.reserve(books.size());
  rowValues.reserve(books.size());

  for (size_t i = 0; i < books.size(); i++) {
    const auto& book = books[i];

    std::string label = book.title;
    if (!book.author.empty()) {
      label += " - " + book.author;
    }
    rowLabels.push_back(std::move(label));

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
    rowValues.push_back(std::move(val));

    fui::ListItem item{};
    item.label = rowLabels.back().c_str();
    item.value = rowValues.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void BookOrbitActivity::buildBrowsingScreen(UiScreen& screen) {
  if (books.empty()) {
    screen.centeredText("No books found in this shelf", screen.theme().bodyText);
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
  if (index >= 0 && index < static_cast<int>(books.size())) {
    handleBookSelected(static_cast<size_t>(index));
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
        headers.emplace_back("Authorization", "Bearer " + BOOKORBIT_STORE.getApiKey());

        const auto dlResult = downloadFile(url, filename, "", "", headers);

        if (dlResult == HttpDownloader::OK) {
          const std::string metaPath = filename + ".meta.json";
          JsonDocument metaDoc;
          metaDoc["title"] = b.title;
          metaDoc["author"] = b.author;
          metaDoc["bookorbit"]["bookId"] = b.id;
          metaDoc["bookorbit"]["fileId"] = b.fileId;
          metaDoc["bookorbit"]["serverUrl"] = BOOKORBIT_STORE.getServerUrl();

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
    case State::BROWSING:
      labels = mappedInput.mapLabels("Back", "Open/Download", "Search", "Down");
      break;
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
