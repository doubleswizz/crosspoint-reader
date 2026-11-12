#include "BookOrbitSettingsActivity.h"

#include <Logging.h>
#include <WiFi.h>

#include "BookOrbitStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "network/BookOrbitClient.h"

namespace fui = freeink::ui;

namespace {
constexpr int IDX_ENABLED = 0;
constexpr int IDX_SERVER_URL = 1;
constexpr int IDX_USERNAME = 2;
constexpr int IDX_API_KEY = 3;
constexpr int IDX_SYNC_ON_OPEN = 4;
constexpr int IDX_SYNC_ON_CLOSE = 5;
constexpr int IDX_TEST_CONNECTION = 6;

const char* menuLabels[BookOrbitSettingsActivity::MENU_ITEMS] = {
    "Enable BookOrbit",  "Server URL",         "Username",       "Password / API Key",
    "Sync On Book Open", "Sync On Book Close", "Test Connection"};
}  // namespace

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("BookOrbitSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = menuLabels[i];
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int BookOrbitSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* BookOrbitSettingsActivity::headerTitle() const { return "BookOrbit Settings"; }

bool BookOrbitSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void BookOrbitSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  app.clearTapFlash();

  if (index == IDX_ENABLED) {
    BOOKORBIT_STORE.setEnabled(!BOOKORBIT_STORE.isEnabled());
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_SERVER_URL) {
    const std::string currentUrl = BOOKORBIT_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Server URL", prefillUrl, 128, InputType::Url),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            const std::string urlToSave = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
            BOOKORBIT_STORE.setServerUrl(urlToSave);
            BOOKORBIT_STORE.saveToFile();
            requestUpdate();
          }
        });
  } else if (index == IDX_USERNAME) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Username",
                                                                   BOOKORBIT_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               BOOKORBIT_STORE.setUsername(kb.text);
                               BOOKORBIT_STORE.saveToFile();
                               requestUpdate();
                             }
                           });
  } else if (index == IDX_API_KEY) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Password / API Key",
                                                BOOKORBIT_STORE.getApiKey(), 128, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT_STORE.setApiKey(kb.text);
            BOOKORBIT_STORE.saveToFile();
            requestUpdate();
          }
        });
  } else if (index == IDX_SYNC_ON_OPEN) {
    BOOKORBIT_STORE.setSyncOnOpen(!BOOKORBIT_STORE.getSyncOnOpen());
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_SYNC_ON_CLOSE) {
    BOOKORBIT_STORE.setSyncOnClose(!BOOKORBIT_STORE.getSyncOnClose());
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (index == IDX_TEST_CONNECTION) {
    if (!BOOKORBIT_STORE.isConfigured()) return;
    auto runTest = [this]() {
      std::string err;
      const bool ok = BookOrbitClient::testConnection(err);
      const std::string msg = ok ? "Successfully connected to BookOrbit server!" : ("Connection error: " + err);
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Connection Test", msg, "", "OK"),
          [](const ActivityResult&) {});
    };
    if (WiFi.status() == WL_CONNECTED) {
      runTest();
    } else {
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                             [runTest](const ActivityResult& result) {
                               if (!result.isCancelled) runTest();
                             });
    }
  }
}

void BookOrbitSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rowValues_[IDX_ENABLED] = BOOKORBIT_STORE.isEnabled() ? "Enabled" : "Disabled";
  rowValues_[IDX_SERVER_URL] = BOOKORBIT_STORE.getServerUrl().empty() ? "[Not Set]" : BOOKORBIT_STORE.getServerUrl();
  rowValues_[IDX_USERNAME] = BOOKORBIT_STORE.getUsername().empty() ? "[Optional]" : BOOKORBIT_STORE.getUsername();
  rowValues_[IDX_API_KEY] = BOOKORBIT_STORE.getApiKey().empty() ? "[Not Set]" : "********";
  rowValues_[IDX_SYNC_ON_OPEN] = BOOKORBIT_STORE.getSyncOnOpen() ? "Yes" : "No";
  rowValues_[IDX_SYNC_ON_CLOSE] = BOOKORBIT_STORE.getSyncOnClose() ? "Yes" : "No";
  rowValues_[IDX_TEST_CONNECTION] = BOOKORBIT_STORE.isConfigured() ? "Test Now" : "[Configure first]";

  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  GUI.setCheckboxRow(rowItems_[IDX_ENABLED], BOOKORBIT_STORE.isEnabled());
  GUI.setCheckboxRow(rowItems_[IDX_SYNC_ON_OPEN], BOOKORBIT_STORE.getSyncOnOpen());
  GUI.setCheckboxRow(rowItems_[IDX_SYNC_ON_CLOSE], BOOKORBIT_STORE.getSyncOnClose());

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void BookOrbitSettingsActivity::render(RenderLock&& lock) {
  UiListActivity::render(std::move(lock));
  if (optionPopup.isActive()) {
    optionPopup.render(renderer);
  }
}
