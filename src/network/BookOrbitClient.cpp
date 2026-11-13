#include "BookOrbitClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "util/BookProgress.h"
#include "util/OpdsFilename.h"

namespace {

std::string g_cachedJwt;
unsigned long g_jwtFetchedAtMs = 0;
constexpr unsigned long JWT_CACHE_TTL_MS = 10UL * 60UL * 1000UL;  // 10 minutes (server TTL is 15m)

std::string cleanBaseUrl(const std::string& raw) {
  std::string url = raw;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

std::string jsonVariantToIdString(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? s : "";
  }
  if (v.is<long>()) {
    return std::to_string(v.as<long>());
  }
  if (v.is<int>()) {
    return std::to_string(v.as<int>());
  }
  return "";
}

void parseBookArray(JsonArrayConst array, std::vector<BookOrbitBook>& outBooks) {
  for (JsonObjectConst item : array) {
    BookOrbitBook book;
    book.id = jsonVariantToIdString(item["id"]);
    book.title = item["title"] | "";
    if (book.title.empty()) continue;

    if (item["author"].is<const char*>()) {
      book.author = item["author"].as<const char*>();
    } else if (item["authors"].is<JsonArrayConst>() && item["authors"].as<JsonArrayConst>().size() > 0) {
      JsonVariantConst firstAuthor = item["authors"][0];
      if (firstAuthor.is<const char*>()) {
        book.author = firstAuthor.as<const char*>();
      } else if (firstAuthor.is<JsonObjectConst>()) {
        book.author = firstAuthor["name"] | "";
      }
    }

    book.description = item["description"] | "";

    if (item["seriesId"].is<int>()) {
      book.seriesId = item["seriesId"].as<int>();
    }
    book.seriesName = item["seriesName"] | "";
    book.seriesIndex = jsonVariantToIdString(item["seriesIndex"]);

    if (item["collapsedSeries"].is<JsonObjectConst>()) {
      JsonObjectConst cs = item["collapsedSeries"].as<JsonObjectConst>();
      book.isCollapsedSeries = (book.seriesId > 0);
      book.seriesBookCount = cs["bookCount"] | 1;
      book.seriesReadCount = cs["readCount"] | 0;
    }

    // BookOrbit readingProgress can be a float (0..100 or 0..1) or an object { percentage: 0..100 }
    if (item["readingProgress"].is<float>()) {
      float p = item["readingProgress"].as<float>();
      book.progress = (p > 1.0f) ? (p / 100.0f) : p;
    } else if (item["readingProgress"].is<JsonObjectConst>() && item["readingProgress"]["percentage"].is<float>()) {
      float p = item["readingProgress"]["percentage"].as<float>();
      book.progress = (p > 1.0f) ? (p / 100.0f) : p;
    } else if (item["progress"].is<float>()) {
      float p = item["progress"].as<float>();
      book.progress = (p > 1.0f) ? (p / 100.0f) : p;
    } else if (item["userProgress"]["progress"].is<float>()) {
      float p = item["userProgress"]["progress"].as<float>();
      book.progress = (p > 1.0f) ? (p / 100.0f) : p;
    }

    // Prefer EPUB file; only include books that have an EPUB or XTCH/XTC or fallback primary
    std::string fallbackFileId;
    if (item["files"].is<JsonArrayConst>()) {
      for (JsonObjectConst f : item["files"].as<JsonArrayConst>()) {
        const std::string fmt = f["format"] | "";
        const std::string fId = jsonVariantToIdString(f["id"]);
        if (fId.empty()) continue;
        if (fmt == "epub" || fmt == "EPUB") {
          book.fileId = fId;
          break;
        }
        if (fallbackFileId.empty() && fmt != "jpg" && fmt != "png" && fmt != "opf" && fmt != "webp") {
          fallbackFileId = fId;
        }
      }
    }
    if (book.fileId.empty()) {
      // Skip non-EPUB entries (e.g. sheet music PDFs) if they have files but none are EPUB
      if (item["files"].is<JsonArrayConst>() && item["files"].as<JsonArrayConst>().size() > 0) {
        continue;
      }
      book.fileId = fallbackFileId;
    }

    BookOrbitClient::checkLocalAvailability(book);
    outBooks.push_back(std::move(book));
  }
}

}  // namespace

std::string BookOrbitClient::getDownloadDir() { return "/books/BookOrbit"; }

bool BookOrbitClient::checkLocalAvailability(BookOrbitBook& book) {
  const std::string folder = getDownloadDir();
  const auto format = static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat);
  const std::string filename = folder + "/" + opdsBookFilename(book.author, book.title, format);

  if (Storage.exists(filename.c_str())) {
    book.isDownloaded = true;
    book.localFilePath = filename;
    // If book is downloaded and server returned valid progress, update local sidecar if newer
    if (book.progress > 0.0f) {
      updateLocalSidecarProgress(filename, book.progress);
    } else {
      // Otherwise read local sidecar progress if server progress is 0
      std::string fId, bId;
      float localProg = 0.0f;
      if (getBookMetadata(filename, fId, bId, &localProg) && localProg > 0.0f) {
        book.progress = localProg;
      }
    }
    return true;
  }

  book.isDownloaded = false;
  book.localFilePath.clear();
  return false;
}

