#include "BookOrbitClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>

#include "CrossPointSettings.h"
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
    // Also read local sidecar progress if server progress is 0
    if (book.progress <= 0.0f) {
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

  if (isScroller) {
    url = baseUrl + "/api/v1/dashboard/scrollers/batch";
    const std::string scrollerType = (shelfOrEndpoint == "currently-reading") ? "continue-reading" : shelfOrEndpoint;
    JsonDocument reqDoc;
    JsonObject item = reqDoc["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = "1";
    item["type"] = scrollerType;
    item["limit"] = pageSize;
    serializeJson(reqDoc, postBody);
  } else if (shelfOrEndpoint.rfind("library:", 0) == 0) {
    const std::string libId = shelfOrEndpoint.substr(8);
    url = baseUrl + "/api/v1/libraries/" + libId + "/books";
    JsonDocument reqDoc;
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
  bool ok = HttpDownloader::postJson(url, postBody, jsonPayload, token, &status, MAX_PAYLOAD_SIZE);
  if (status == 401) {
    // Token may have expired; force refresh and retry once
    if (ensureAuthenticated(token, outErrorMessage, true)) {
      jsonPayload.clear();
      ok = HttpDownloader::postJson(url, postBody, jsonPayload, token, &status, MAX_PAYLOAD_SIZE);
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
    bookFilter["readingProgress"] = true;
    bookFilter["files"][0]["id"] = true;
    bookFilter["files"][0]["format"] = true;
  } else {
    filter["total"] = true;
    JsonObject bookFilter = filter["items"][0].to<JsonObject>();
    bookFilter["id"] = true;
    bookFilter["title"] = true;
    bookFilter["authors"] = true;
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
        parseBookArray(booksArr, outBooks);
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

void BookOrbitClient::recordBookProgress(const std::string& bookPath, float progressRatio) {
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
  if (BOOKORBIT_STORE.getSyncOnClose() && WiFi.status() == WL_CONNECTED) {
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
