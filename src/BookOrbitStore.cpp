#include "BookOrbitStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

void BookOrbitStore::toJson(JsonDocument& doc) const {
  doc["enabled"] = config.enabled;
  doc["serverUrl"] = config.serverUrl;
  doc["username"] = config.username;
  doc["apiKey_obf"] = obfuscation::obfuscateToBase64(config.apiKey);
  doc["syncOnOpen"] = config.syncOnOpen;
  doc["syncOnClose"] = config.syncOnClose;
  doc["periodicSyncMinutes"] = config.periodicSyncMinutes;
}

bool BookOrbitStore::fromJson(JsonVariantConst doc) {
  config.enabled = doc["enabled"] | false;
  config.serverUrl = doc["serverUrl"] | "";
  config.username = doc["username"] | "";
  config.syncOnOpen = doc["syncOnOpen"] | true;
  config.syncOnClose = doc["syncOnClose"] | true;
  config.periodicSyncMinutes = doc["periodicSyncMinutes"] | 0;

  if (doc["apiKey_obf"].is<const char*>()) {
    config.apiKey = obfuscation::deobfuscateFromBase64(doc["apiKey_obf"].as<const char*>());
  } else if (doc["apiKey"].is<const char*>()) {
    config.apiKey = doc["apiKey"].as<const char*>();
    requestResave();
  } else {
    config.apiKey.clear();
  }

  LOG_DBG("BKO", "Loaded BookOrbit config (enabled=%d, url=%s)", config.enabled, config.serverUrl.c_str());
  return true;
}

void BookOrbitStore::setConfig(const BookOrbitConfig& newConfig) {
  config = newConfig;
  requestResave();
}

void BookOrbitStore::setEnabled(bool enabled) {
  config.enabled = enabled;
  requestResave();
}

void BookOrbitStore::setServerUrl(const std::string& url) {
  config.serverUrl = url;
  requestResave();
}

void BookOrbitStore::setApiKey(const std::string& key) {
  config.apiKey = key;
  requestResave();
}

void BookOrbitStore::setUsername(const std::string& user) {
  config.username = user;
  requestResave();
}

void BookOrbitStore::setSyncOnOpen(bool val) {
  config.syncOnOpen = val;
  requestResave();
}

void BookOrbitStore::setSyncOnClose(bool val) {
  config.syncOnClose = val;
  requestResave();
}

void BookOrbitStore::setPeriodicSyncMinutes(uint8_t minutes) {
  config.periodicSyncMinutes = minutes;
  requestResave();
}