bool BookOrbitClient::ensureAuthenticated(std::string& outBearerToken, std::string& outErrorMessage,
                                          const bool forceRefresh) {
  if (!BOOKORBIT_STORE.isConfigured()) {
    outErrorMessage = "BookOrbit is not configured";
    return false;
  }

  const std::string& keyOrPass = BOOKORBIT_STORE.getApiKey();
  const std::string& username = BOOKORBIT_STORE.getUsername();

  // If the stored credential is a BookOrbit API key (starts with bok_ or bo_) or no username is set,
  // use it directly as the Bearer token.
  const bool isApiKey = (keyOrPass.rfind("bok_", 0) == 0) || (keyOrPass.rfind("bo_", 0) == 0) || username.empty();
  if (isApiKey) {
    outBearerToken = keyOrPass;
    return true;
  }

  // Otherwise, keyOrPass is the user's password; check cached JWT first.
  const unsigned long now = millis();
  if (!forceRefresh && !g_cachedJwt.empty() && (now - g_jwtFetchedAtMs < JWT_CACHE_TTL_MS)) {
    outBearerToken = g_cachedJwt;
    return true;
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/auth/login";
  JsonDocument reqDoc;
  reqDoc["username"] = username;
  reqDoc["password"] = keyOrPass;
  std::string reqBody;
  serializeJson(reqDoc, reqBody);

  std::string respBody;
  int status = 0;
  const bool ok = HttpDownloader::postJson(url, reqBody, respBody, "", &status);
  if (!ok || status < 200 || status >= 300) {
    outErrorMessage = "Login failed (HTTP " + std::to_string(status) + ")";
    LOG_ERR("BKO", "Login failed for user %s (HTTP %d)", username.c_str(), status);
    return false;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, respBody) || !respDoc["accessToken"].is<const char*>()) {
    outErrorMessage = "Invalid login response from BookOrbit";
    return false;
  }

  g_cachedJwt = respDoc["accessToken"].as<const char*>();
  g_jwtFetchedAtMs = millis();
  outBearerToken = g_cachedJwt;
  return true;
}

bool BookOrbitClient::testConnection(std::string& outErrorMessage) {
  std::string token;
  if (!ensureAuthenticated(token, outErrorMessage, true)) {
    return false;
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/auth/me";
  HttpDownloader::FetchOptions opts;
  opts.bearer = token;
  int status = 0;
  opts.statusOut = &status;

  std::string responseBody;
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&responseBody](const uint8_t* data, size_t len) {
        if (responseBody.size() + len < 2048) {
          responseBody.append(reinterpret_cast<const char*>(data), len);
        }
        return true;
      },
      opts);

  if (!ok || status < 200 || status >= 300) {
    outErrorMessage = "Connection failed (HTTP " + std::to_string(status) + ")";
    LOG_ERR("BKO", "testConnection failed: status %d", status);
    return false;
  }

  return true;
}

bool BookOrbitClient::fetchLibraries(std::vector<BookOrbitLibrary>& outLibraries, std::string& outErrorMessage) {
  outLibraries.clear();

  std::string token;
  if (!ensureAuthenticated(token, outErrorMessage)) {
    return false;
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/libraries";
  HttpDownloader::FetchOptions opts;
  opts.bearer = token;
  int status = 0;
  opts.statusOut = &status;

  std::string jsonPayload;
  jsonPayload.reserve(4096);
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&jsonPayload](const uint8_t* data, size_t len) {
        if (jsonPayload.size() + len <= 16384) {
          jsonPayload.append(reinterpret_cast<const char*>(data), len);
          return true;
        }
        return false;
      },
      opts);

  if (!ok || status < 200 || status >= 300) {
    outErrorMessage = "Failed to load libraries (HTTP " + std::to_string(status) + ")";
    return false;
  }

  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["name"] = true;
  filter[0]["bookCount"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, jsonPayload, DeserializationOption::Filter(filter)) || !doc.is<JsonArray>()) {
    outErrorMessage = "Invalid libraries response";
    return false;
  }

  for (JsonObjectConst obj : doc.as<JsonArrayConst>()) {
    BookOrbitLibrary lib;
    lib.id = obj["id"] | 0;
    lib.name = obj["name"] | "";
    lib.bookCount = obj["bookCount"] | 0;
    if (lib.id > 0 && !lib.name.empty()) {
      outLibraries.push_back(std::move(lib));
    }
  }
  return true;
}

