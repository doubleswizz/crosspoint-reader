#include "BookOrbitActivity.h"

#include <ArduinoJson.h>
#include <FreeInkUIIcon.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "BookOrbitStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "components/icons/opdsIcons.h"
#include "components/icons/search32.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t PAGE_FIRST = 0;
constexpr int16_t PAGE_PREV = 1;
constexpr int16_t PAGE_NEXT = 2;
constexpr int16_t PAGE_LAST = 3;
}  // namespace

BookOrbitActivity::BookOrbitActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const EntryMode entryMode)
    : CatalogActivity("BookOrbit", renderer, mappedInput), entryMode(entryMode) {}

void BookOrbitActivity::onEnter() {
  CatalogActivity::onEnter();
  app.on(ACTION_PAGE, &BookOrbitActivity::onPageEvent, this);

  switch (entryMode) {
    case EntryMode::CONTINUE_READING:
      currentView = ShelfView::CURRENTLY_READING;
      headerTitleStr = "Continue Reading";
      if (BOOKORBIT_STORE.isConfigured() && BOOKORBIT_STORE.getSyncOnOpen()) {
        ensureWifi([this] { startProgressSync(); },
                   [this] {
                     if (!tryLoadLocalContinueReading()) {
                       finish();
                     }
                   });
      } else if (wifiConnected()) {
        loadShelf(ShelfView::CURRENTLY_READING, 0);
      } else if (!tryLoadLocalContinueReading()) {
        ensureWifi([this] { loadShelf(ShelfView::CURRENTLY_READING, 0); }, [this] { finish(); });
      }
      break;
    case EntryMode::SEARCH:
      currentView = ShelfView::SEARCH_RESULTS;
      headerTitleStr = "Search";
      ensureWifi([this] { launchInitialSearch(); }, [this] { finish(); });
      break;
    case EntryMode::LIBRARIES:
      currentView = ShelfView::LIBRARIES_LIST;
      headerTitleStr = "Libraries";
      ensureWifi([this] { loadLibrariesList(); }, [this] { finish(); });
      break;
    case EntryMode::SYNC_ONLY:
      headerTitleStr = "Sync";
      if (!BOOKORBIT_STORE.isConfigured()) {
        RenderLock lock(*this);
        errorMessage = "BookOrbit is not configured";
        state = State::ERROR;
        requestUpdate();
        return;
      }
      ensureWifi([this] { startProgressSync(); }, [this] { finish(); });
      break;
    case EntryMode::ROOT_MENU:
    default:
      loadRootMenu();
      // Sync on open: join Wi-Fi, pull progress from other devices, push anything pending. Backing out of the
      // Wi-Fi screen (or no network) simply leaves the offline BookOrbit screen, so reading local books still works.
      if (BOOKORBIT_STORE.isConfigured() && BOOKORBIT_STORE.getSyncOnOpen()) {
        ensureWifi([this] { startProgressSync(); });
      }
      break;
  }
}

void BookOrbitActivity::onExit() { CatalogActivity::onExit(); }

const char* BookOrbitActivity::headerTitle() const { return headerTitleStr.c_str(); }

int BookOrbitActivity::listCount() const {
  if (state != State::BROWSING) return 0;
  if (currentView == ShelfView::ROOT_MENU) return 3;
  return static_cast<int>(rowItems.size());
}

bool BookOrbitActivity::isPaginatedView() const {
  return currentView == ShelfView::ALL_BOOKS || currentView == ShelfView::LIBRARY_BOOKS ||
         currentView == ShelfView::SEARCH_RESULTS || currentView == ShelfView::SERIES_BOOKS;
}

int BookOrbitActivity::totalPages() const {
  if (!isPaginatedView() || totalBooks <= 0) return 1;
  return (totalBooks + PAGE_SIZE - 1) / PAGE_SIZE;
}

void BookOrbitActivity::startBrowse() {
  if (entryMode == EntryMode::SYNC_ONLY) {
    startProgressSync();
  } else if (entryMode == EntryMode::SEARCH && activeSearchQuery.empty()) {
    launchInitialSearch();
  } else if (currentView == ShelfView::ROOT_MENU) {
    loadRootMenu();
  } else if (currentView == ShelfView::LIBRARIES_LIST) {
    loadLibrariesList();
  } else {
    loadShelf(currentView, currentPage);
  }
}

