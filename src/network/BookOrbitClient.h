#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "BookOrbitStore.h"
#include "network/HttpDownloader.h"

struct BookOrbitLibrary {
  int id = 0;
  std::string name;
  int bookCount = 0;
};

struct BookOrbitBook {
  std::string id;
  std::string fileId;
  std::string title;
  std::string author;
  std::string description;
  std::string coverUrl;
  float progress = 0.0f;
  bool isDownloaded = false;
  std::string localFilePath;

  // Series metadata & collapsed series ("smushed" series) info
  int seriesId = 0;
  std::string seriesName;
  std::string seriesIndex;
  bool isCollapsedSeries = false;
  int seriesBookCount = 0;
  int seriesReadCount = 0;
};

class BookOrbitClient {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  // Obtains a valid Bearer token (uses permanent API key if bok_/bo_, or logs in with username + password)
  static bool ensureAuthenticated(std::string& outBearerToken, std::string& outErrorMessage, bool forceRefresh = false);

  // Validates connection and credentials against BookOrbit server (/api/v1/auth/me)
  static bool testConnection(std::string& outErrorMessage);

  // Fetches available libraries on the BookOrbit server
  static bool fetchLibraries(std::vector<BookOrbitLibrary>& outLibraries, std::string& outErrorMessage);

  // Fetches reading list / books from a shelf, library, search, or series ("currently-reading", "want-to-read",
  // "recently-added", "library:<id>", "search:<q>", "series:<id>", or "")
  static bool fetchBooks(const std::string& shelfOrEndpoint, std::vector<BookOrbitBook>& outBooks,
                         std::string& outErrorMessage, int page = 0, int pageSize = 25, int* outTotal = nullptr);

  // Helper to fetch "currently-reading" books
  static bool fetchCurrentlyReading(std::vector<BookOrbitBook>& outBooks, std::string& outErrorMessage);

  // Downloads book EPUB from BookOrbit to SD storage
  static HttpDownloader::DownloadError downloadBook(BookOrbitBook& book, ProgressCallback progressCb,
                                                    const bool* cancelFlag, std::string& outErrorMessage);

  // Syncs reading progress back to BookOrbit
  static bool syncReadingProgress(const std::string& fileId, float progressPercent, int currentPosition = 0,
                                  int durationSeconds = 0);

  // Checks if a book exists on local storage
  static bool checkLocalAvailability(BookOrbitBook& book);

  // Checks if a book has BookOrbit metadata sidecar (.meta.json)
  static bool hasBookMetadata(const std::string& bookPath);

  // Reads sidecar metadata (.meta.json)
  static bool getBookMetadata(const std::string& bookPath, std::string& outFileId, std::string& outBookId,
                              float* outProgress = nullptr);

  // Records current book progress to sidecar and attempts to sync if online
  static void recordBookProgress(const std::string& bookPath, float progressRatio);

  // Updates local sidecar metadata with server progress if newer
  static void updateLocalSidecarProgress(const std::string& bookPath, float serverRatio);

  // Pulls updated reading progress from server for all locally downloaded books
  static bool pullRemoteProgress(int* outUpdatedCount = nullptr);

  // Performs full 2-way progress sync: pushes dirty local progress and pulls latest server progress
  static bool syncAllProgress(int* outPushedCount = nullptr, int* outPulledCount = nullptr);

  // Checks and consumes pending remote progress for a book upon opening
  static bool checkAndConsumeRemoteProgress(const std::string& bookPath, float& outProgress);

  // Scans BookOrbit download directory and pushes any pending progress syncs
  static void syncPendingProgress();

  // Loads all downloaded and in-progress books from local storage (/books/BookOrbit)
  static bool loadLocalBooks(std::vector<BookOrbitBook>& outBooks);

  // SD download directory for BookOrbit books
  static std::string getDownloadDir();
};