bool BookOrbitClient::fetchCurrentlyReading(std::vector<BookOrbitBook>& outBooks, std::string& outErrorMessage) {
  return fetchBooks("currently-reading", outBooks, outErrorMessage);
}

bool BookOrbitClient::fetchBooks(const std::string& shelfOrEndpoint, std::vector<BookOrbitBook>& outBooks,
                                 std::string& outErrorMessage, const int page, const int pageSize, int* outTotal) {
  outBooks.clear();
  if (outTotal) *outTotal = 0;

  std::string token;
  if (!ensureAuthenticated(token, outErrorMessage)) {
    return false;
  }

  const std::string baseUrl = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl());
  std::string url;
  std::string postBody;

  const bool isScroller = (shelfOrEndpoint == "currently-reading" || shelfOrEndpoint == "continue-reading" ||
                           shelfOrEndpoint == "want-to-read" || shelfOrEndpoint == "recently-added");
  const bool isSeries = (shelfOrEndpoint.rfind("series:", 0) == 0);

  if (isScroller) {
    url = baseUrl + "/api/v1/dashboard/scrollers/batch";
    const std::string scrollerType = (shelfOrEndpoint == "currently-reading") ? "continue-reading" : shelfOrEndpoint;
    JsonDocument reqDoc;
    JsonObject item = reqDoc["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = "1";
    item["type"] = scrollerType;
    item["limit"] = pageSize;
    serializeJson(reqDoc, postBody);
  } else if (isSeries) {
    const std::string seriesId = shelfOrEndpoint.substr(7);
    url = baseUrl + "/api/v1/series/" + seriesId + "/books?page=" + std::to_string(page) +
          "&size=" + std::to_string(pageSize);
  } else if (shelfOrEndpoint.rfind("library:", 0) == 0) {
    const std::string libId = shelfOrEndpoint.substr(8);
    url = baseUrl + "/api/v1/libraries/" + libId + "/books";
    JsonDocument reqDoc;
    reqDoc["collapseSeries"] = true;
    reqDoc["collapseLinked"] = true;
    JsonObject sortObj = reqDoc["sort"].to<JsonArray>().add<JsonObject>();
    sortObj["field"] = "title";
    sortObj["dir"] = "asc";
    reqDoc["pagination"]["page"] = page;
    reqDoc["pagination"]["size"] = pageSize;
    serializeJson(reqDoc, postBody);
  } else {
    url = baseUrl + "/api/v1/books/query";
    JsonDocument reqDoc;
    if (shelfOrEndpoint.rfind("search:", 0) == 0) {
      reqDoc["q"] = shelfOrEndpoint.substr(7);
    } else if (shelfOrEndpoint.rfind("/api/v1/books?q=", 0) == 0) {
      reqDoc["q"] = shelfOrEndpoint.substr(16);
    }
    reqDoc["collapseSeries"] = true;
    reqDoc["collapseLinked"] = true;
    JsonObject sortObj = reqDoc["sort"].to<JsonArray>().add<JsonObject>();
    sortObj["field"] = "title";
    sortObj["dir"] = "asc";
    reqDoc["pagination"]["page"] = page;
    reqDoc["pagination"]["size"] = pageSize;
    serializeJson(reqDoc, postBody);
  }

  std::string jsonPayload;
  constexpr size_t MAX_PAYLOAD_SIZE = 48 * 1024;
  jsonPayload.reserve(16384);

  int status = 0;
  bool ok = false;
  if (isSeries) {
    HttpDownloader::FetchOptions opts;
    opts.bearer = token;
    opts.statusOut = &status;
    ok = HttpDownloader::fetchUrl(
        url,
        [&jsonPayload](const uint8_t* data, size_t len) {
          if (jsonPayload.size() + len <= MAX_PAYLOAD_SIZE) {
            jsonPayload.append(reinterpret_cast<const char*>(data), len);
            return true;
          }
          return false;
        },
        opts);
    if (status == 401 && ensureAuthenticated(token, outErrorMessage, true)) {
      jsonPayload.clear();
      opts.bearer = token;
      ok = HttpDownloader::fetchUrl(
          url,
          [&jsonPayload](const uint8_t* data, size_t len) {
            if (jsonPayload.size() + len <= MAX_PAYLOAD_SIZE) {
              jsonPayload.append(reinterpret_cast<const char*>(data), len);
              return true;
            }
            return false;
          },
          opts);
    }
  } else {
    ok = HttpDownloader::postJson(url, postBody, jsonPayload, token, &status, MAX_PAYLOAD_SIZE);
    if (status == 401) {
      // Token may have expired; force refresh and retry once
      if (ensureAuthenticated(token, outErrorMessage, true)) {
        jsonPayload.clear();
        ok = HttpDownloader::postJson(url, postBody, jsonPayload, token, &status, MAX_PAYLOAD_SIZE);
      }
    }
  }

  if (!ok || status < 200 || status >= 300) {
    outErrorMessage = "Failed to fetch books (HTTP " + std::to_string(status) + ")";
    LOG_ERR("BKO", "fetchBooks failed for %s (status %d)", url.c_str(), status);
    return false;
  }

  // Build filter to keep ArduinoJson heap usage small
  JsonDocument filter;
  if (isScroller) {
    JsonObject bookFilter = filter["items"][0]["books"][0].to<JsonObject>();
    bookFilter["id"] = true;
    bookFilter["title"] = true;
    bookFilter["authors"] = true;
    bookFilter["seriesId"] = true;
    bookFilter["seriesName"] = true;
    bookFilter["seriesIndex"] = true;
    bookFilter["readingProgress"] = true;
    bookFilter["files"][0]["id"] = true;
    bookFilter["files"][0]["format"] = true;
  } else {
    filter["total"] = true;
    JsonObject bookFilter = filter["items"][0].to<JsonObject>();
    bookFilter["id"] = true;
    bookFilter["title"] = true;
    bookFilter["authors"] = true;
    bookFilter["seriesId"] = true;
    bookFilter["seriesName"] = true;
    bookFilter["seriesIndex"] = true;
    bookFilter["collapsedSeries"]["bookCount"] = true;
    bookFilter["collapsedSeries"]["readCount"] = true;
    bookFilter["readingProgress"] = true;
    bookFilter["files"][0]["id"] = true;
    bookFilter["files"][0]["format"] = true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonPayload, DeserializationOption::Filter(filter));
  if (err && err != DeserializationError::NoMemory) {
    LOG_ERR("BKO", "JSON parse error in fetchBooks: %s", err.c_str());
    outErrorMessage = "Failed to parse book list";
    return false;
  }

  if (isScroller) {
    JsonArrayConst scrollers = doc["items"].as<JsonArrayConst>();
    if (!scrollers.isNull() && scrollers.size() > 0) {
      JsonArrayConst booksArr = scrollers[0]["books"].as<JsonArrayConst>();
      if (!booksArr.isNull()) {
        std::vector<BookOrbitBook> rawBooks;
        parseBookArray(booksArr, rawBooks);
        // Keep Continue Reading as individual books so clicking opens the book directly;
        // smush series only for other scroller shelves (want-to-read, recently-added).
        const bool smushSeries = (shelfOrEndpoint != "continue-reading" && shelfOrEndpoint != "currently-reading");
        if (!smushSeries) {
          outBooks = std::move(rawBooks);
        } else {
          for (auto& b : rawBooks) {
            if (b.seriesId > 0 && !b.seriesName.empty()) {
              bool merged = false;
              for (auto& existing : outBooks) {
                if (existing.seriesId == b.seriesId) {
                  existing.isCollapsedSeries = true;
                  existing.seriesBookCount++;
                  merged = true;
                  break;
                }
              }
              if (!merged) {
                b.isCollapsedSeries = true;
                b.seriesBookCount = 1;
                outBooks.push_back(std::move(b));
              }
            } else {
              outBooks.push_back(std::move(b));
            }
          }
        }
      }
    }
    if (outTotal) *outTotal = static_cast<int>(outBooks.size());
  } else {
    if (outTotal && doc["total"].is<int>()) {
      *outTotal = doc["total"].as<int>();
    }
    JsonArrayConst booksArr = doc["items"].as<JsonArrayConst>();
    if (!booksArr.isNull()) {
      parseBookArray(booksArr, outBooks);
    }
  }

  LOG_DBG("BKO", "Fetched %zu books from %s", outBooks.size(), shelfOrEndpoint.c_str());
  return true;
}

