#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "BookOrbitStore.h"
#include "network/HttpDownloader.h"

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
};

class BookOrbitClient {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  // Validates connection and credentials against BookOrbit server (/api/v1/auth/me or health)
  static bool testConnection(std::string& outErrorMessage);

  // Fetches reading list / books from a shelf
  static bool fetchBooks(const std::string& shelfOrEndpoint, std::vector<BookOrbitBook>& outBooks,
                         std::string& outErrorMessage);

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

  // SD download directory for BookOrbit books
  static std::string getDownloadDir();
};