void BookOrbitActivity::retryBrowse() {
  if (entryMode == EntryMode::ROOT_MENU && currentView == ShelfView::ROOT_MENU) {
    loadRootMenu();
  } else if (!wifiConnected()) {
    ensureWifi([this] { startBrowse(); },
               entryMode != EntryMode::ROOT_MENU ? std::function<void()>([this] { finish(); }) : nullptr);
  } else {
    startBrowse();
  }
}

void BookOrbitActivity::onBackButton() {
  if (state == State::BROWSING) {
    if (currentView == ShelfView::SERIES_BOOKS) {
      loadShelf(parentBeforeSeriesView, parentPageBeforeSeries);
      return;
    }
    if (currentView == ShelfView::LIBRARY_BOOKS) {
      loadLibrariesList();
      return;
    }
    if (entryMode == EntryMode::ROOT_MENU && currentView != ShelfView::ROOT_MENU) {
      activeSearchQuery.clear();
      loadRootMenu();
      return;
    }
  }
  finish();
}

bool BookOrbitActivity::handleCustomInput() {
  if (state == State::BROWSING && isPaginatedView() && totalPages() > 1) {
    if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
      if (currentPage + 1 < totalPages()) {
        loadShelf(currentView, currentPage + 1);
      }
      return true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
      if (currentPage > 0) {
        loadShelf(currentView, currentPage - 1);
      }
      return true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
        mappedInput.isPressed(MappedInputManager::Button::PageBack)) {
      return true;
    }
  }
  return CatalogActivity::handleCustomInput();
}

void BookOrbitActivity::onPageEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitActivity*>(user);
  if (self->state != State::BROWSING || !self->isPaginatedView()) return;
  self->app.clearTapFlash();
  const int pages = self->totalPages();
  int nextPage = self->currentPage;
  switch (event.value) {
    case PAGE_FIRST:
      nextPage = 0;
      break;
    case PAGE_PREV:
      if (self->currentPage > 0) nextPage = self->currentPage - 1;
      break;
    case PAGE_NEXT:
      if (self->currentPage + 1 < pages) nextPage = self->currentPage + 1;
      break;
    case PAGE_LAST:
      if (pages > 0) nextPage = pages - 1;
      break;
    default:
      return;
  }
  if (nextPage != self->currentPage) {
    self->loadShelf(self->currentView, nextPage);
  }
}

void BookOrbitActivity::ensureWifi(std::function<void()> onConnected, std::function<void()> onCancelled) {
  if (wifiConnected()) {
    if (onConnected) onConnected();
    return;
  }

  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid())) {
    {
      RenderLock lock(*this);
      state = State::LOADING;
      statusMessage = "Connecting to Wi-Fi...";
    }
    requestUpdate(true);
    if (BookOrbitClient::connectWifiBriefly(8000) && wifiConnected()) {
      if (onConnected) onConnected();
      return;
    }
  }

  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    RenderLock lock(*this);
    errorMessage = "Out of memory launching Wi-Fi";
    state = State::ERROR;
    requestUpdate();
    return;
  }

  state = State::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::move(wifi), [this, callback = std::move(onConnected),
                                           cancelCb = std::move(onCancelled)](const ActivityResult& result) {
    if (result.isCancelled || !wifiConnected()) {
      if (cancelCb) {
        cancelCb();
        return;
      }
      RenderLock lock(*this);
      state = State::BROWSING;
      requestUpdate();
    } else {
      if (callback) {
        callback();
      }
    }
  });
}

void BookOrbitActivity::launchInitialSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), searchPrefill());
  if (!keyboard) {
    LOG_ERR("BKO", "OOM: search keyboard");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  {
    RenderLock lock(*this);
    state = State::SEARCH_INPUT;
  }
  requestUpdate();
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    const std::string query = std::get<KeyboardResult>(result.data).text;
    if (query.empty()) {
      finish();
      return;
    }
    state = State::BROWSING;
    performSearch(query);
  });
}

void BookOrbitActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    activeSearchQuery.clear();
    if (entryMode == EntryMode::SEARCH) {
      finish();
    } else {
      loadRootMenu();
    }
    return;
  }
  activeSearchQuery = query;
  currentView = ShelfView::SEARCH_RESULTS;
  ensureWifi([this] { loadShelf(currentView, 0); },
             entryMode == EntryMode::SEARCH ? std::function<void()>([this] { finish(); }) : nullptr);
}

void BookOrbitActivity::loadRootMenu() {
  currentView = ShelfView::ROOT_MENU;
  currentPage = 0;
  totalBooks = 0;
  headerTitleStr = "BookOrbit";

  {
    RenderLock lock(*this);
    books.clear();
    rowItems.clear();
    rowLabels.clear();
    rowSubtitles.clear();
    rowValues.clear();
    state = State::BROWSING;
    nav.reset();
  }
  requestUpdate();
}

void BookOrbitActivity::loadLibrariesList() {
  currentView = ShelfView::LIBRARIES_LIST;
  currentPage = 0;
  totalBooks = 0;
  headerTitleStr = "Libraries";

  if (libraries.empty()) {
    {
      RenderLock lock(*this);
      state = State::LOADING;
      statusMessage = "Loading libraries...";
    }
    requestUpdate(true);

    std::string err;
    if (!BookOrbitClient::fetchLibraries(libraries, err)) {
      {
        RenderLock lock(*this);
        errorMessage = err.empty() ? "Failed to load libraries" : err;
        state = State::ERROR;
      }
      requestUpdate();
      return;
    }
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

bool BookOrbitActivity::tryLoadLocalContinueReading() {
  std::vector<BookOrbitBook> localBooks;
  if (!BookOrbitClient::loadLocalBooks(localBooks) || localBooks.empty()) {
    return false;
  }
  currentView = ShelfView::CURRENTLY_READING;
  headerTitleStr = "Continue Reading";
  {
    RenderLock lock(*this);
    books = std::move(localBooks);
    totalBooks = static_cast<int>(books.size());
    rebuildRowItems();
    state = State::BROWSING;
    nav.reset();
  }
  requestUpdate();
  return true;
}

void BookOrbitActivity::loadShelf(const ShelfView view, const int page) {
  currentView = view;
  currentPage = page;

  switch (view) {
    case ShelfView::ROOT_MENU:
      loadRootMenu();
      return;
    case ShelfView::LIBRARIES_LIST:
      loadLibrariesList();
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
    case ShelfView::SERIES_BOOKS:
      headerTitleStr = activeSeriesName.empty() ? "Series" : activeSeriesName;
      break;
  }

  // When offline, browsing "Continue Reading" directly shows locally downloaded/reading books!
  if (view == ShelfView::CURRENTLY_READING && !wifiConnected()) {
    if (tryLoadLocalContinueReading()) {
      return;
    }
    ensureWifi([this] { loadShelf(ShelfView::CURRENTLY_READING, 0); },
               entryMode == EntryMode::CONTINUE_READING ? std::function<void()>([this] { finish(); }) : nullptr);
    return;
  }

  {
    RenderLock lock(*this);
    state = State::LOADING;
    statusMessage = (view == ShelfView::SERIES_BOOKS) ? "Loading series..." : "Fetching books...";
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
    case ShelfView::SERIES_BOOKS:
      shelfQuery = "series:" + std::to_string(activeSeriesId);
      break;
    case ShelfView::ROOT_MENU:
    case ShelfView::LIBRARIES_LIST:
      break;
  }

  std::string err;
  std::vector<BookOrbitBook> fetchedBooks;
  int fetchedTotal = 0;
  const bool ok = BookOrbitClient::fetchBooks(shelfQuery, fetchedBooks, err, page, PAGE_SIZE, &fetchedTotal);

  if (!ok) {
    if (view == ShelfView::CURRENTLY_READING) {
      std::vector<BookOrbitBook> localBooks;
      if (BookOrbitClient::loadLocalBooks(localBooks)) {
        RenderLock lock(*this);
        books = std::move(localBooks);
        totalBooks = static_cast<int>(books.size());
        rebuildRowItems();
        state = State::BROWSING;
        nav.reset();
        requestUpdate();
        return;
      }
    }
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
  rowSubtitles.clear();
  rowValues.clear();

  if (currentView == ShelfView::ROOT_MENU) {
    return;
  }

  auto addRow = [&](std::string label, std::string subtitle, std::string val, fui::BitmapRef icon) {
    const int16_t idx = static_cast<int16_t>(rowItems.size());
    rowLabels.push_back(std::move(label));
    rowSubtitles.push_back(std::move(subtitle));
    rowValues.push_back(std::move(val));
    fui::ListItem item{};
    item.label = rowLabels.back().c_str();
    item.subtitle = rowSubtitles.back().empty() ? nullptr : rowSubtitles.back().c_str();
    item.value = rowValues.back().empty() ? nullptr : rowValues.back().c_str();
    item.icon = icon;
    item.actionValue = idx;
    rowItems.push_back(item);
  };

  if (currentView == ShelfView::LIBRARIES_LIST) {
    rowItems.reserve(libraries.size());
    rowLabels.reserve(libraries.size());
    rowSubtitles.reserve(libraries.size());
    rowValues.reserve(libraries.size());

    for (const auto& lib : libraries) {
      std::string subtitle = std::to_string(lib.bookCount) + (lib.bookCount == 1 ? " book" : " books");
      addRow(lib.name, std::move(subtitle), ">", fui::bitmapFromIcon(icon_library_32));
    }
    return;
  }

  rowItems.reserve(books.size());
  rowLabels.reserve(books.size());
  rowSubtitles.reserve(books.size());
  rowValues.reserve(books.size());

  const bool inSeriesView = (currentView == ShelfView::SERIES_BOOKS);
  const bool showIndividualBooks = inSeriesView || (currentView == ShelfView::CURRENTLY_READING);

  for (size_t i = 0; i < books.size(); i++) {
    const auto& book = books[i];
    const bool isSeriesRow =
        !showIndividualBooks && (book.isCollapsedSeries || (book.seriesId > 0 && !book.seriesName.empty()));

    if (isSeriesRow) {
      std::string seriesTitle = !book.seriesName.empty() ? book.seriesName : book.title;
      std::string sub = "Series";
      if (book.seriesBookCount > 0) {
        sub += " \xC2\xB7 " + std::to_string(book.seriesBookCount) + (book.seriesBookCount == 1 ? " book" : " books");
      }
      if (!book.author.empty()) {
        sub += " \xC2\xB7 " + book.author;
      }
      std::string val = book.seriesBookCount > 0 ? (std::to_string(book.seriesBookCount) + " >") : "Series >";
      addRow(std::move(seriesTitle), std::move(sub), std::move(val), fui::bitmapFromIcon(icon_library_32));
      continue;
    }

    std::string label = book.title;
    if (inSeriesView && !book.seriesIndex.empty()) {
      label = "#" + book.seriesIndex + " " + book.title;
    }

    std::string subtitle = book.author;
    if (!inSeriesView && !book.seriesName.empty()) {
      if (!subtitle.empty()) subtitle += " \xC2\xB7 ";
      subtitle += book.seriesName;
      if (!book.seriesIndex.empty()) {
        subtitle += " #" + book.seriesIndex;
      }
    }

    std::string val;
    if (book.isDownloaded) {
      val = "Saved";
    }
    if (book.progress > 0.0f) {
      char pct[16];
      snprintf(pct, sizeof(pct), "%.0f%%", book.progress * 100.0f);
      if (!val.empty()) val += " ";
      val += pct;
    }
    fui::BitmapRef icon = fui::bitmapFromIcon(book.isDownloaded ? icon_book_open_32 : icon_book_32);
    addRow(std::move(label), std::move(subtitle), std::move(val), icon);
  }
}

void BookOrbitActivity::buildHomeTilesScreen(UiScreen& screen) {
  const auto& theme = screen.theme();
  screen.spacer(theme.spaceMd);
  screen.insetContent(fui::Insets{0, 16, 12, 16});

  struct TileDef {
    std::string title;
    std::string subtitle;
    fui::BitmapRef icon;
  };

  std::vector<TileDef> tiles;
  tiles.reserve(3);

  char libSubtitle[64];
  if (!libraries.empty()) {
    snprintf(libSubtitle, sizeof(libSubtitle), "%zu allowed %s", libraries.size(),
             libraries.size() == 1 ? "library" : "libraries");
  } else {
    snprintf(libSubtitle, sizeof(libSubtitle), "Browse your libraries");
  }

  char syncSubtitle[64] = "2-way sync with server";
  if (lastSyncPulledCount >= 0 || lastSyncPushedCount >= 0) {
    if (lastSyncPulledCount == 0 && lastSyncPushedCount == 0) {
      snprintf(syncSubtitle, sizeof(syncSubtitle), "All books up to date");
    } else {
      snprintf(syncSubtitle, sizeof(syncSubtitle), "Synced (%d pushed, %d updated)", std::max(0, lastSyncPushedCount),
               std::max(0, lastSyncPulledCount));
    }
  }

  tiles.push_back({"Search", "Find books, authors & series", fui::bitmapFromIcon(icon_search_32)});
  tiles.push_back({"Libraries", libSubtitle, fui::bitmapFromIcon(icon_library_32)});
  tiles.push_back({"Sync Progress", syncSubtitle, fui::bitmapFromIcon(icon_refresh_cw_32)});

  const int numTiles = static_cast<int>(tiles.size());
  const fui::Rect area = screen.body();
  const int16_t gap = 12;
  const int16_t rawTileH = static_cast<int16_t>((area.height - (numTiles - 1) * gap) / numTiles);
  const int16_t tileH = std::clamp<int16_t>(rawTileH, 76, 110);
  const int16_t totalH = static_cast<int16_t>(numTiles * tileH + (numTiles - 1) * gap);
  const int16_t startY = static_cast<int16_t>(area.y + std::max<int16_t>(0, (area.height - totalH) / 2));

  const int16_t titleLh = screen.target().lineHeight(theme.titleText.font);
  const int16_t subLh = screen.target().lineHeight(theme.smallText.font);
  const int16_t textGap = 6;
  const int16_t textBlockH = static_cast<int16_t>(titleLh + textGap + subLh);

  for (int i = 0; i < numTiles; ++i) {
    const fui::Rect card{area.x, static_cast<int16_t>(startY + i * (tileH + gap)), area.width, tileH};
    const bool selected = (nav.selected == i);

    // Register touch & button hit area for this tile
    screen.frame().hit(card, ACTION_ROW, static_cast<int16_t>(i), fui::InputTouch);
    const fui::State st =
        screen.frame().stateFor(ACTION_ROW, static_cast<int16_t>(i), selected ? fui::StateSelected : fui::StateNormal);
    const bool inverted = fui::hasState(st, fui::StateSelected) || fui::hasState(st, fui::StateActive) ||
                          fui::hasState(st, fui::StateFocused);

    const fui::Paint bg = fui::Paint::solid(inverted ? fui::Color::Black : fui::Color::White);
    const fui::Paint fg = fui::Paint::solid(inverted ? fui::Color::White : fui::Color::Black);
    const uint8_t radius = theme.controlRadius > 0 ? theme.controlRadius : 14;

    screen.target().fill(card, bg, radius);
    screen.target().stroke(card, fui::Paint::solid(fui::Color::Black), 2, radius);

    // Leading 32x32 icon badge
    const int16_t iconSize = 32;
    const int16_t iconX = static_cast<int16_t>(card.x + 22);
    const int16_t iconY = static_cast<int16_t>(card.y + (card.height - iconSize) / 2);
    screen.target().bitmap(fui::Rect{iconX, iconY, iconSize, iconSize}, tiles[i].icon, fui::BitmapMode::Center, fg);

    // Trailing chevron icon
    const int16_t chevSize = 24;
    const int16_t chevX = static_cast<int16_t>(card.right() - 18 - chevSize);
    const int16_t chevY = static_cast<int16_t>(card.y + (card.height - chevSize) / 2);
    screen.target().bitmap(fui::Rect{chevX, chevY, chevSize, chevSize}, fui::bitmapFromIcon(icon_page_next_24),
                           fui::BitmapMode::Center, fg);

    // Title + subtitle text column
    const int16_t textX = static_cast<int16_t>(iconX + iconSize + 18);
    const int16_t textW = static_cast<int16_t>(chevX - textX - 10);
    const int16_t textY = static_cast<int16_t>(card.y + (card.height - textBlockH) / 2);

    fui::TextStyle titleStyle = fui::textStyleWithForeground(theme.titleText, fg);
    titleStyle.bold = true;
    titleStyle.align = fui::TextAlign::Left;
    titleStyle.maxLines = 1;
    screen.target().text(fui::Rect{textX, textY, textW, titleLh}, tiles[i].title.c_str(), titleStyle);

    fui::TextStyle subStyle = fui::textStyleWithForeground(theme.smallText, fg);
    subStyle.align = fui::TextAlign::Left;
    subStyle.maxLines = 1;
    screen.target().text(fui::Rect{textX, static_cast<int16_t>(textY + titleLh + textGap), textW, subLh},
                         tiles[i].subtitle.c_str(), subStyle);
  }
}

void BookOrbitActivity::buildPaginationBar(UiScreen& screen) {
  const int pages = totalPages();
  if (pages <= 1) return;

  const bool hasPrev = currentPage > 0;
  const bool hasNext = (currentPage + 1) < pages;

  fui::TabItem left[2], right[2];
  int lc = 0, rc = 0;
  const auto addTab = [](fui::TabItem* tabs, int& count, const freeink::Icon& icon, const int16_t value, bool enabled) {
    tabs[count].icon = fui::bitmapFromIcon(icon);
    tabs[count].value = value;
    tabs[count].enabled = enabled;
    ++count;
  };

  addTab(left, lc, icon_page_first_32, PAGE_FIRST, hasPrev);
  addTab(left, lc, icon_page_prev_32, PAGE_PREV, hasPrev);
  addTab(right, rc, icon_page_next_32, PAGE_NEXT, hasNext);
  addTab(right, rc, icon_page_last_32, PAGE_LAST, hasNext);

  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect band = screen.takeBottom(static_cast<int16_t>(metrics.tabBarHeight));
  const fui::Rect frameRect = screen.frame().screen();
  const fui::Rect barRect{frameRect.x, band.y, frameRect.width, band.height};
  screen.target().fill(fui::Rect{barRect.x, barRect.y, barRect.width, 1}, fui::Paint::solid(fui::Color::Black));

  const auto side = static_cast<int16_t>(metrics.contentSidePadding);
  const int16_t zoneW = static_cast<int16_t>((barRect.width - 2 * side) / 3);
  const int16_t slotY = static_cast<int16_t>(barRect.y + 1);
  const int16_t slotH = static_cast<int16_t>(barRect.height - 1);
  fui::TabBarProps props;
  props.action = ACTION_PAGE;
  props.inputMask = fui::InputTouch;
  props.iconSize = 32;
  props.tabs = left;
  props.count = static_cast<uint16_t>(lc);
  fui::tabBar(screen.frame(), fui::Rect{static_cast<int16_t>(barRect.x + side), slotY, zoneW, slotH}, props);
  props.tabs = right;
  props.count = static_cast<uint16_t>(rc);
  fui::tabBar(screen.frame(), fui::Rect{static_cast<int16_t>(barRect.right() - side - zoneW), slotY, zoneW, slotH},
              props);

  char pageText[32];
  snprintf(pageText, sizeof(pageText), "%d / %d", currentPage + 1, pages);
  fui::TextStyle centered = screen.theme().smallText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  screen.target().text(fui::Rect{static_cast<int16_t>(barRect.x + side + zoneW),
                                 static_cast<int16_t>(slotY + (slotH - lh) / 2), zoneW, lh},
                       pageText, centered);
}

void BookOrbitActivity::buildBrowsingScreen(UiScreen& screen) {
  if (currentView == ShelfView::ROOT_MENU) {
    buildHomeTilesScreen(screen);
    return;
  }

  if (isPaginatedView() && totalPages() > 1) {
    buildPaginationBar(screen);
  }

  if (rowItems.empty()) {
    const char* emptyMsg = (currentView == ShelfView::LIBRARIES_LIST) ? "No libraries found" : "No EPUB books found";
    screen.centeredText(emptyMsg, screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 1;
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
  if (index < 0 || index >= listCount()) return;

  if (currentView == ShelfView::ROOT_MENU) {
    const int actionIdx = index;
    if (actionIdx == 0) {
      ensureWifi([this] { launchSearch(); });
    } else if (actionIdx == 1) {
      ensureWifi([this] { loadLibrariesList(); });
    } else if (actionIdx == 2) {
      ensureWifi([this] { startProgressSync(); });
    }
    return;
  }

  if (currentView == ShelfView::LIBRARIES_LIST) {
    if (index >= 0 && static_cast<size_t>(index) < libraries.size()) {
      activeLibraryId = libraries[index].id;
      activeLibraryName = libraries[index].name;
      loadShelf(ShelfView::LIBRARY_BOOKS, 0);
    }
    return;
  }

  if (index >= 0 && index < static_cast<int>(books.size())) {
    handleBookSelected(static_cast<size_t>(index));
  }
}

void BookOrbitActivity::startProgressSync() {
  if (!BOOKORBIT_STORE.isConfigured()) {
    RenderLock lock(*this);
    errorMessage = "BookOrbit is not configured";
    state = State::ERROR;
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::LOADING;
    statusMessage = "Syncing reading progress...";
  }
  requestUpdate();

  int pushed = 0, pulled = 0;
  BookOrbitClient::syncAllProgress(&pushed, &pulled);
  lastSyncPushedCount = pushed;
  lastSyncPulledCount = pulled;

  if (entryMode == EntryMode::CONTINUE_READING) {
    loadShelf(ShelfView::CURRENTLY_READING, 0);
    return;
  }
  if (entryMode == EntryMode::SYNC_ONLY) {
    finishSyncDialog();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::BROWSING;
    currentView = ShelfView::ROOT_MENU;
  }
  requestUpdate();
}

void BookOrbitActivity::finishSyncDialog() {
  char msg[96];
  if (lastSyncPushedCount <= 0 && lastSyncPulledCount <= 0) {
    snprintf(msg, sizeof(msg), "All books are up to date.");
  } else {
    snprintf(msg, sizeof(msg), "Synced (%d pushed, %d updated)", std::max(0, lastSyncPushedCount),
             std::max(0, lastSyncPulledCount));
  }
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, "BookOrbit Sync", msg, "", "OK");
  if (!dialog) {
    finish();
    return;
  }
  state = State::DONE;
  startActivityForResult(std::move(dialog), [this](const ActivityResult&) { finish(); });
}

// Leaving this Wi-Fi activity normally triggers a silent restart to Home (heap defrag),
// which would swallow the reader launch. Route through the reboot-to-reader path instead:
// it reboots on a clean heap straight into the book. Touch boards skip the reboot and fall
// through to the in-place launch.
void BookOrbitActivity::openBook(const std::string& path) {
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  BookOrbitClient::skipNextOpenSync();  // BookOrbit just had its chance to sync (or the user skipped Wi-Fi)
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    silentRestartToReader();
  } else {
    activityManager.goToReader(path);
  }
}

void BookOrbitActivity::handleBookSelected(size_t index) {
  if (index >= books.size()) return;
  auto& book = books[index];

  // If this row represents a smushed series (and we aren't in Continue Reading or inside SERIES_BOOKS),
  // open the series to browse its individual books in series order!
  const bool showIndividualBooks =
      (currentView == ShelfView::SERIES_BOOKS) || (currentView == ShelfView::CURRENTLY_READING);
  const bool isSeriesRow =
      !showIndividualBooks && (book.isCollapsedSeries || (book.seriesId > 0 && !book.seriesName.empty()));
  if (isSeriesRow && book.seriesId > 0) {
    parentBeforeSeriesView = currentView;
    parentPageBeforeSeries = currentPage;
    activeSeriesId = book.seriesId;
    activeSeriesName = !book.seriesName.empty() ? book.seriesName : book.title;
    loadShelf(ShelfView::SERIES_BOOKS, 0);
    return;
  }

  // If already downloaded, immediately launch into reader!
  if (book.isDownloaded && !book.localFilePath.empty()) {
    openBook(book.localFilePath);
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
    openBook(book.localFilePath);
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
      const char* confirmLabel =
          (currentView == ShelfView::ROOT_MENU || currentView == ShelfView::LIBRARIES_LIST) ? "Open" : "Select";
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