HttpDownloader::DownloadError BookOrbitClient::downloadBook(BookOrbitBook& book, ProgressCallback progressCb,
                                                            const bool* cancelFlag, std::string& outErrorMessage) {
  std::string token;
  if (!ensureAuthenticated(token, outErrorMessage)) {
    return HttpDownloader::HTTP_ERROR;
  }

  const std::string folder = getDownloadDir();
  if (!Storage.exists(folder.c_str())) {
    Storage.mkdir(folder.c_str());
  }

  const auto format = static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat);
  const std::string filename = folder + "/" + opdsBookFilename(book.author, book.title, format);

  std::string downloadEndpoint;
  if (!book.fileId.empty()) {
    downloadEndpoint = "/api/v1/books/files/" + book.fileId + "/download";
  } else {
    downloadEndpoint = "/api/v1/books/" + book.id + "/download";
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + downloadEndpoint;

  std::vector<HttpDownloader::Header> headers;
  headers.emplace_back("Authorization", "Bearer " + token);

  const auto err = HttpDownloader::downloadToFile(url, filename, progressCb, cancelFlag, "", "", headers);

  if (err != HttpDownloader::OK) {
    outErrorMessage = "Download failed (" + std::to_string(err) + ")";
    return err;
  }

  // Validate EPUB magic header ('P', 'K', 3, 4)
  uint8_t magic[4] = {0};
  {
    HalFile check;
    if (Storage.openFileForRead("BKO", filename.c_str(), check)) {
      check.read(magic, sizeof(magic));
      check.close();
    }
  }

  if (!(magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4)) {
    LOG_ERR("BKO", "Downloaded file is not a valid EPUB");
    Storage.remove(filename.c_str());
    outErrorMessage = "File received is not a valid EPUB";
    return HttpDownloader::FILE_ERROR;
  }

  // Write sidecar metadata (.meta.json)
  const std::string metaPath = filename + ".meta.json";
  JsonDocument metaDoc;
  metaDoc["title"] = book.title;
  metaDoc["author"] = book.author;
  metaDoc["bookorbit"]["bookId"] = book.id;
  metaDoc["bookorbit"]["fileId"] = book.fileId;
  metaDoc["bookorbit"]["serverUrl"] = BOOKORBIT_STORE.getServerUrl();
  metaDoc["progress"] = book.progress;
  metaDoc["pendingSync"] = false;

  HalFile metaFile;
  if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
    serializeJson(metaDoc, metaFile);
    metaFile.close();
  }

  book.isDownloaded = true;
  book.localFilePath = filename;
  LOG_INF("BKO", "Successfully downloaded %s", filename.c_str());
  return HttpDownloader::OK;
}

