#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

struct BookOrbitConfig {
  bool enabled = false;
  std::string serverUrl;
  std::string apiKey;
  std::string username;
  bool syncOnOpen = true;
  bool syncOnClose = true;
  // Background progress push while reading: 0 = off, otherwise minutes between pushes.
  uint8_t periodicSyncMinutes = 0;

  bool isConfigured() const { return enabled && !serverUrl.empty() && !apiKey.empty(); }
};

class BookOrbitStore : public PersistableStore<BookOrbitStore> {
 private:
  BookOrbitConfig config;

  BookOrbitStore() = default;
  friend class PersistableStore<BookOrbitStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookorbit.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const BookOrbitConfig& getConfig() const { return config; }
  void setConfig(const BookOrbitConfig& newConfig);

  bool isConfigured() const { return config.isConfigured(); }
  bool isEnabled() const { return config.enabled; }
  void setEnabled(bool enabled);
  const std::string& getServerUrl() const { return config.serverUrl; }
  void setServerUrl(const std::string& url);
  const std::string& getApiKey() const { return config.apiKey; }
  void setApiKey(const std::string& key);
  const std::string& getUsername() const { return config.username; }
  void setUsername(const std::string& user);
  bool getSyncOnOpen() const { return config.syncOnOpen; }
  void setSyncOnOpen(bool val);
  bool getSyncOnClose() const { return config.syncOnClose; }
  void setSyncOnClose(bool val);
  uint8_t getPeriodicSyncMinutes() const { return config.periodicSyncMinutes; }
  void setPeriodicSyncMinutes(uint8_t minutes);
};

#define BOOKORBIT_STORE BookOrbitStore::getInstance()
