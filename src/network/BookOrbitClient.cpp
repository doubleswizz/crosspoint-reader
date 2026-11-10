#include "BookOrbitClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "util/OpdsFilename.h"

namespace {

std::string cleanBaseUrl(const std::string& raw) {
  std::string url = raw;
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
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
    return true;
  }

  book.isDownloaded = false;
  book.localFilePath.clear();
  return false;
}

bool BookOrbitClient::testConnection(std::string& outErrorMessage) {
  if (!BOOKORBIT_STORE.isConfigured()) {
    outErrorMessage = "BookOrbit is not configured";
    return false;
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/health";
  HttpDownloader::FetchOptions opts;
  opts.bearer = BOOKORBIT_STORE.getApiKey();
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

bool BookOrbitClient::fetchCurrentlyReading(std::vector<BookOrbitBook>& outBooks, std::string& outErrorMessage) {
  return fetchBooks("currently-reading", outBooks, outErrorMessage);
}

bool BookOrbitClient::fetchBooks(const std::string& shelfOrEndpoint, std::vector<BookOrbitBook>& outBooks,
                                 std::string& outErrorMessage) {
  outBooks.clear();

  if (!BOOKORBIT_STORE.isConfigured()) {
    outErrorMessage = "BookOrbit is not configured";
    return false;
  }

  std::string endpoint = shelfOrEndpoint;
  if (!endpoint.empty() && endpoint.front() != '/') {
    endpoint = "/api/v1/books?shelf=" + endpoint;
  } else if (endpoint.empty()) {
    endpoint = "/api/v1/books";
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + endpoint;

  HttpDownloader::FetchOptions opts;
  opts.bearer = BOOKORBIT_STORE.getApiKey();
  int status = 0;
  opts.statusOut = &status;

  std::string jsonPayload;
  constexpr size_t MAX_PAYLOAD_SIZE = 64 * 1024;
  jsonPayload.reserve(8192);

  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&jsonPayload](const uint8_t* data, size_t len) {
        if (jsonPayload.size() + len <= MAX_PAYLOAD_SIZE) {
          jsonPayload.append(reinterpret_cast<const char*>(data), len);
          return true;
        }
        return false;
      },
      opts);

  if (!ok || status < 200 || status >= 300) {
    outErrorMessage = "Failed to fetch books (HTTP " + std::to_string(status) + ")";
    LOG_ERR("BKO", "fetchBooks failed for %s (status %d)", url.c_str(), status);
    return false;
  }

  // Parse JSON response
  JsonDocument filter;
  filter[0]["id"] = true;
  filter[0]["title"] = true;
  filter[0]["author"] = true;
  filter[0]["authors"][0]["name"] = true;
  filter[0]["description"] = true;
  filter[0]["progress"] = true;
  filter[0]["userProgress"]["progress"] = true;
  filter[0]["files"][0]["id"] = true;
  filter[0]["files"][0]["format"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonPayload, DeserializationOption::Filter(filter));
  if (err && err != DeserializationError::NoMemory) {
    // If root is an object like { "data": [...] }, try parsing with data filter
    JsonDocument objFilter;
    objFilter["data"][0]["id"] = true;
    objFilter["data"][0]["title"] = true;
    objFilter["data"][0]["author"] = true;
    objFilter["data"][0]["authors"][0]["name"] = true;
    objFilter["data"][0]["description"] = true;
    objFilter["data"][0]["progress"] = true;
    objFilter["data"][0]["userProgress"]["progress"] = true;
    objFilter["data"][0]["files"][0]["id"] = true;
    objFilter["data"][0]["files"][0]["format"] = true;

    doc.clear();
    err = deserializeJson(doc, jsonPayload, DeserializationOption::Filter(objFilter));
  }

  JsonArrayConst array = doc.is<JsonArray>() ? doc.as<JsonArrayConst>() : doc["data"].as<JsonArrayConst>();
  if (array.isNull()) {
    array = doc["books"].as<JsonArrayConst>();
  }

  if (array.isNull()) {
    LOG_DBG("BKO", "Empty book response or unrecognized structure");
    return true;
  }

  for (JsonObjectConst item : array) {
    BookOrbitBook book;
    book.id = item["id"] | "";
    book.title = item["title"] | "";
    if (book.title.empty()) continue;

    if (item["author"].is<const char*>()) {
      book.author = item["author"].as<const char*>();
    } else if (item["authors"].is<JsonArrayConst>() && item["authors"].as<JsonArrayConst>().size() > 0) {
      book.author = item["authors"][0]["name"] | "";
    }

    book.description = item["description"] | "";

    if (item["progress"].is<float>()) {
      book.progress = item["progress"].as<float>();
    } else if (item["userProgress"]["progress"].is<float>()) {
      book.progress = item["userProgress"]["progress"].as<float>();
    }

    if (item["files"].is<JsonArrayConst>()) {
      for (JsonObjectConst f : item["files"].as<JsonArrayConst>()) {
        const std::string fmt = f["format"] | "";
        if (fmt == "epub" || fmt.empty()) {
          book.fileId = f["id"] | "";
          break;
        }
      }
    }

    checkLocalAvailability(book);
    outBooks.push_back(std::move(book));
  }

  LOG_DBG("BKO", "Fetched %zu books from %s", outBooks.size(), shelfOrEndpoint.c_str());
  return true;
}

HttpDownloader::DownloadError BookOrbitClient::downloadBook(BookOrbitBook& book, ProgressCallback progressCb,
                                                            const bool* cancelFlag, std::string& outErrorMessage) {
  if (!BOOKORBIT_STORE.isConfigured()) {
    outErrorMessage = "BookOrbit is not configured";
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
  headers.emplace_back("Authorization", "Bearer " + BOOKORBIT_STORE.getApiKey());

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

bool BookOrbitClient::syncReadingProgress(const std::string& fileId, float progressPercent, int currentPosition,
                                          int durationSeconds) {
  if (!BOOKORBIT_STORE.isConfigured() || fileId.empty()) {
    return false;
  }

  const std::string url = cleanBaseUrl(BOOKORBIT_STORE.getServerUrl()) + "/api/v1/books/files/" + fileId + "/progress";

  JsonDocument doc;
  doc["progress"] = progressPercent;
  if (currentPosition > 0) doc["position"] = currentPosition;
  if (durationSeconds > 0) doc["duration"] = durationSeconds;

  std::string jsonBody;
  serializeJson(doc, jsonBody);

  std::string response;
  int status = 0;
  const bool ok = HttpDownloader::postJson(url, jsonBody, response, BOOKORBIT_STORE.getApiKey(), &status);

  if (!ok) {
    LOG_ERR("BKO", "Progress sync failed (HTTP %d)", status);
    return false;
  }

  LOG_INF("BKO", "Synced progress %.2f for file %s", progressPercent, fileId.c_str());
  return true;
}