bool BookOrbitClient::hasBookMetadata(const std::string& bookPath) {
  const std::string metaPath = bookPath + ".meta.json";
  return Storage.exists(metaPath.c_str());
}

bool BookOrbitClient::getBookMetadata(const std::string& bookPath, std::string& outFileId, std::string& outBookId,
                                      float* outProgress) {
  const std::string metaPath = bookPath + ".meta.json";
  if (!Storage.exists(metaPath.c_str())) return false;

  std::string fileContent;
  if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, fileContent)) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, fileContent);
  if (err) return false;

  const std::string fId = jsonVariantToIdString(doc["bookorbit"]["fileId"]);
  const std::string bId = jsonVariantToIdString(doc["bookorbit"]["bookId"]);
  if (fId.empty()) return false;

  outFileId = fId;
  outBookId = bId;
  if (outProgress && doc["progress"].is<float>()) {
    *outProgress = doc["progress"].as<float>();
  }
  return true;
}

void BookOrbitClient::recordBookProgress(const std::string& bookPath, float progressRatio, const bool forcePush) {
  if (!BOOKORBIT_STORE.isEnabled()) return;
  const std::string metaPath = bookPath + ".meta.json";
  if (!Storage.exists(metaPath.c_str())) return;

  std::string fileContent;
  if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, fileContent)) {
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, fileContent);
  if (err) return;

  const std::string fileId = jsonVariantToIdString(doc["bookorbit"]["fileId"]);
  if (fileId.empty()) return;

  doc["progress"] = progressRatio;

  bool synced = false;
  if ((forcePush || BOOKORBIT_STORE.getSyncOnClose()) && WiFi.status() == WL_CONNECTED) {
    synced = syncReadingProgress(fileId, progressRatio);
  }

  doc["pendingSync"] = !synced;

  HalFile metaFile;
  if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
    serializeJson(doc, metaFile);
    metaFile.close();
  }
}

void BookOrbitClient::syncPendingProgress() {
  if (!BOOKORBIT_STORE.isConfigured() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const std::string downloadDir = getDownloadDir();
  auto dir = Storage.open(downloadDir.c_str());
  if (!dir || !dir.isDirectory()) {
    return;
  }

  dir.rewindDirectory();
  std::vector<std::string> metaFiles;
  char nameBuf[128];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(nameBuf, sizeof(nameBuf));
    const std::string name(nameBuf);
    if (name.length() > 10 && name.rfind(".meta.json") == name.length() - 10) {
      metaFiles.push_back(downloadDir + "/" + name);
    }
  }

  for (const auto& metaPath : metaFiles) {
    std::string content;
    if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, content)) continue;

    JsonDocument doc;
    if (deserializeJson(doc, content)) continue;

    const bool pendingSync = doc["pendingSync"] | false;
    const std::string fileId = jsonVariantToIdString(doc["bookorbit"]["fileId"]);
    if (pendingSync && !fileId.empty()) {
      const float progress = doc["progress"] | 0.0f;
      LOG_INF("BKO", "Syncing pending progress %.2f for file %s", progress, fileId.c_str());
      if (syncReadingProgress(fileId, progress)) {
        doc["pendingSync"] = false;
        HalFile metaFile;
        if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
          serializeJson(doc, metaFile);
          metaFile.close();
        }
      }
    }
  }
}

bool BookOrbitClient::syncReadingProgress(const std::string& fileId, float progressPercent, int currentPosition,
                                          int durationSeconds) {
  if (!BOOKORBIT_STORE.isConfigured() || fileId.empty()) {
    return false;
  }

  std::string token;
  std::string errMsg;
  if (!ensureAuthenticated(token, errMsg)) {
    return false;
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/reading-progress";

  // Convert 0..1 ratio to 0..100 percentage expected by BookOrbit's /api/v1/reading-progress
  const float pct100 = (progressPercent <= 1.0f) ? (progressPercent * 100.0f) : progressPercent;
  const long numericFileId = std::strtol(fileId.c_str(), nullptr, 10);

  JsonDocument doc;
  JsonObject writeObj = doc["writes"].to<JsonArray>().add<JsonObject>();
  if (numericFileId > 0) {
    writeObj["fileId"] = numericFileId;
  } else {
    writeObj["fileId"] = fileId;
  }
  writeObj["kind"] = "text";
  writeObj["percentage"] = pct100;
  writeObj["text"]["cfi"] = nullptr;
  if (currentPosition > 0) {
    writeObj["text"]["page"] = currentPosition;
  } else {
    writeObj["text"]["page"] = nullptr;
  }
  writeObj["kobo"]["source"] = nullptr;
  writeObj["kobo"]["type"] = nullptr;
  writeObj["kobo"]["value"] = nullptr;
  writeObj["kobo"]["contentPercent"] = nullptr;
  writeObj["koreader"] = nullptr;
  (void)durationSeconds;

  std::string jsonBody;
  serializeJson(doc, jsonBody);

  std::string response;
  int status = 0;
  const bool ok = HttpDownloader::putJson(url, jsonBody, response, token, &status);

  if (!ok) {
    LOG_ERR("BKO", "Progress sync failed (HTTP %d)", status);
    return false;
  }

  LOG_INF("BKO", "Synced progress %.2f%% for file %s", pct100, fileId.c_str());
  return true;
}

void BookOrbitClient::updateLocalSidecarProgress(const std::string& bookPath, float serverRatio) {
  const std::string metaPath = bookPath + ".meta.json";
  if (!Storage.exists(metaPath.c_str())) return;

  std::string fileContent;
  if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, fileContent)) return;

  JsonDocument doc;
  if (deserializeJson(doc, fileContent)) return;

  const float currentProg = doc["progress"] | 0.0f;
  const bool pendingSync = doc["pendingSync"] | false;

  // Don't overwrite local unpushed progress if local is ahead of server
  if (pendingSync && currentProg > serverRatio) {
    return;
  }

  if (std::abs(serverRatio - currentProg) > 0.005f) {
    doc["progress"] = serverRatio;
    doc["serverProgress"] = serverRatio;
    doc["remoteProgressPending"] = true;
    doc["pendingSync"] = false;

    HalFile metaFile;
    if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
      serializeJson(doc, metaFile);
      metaFile.close();
      LOG_INF("BKO", "Updated sidecar progress for %s to %.2f (pending reader jump)", bookPath.c_str(), serverRatio);
    }
  }
}

bool BookOrbitClient::checkAndConsumeRemoteProgress(const std::string& bookPath, float& outProgress) {
  const std::string metaPath = bookPath + ".meta.json";
  if (!Storage.exists(metaPath.c_str())) return false;

  std::string fileContent;
  if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, fileContent)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, fileContent)) return false;

  const bool remotePending = doc["remoteProgressPending"] | false;
  const float prog = doc["progress"] | 0.0f;

  if (remotePending && prog > 0.0f) {
    outProgress = prog;
    doc["remoteProgressPending"] = false;
    HalFile metaFile;
    if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
      serializeJson(doc, metaFile);
      metaFile.close();
    }
    return true;
  }
  return false;
}

// Fetches the server's reading position for one downloaded book and, when it differs from the local one,
// records it in the sidecar for the reader to jump to. Returns true if the sidecar was updated.
static bool pullOneBook(const std::string& metaPath, const std::string& token) {
  std::string content;
  if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, content)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, content)) return false;

  const std::string fileId = jsonVariantToIdString(doc["bookorbit"]["fileId"]);
  if (fileId.empty()) return false;

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/reading-progress?fileId=" + fileId;
  std::string response;
  int status = 0;
  HttpDownloader::FetchOptions opts;
  opts.bearer = token;
  opts.statusOut = &status;
  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&response](const uint8_t* data, size_t len) {
        if (response.size() + len < 4096) {
          response.append(reinterpret_cast<const char*>(data), len);
        }
        return true;
      },
      opts);
  if (!ok || status != 200) {
    return false;
  }

  JsonDocument respDoc;
  if (deserializeJson(respDoc, response)) return false;

  if (!respDoc["items"].is<JsonArrayConst>() || respDoc["items"].as<JsonArrayConst>().size() == 0) {
    return false;
  }

  JsonObjectConst item = respDoc["items"][0];
  if (!item["percentage"].is<float>()) return false;

  const float serverPct = item["percentage"].as<float>();
  const float serverRatio = serverPct / 100.0f;
  const float currentProg = doc["progress"] | 0.0f;
  const bool pendingSync = doc["pendingSync"] | false;

  // If local has unpushed progress ahead of server, don't clobber it
  if (pendingSync && currentProg > serverRatio) {
    return false;
  }

  if (std::abs(serverRatio - currentProg) > 0.005f) {
    LOG_INF("BKO", "Pulled newer server progress for %s: %.2f -> %.2f", fileId.c_str(), currentProg, serverRatio);
    doc["progress"] = serverRatio;
    doc["serverProgress"] = serverRatio;
    doc["remoteProgressPending"] = true;
    doc["pendingSync"] = false;

    HalFile metaFile;
    if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
      serializeJson(doc, metaFile);
      metaFile.close();
      return true;
    }
  }
  return false;
}

bool BookOrbitClient::pullRemoteProgress(int* outUpdatedCount) {
  if (outUpdatedCount) *outUpdatedCount = 0;
  if (!BOOKORBIT_STORE.isConfigured() || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  std::string token, errMsg;
  if (!ensureAuthenticated(token, errMsg)) {
    return false;
  }

  const std::string downloadDir = getDownloadDir();
  auto dir = Storage.open(downloadDir.c_str());
  if (!dir || !dir.isDirectory()) {
    return false;
  }

  dir.rewindDirectory();
  std::vector<std::string> metaFiles;
  char nameBuf[128];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(nameBuf, sizeof(nameBuf));
    const std::string name(nameBuf);
    if (name.length() > 10 && name.rfind(".meta.json") == name.length() - 10) {
      metaFiles.push_back(downloadDir + "/" + name);
    }
  }

  int updated = 0;
  for (const auto& metaPath : metaFiles) {
    if (pullOneBook(metaPath, token)) updated++;
  }

  if (outUpdatedCount) *outUpdatedCount = updated;
  return true;
}

bool BookOrbitClient::syncAllProgress(int* outPushedCount, int* outPulledCount) {
  if (outPushedCount) *outPushedCount = 0;
  if (outPulledCount) *outPulledCount = 0;

  if (!BOOKORBIT_STORE.isConfigured() || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  // 1. Scan and push any dirty local progress
  int pushed = 0;
  const std::string downloadDir = getDownloadDir();
  auto dir = Storage.open(downloadDir.c_str());
  if (dir && dir.isDirectory()) {
    dir.rewindDirectory();
    std::vector<std::string> metaFiles;
    char nameBuf[128];
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      if (entry.isDirectory()) continue;
      entry.getName(nameBuf, sizeof(nameBuf));
      const std::string name(nameBuf);
      if (name.length() > 10 && name.rfind(".meta.json") == name.length() - 10) {
        metaFiles.push_back(downloadDir + "/" + name);
      }
    }

    for (const auto& metaPath : metaFiles) {
      std::string content;
      if (!Storage.readFileToString("BKO", metaPath.c_str(), 2048, content)) continue;

      JsonDocument doc;
      if (deserializeJson(doc, content)) continue;

      const bool pendingSync = doc["pendingSync"] | false;
      const std::string fileId = jsonVariantToIdString(doc["bookorbit"]["fileId"]);
      if (pendingSync && !fileId.empty()) {
        const float progress = doc["progress"] | 0.0f;
        LOG_INF("BKO", "Syncing pending progress %.2f for file %s", progress, fileId.c_str());
        if (syncReadingProgress(fileId, progress)) {
          doc["pendingSync"] = false;
          HalFile metaFile;
          if (Storage.openFileForWrite("BKO", metaPath.c_str(), metaFile)) {
            serializeJson(doc, metaFile);
            metaFile.close();
            pushed++;
          }
        }
      }
    }
  }
  if (outPushedCount) *outPushedCount = pushed;

  // 2. Pull latest server progress for all downloaded books
  return pullRemoteProgress(outPulledCount);
}

bool BookOrbitClient::loadLocalBooks(std::vector<BookOrbitBook>& outBooks) {
  outBooks.clear();
  const std::string downloadDir = getDownloadDir();
  auto dir = Storage.open(downloadDir.c_str());
  if (!dir || !dir.isDirectory()) {
    return false;
  }

  dir.rewindDirectory();
  std::vector<std::string> files;
  char nameBuf[128];
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(nameBuf, sizeof(nameBuf));
    const std::string name(nameBuf);
    if (name.length() > 5 && name.rfind(".epub") == name.length() - 5) {
      files.push_back(downloadDir + "/" + name);
    }
  }

  for (const auto& epubPath : files) {
    BookOrbitBook b;
    b.isDownloaded = true;
    b.localFilePath = epubPath;

    const std::string metaPath = epubPath + ".meta.json";
    std::string content;
    if (Storage.exists(metaPath.c_str()) && Storage.readFileToString("BKO", metaPath.c_str(), 2048, content)) {
      JsonDocument doc;
      if (!deserializeJson(doc, content)) {
        b.title = doc["title"] | "";
        b.author = doc["author"] | "";
        b.id = jsonVariantToIdString(doc["bookorbit"]["bookId"]);
        b.fileId = jsonVariantToIdString(doc["bookorbit"]["fileId"]);
        b.progress = doc["progress"] | 0.0f;
        if (doc["seriesId"].is<int>()) {
          b.seriesId = doc["seriesId"].as<int>();
        }
        b.seriesName = doc["seriesName"] | "";
        b.seriesIndex = jsonVariantToIdString(doc["seriesIndex"]);
      }
    }

    if (b.title.empty()) {
      size_t slash = epubPath.rfind('/');
      std::string name = (slash != std::string::npos) ? epubPath.substr(slash + 1) : epubPath;
      if (name.length() > 5 && name.rfind(".epub") == name.length() - 5) {
        name = name.substr(0, name.length() - 5);
      }
      size_t dash = name.find(" - ");
      if (dash != std::string::npos) {
        if (b.author.empty()) b.author = name.substr(0, dash);
        b.title = name.substr(dash + 3);
      } else {
        b.title = name;
      }
    }

    const int liveProg = loadBookProgress(epubPath);
    if (liveProg >= 0) {
      b.progress = static_cast<float>(liveProg) / 100.0f;
    }

    outBooks.push_back(std::move(b));
  }

  // Sort: active reading (0 < prog < 1) first, then higher progress, then title
  std::sort(outBooks.begin(), outBooks.end(), [](const BookOrbitBook& a, const BookOrbitBook& b) {
    const bool aActive = (a.progress > 0.0f && a.progress < 0.99f);
    const bool bActive = (b.progress > 0.0f && b.progress < 0.99f);
    if (aActive != bActive) return aActive > bActive;
    if (a.progress != b.progress) return a.progress > b.progress;
    return a.title < b.title;
  });

  return !outBooks.empty();
}

bool BookOrbitClient::connectWifiBriefly(const unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
  WIFI_STORE.loadFromFile();
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) {
    LOG_DBG("BKO", "No saved Wi-Fi network; skipping background connect");
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  const unsigned long deadline = millis() + timeoutMs;
  while ((WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) && millis() < deadline) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void BookOrbitClient::wifiOff() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool BookOrbitClient::pushProgressBurst(const std::string& bookPath, const float progressRatio) {
  if (!BOOKORBIT_STORE.isConfigured()) return false;

  // Wi-Fi + TLS need internal RAM (not PSRAM) in one piece; the reader itself runs with thin margins.
  constexpr size_t MIN_INTERNAL_FREE = 100 * 1024;
  constexpr size_t MIN_INTERNAL_BLOCK = 50 * 1024;
  const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t blockInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  LOG_INF("BKO", "periodic sync check: internal free=%u block=%u", (unsigned)freeInternal, (unsigned)blockInternal);
  if (freeInternal < MIN_INTERNAL_FREE || blockInternal < MIN_INTERNAL_BLOCK) {
    LOG_INF("BKO", "periodic sync skipped: not enough internal heap");
    return false;
  }
  if (powerManager.getBatteryPercentage() < 25) return false;

  const bool radioWasUp = (WiFi.status() == WL_CONNECTED);
  const bool connected = connectWifiBriefly(8000);
  if (connected) {
    recordBookProgress(bookPath, progressRatio, /*forcePush=*/true);
  }
  if (!radioWasUp) wifiOff();
  LOG_INF("BKO", "periodic sync %s; internal free=%u block=%u", connected ? "done" : "failed (no Wi-Fi)",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return connected;
}

bool BookOrbitClient::pullProgressForBook(const std::string& bookPath) {
  if (!BOOKORBIT_STORE.isConfigured() || WiFi.status() != WL_CONNECTED) return false;
  std::string token, errMsg;
  if (!ensureAuthenticated(token, errMsg)) return false;
  const std::string metaPath = bookPath + ".meta.json";
  if (!Storage.exists(metaPath.c_str())) return false;
  return pullOneBook(metaPath, token);
}

namespace {
// RTC_NOINIT survives ESP.restart() (not power loss): lets one reader open skip its sync, e.g. the reboot
// that follows a sync, or a book opened from BookOrbit, which has just synced on its own.
RTC_NOINIT_ATTR uint32_t g_skipOpenSyncMagic;
constexpr uint32_t SKIP_OPEN_SYNC_MAGIC = 0x0B0C5A11;
}  // namespace

void BookOrbitClient::skipNextOpenSync() { g_skipOpenSyncMagic = SKIP_OPEN_SYNC_MAGIC; }

bool BookOrbitClient::consumeSkipOpenSync() {
  const bool skip = (g_skipOpenSyncMagic == SKIP_OPEN_SYNC_MAGIC);
  g_skipOpenSyncMagic = 0;
  return skip;
}
