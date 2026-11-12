#include "MoreActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t ICON_SIZE = 32;
}  // namespace

MoreActivity::MoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("More", renderer, mappedInput) {}

void MoreActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void MoreActivity::rebuildRows() {
  entries_.clear();
  entries_.push_back({Action::FILE_BROWSER, tr(STR_BROWSE_FILES), &icon_folder_32});
  entries_.push_back({Action::LIBRARY, tr(STR_LIBRARY), &icon_library_32});
  entries_.push_back({Action::BOOKORBIT, tr(STR_BOOKORBIT), &icon_book_open_32});
  if (SETTINGS.pluginsEnabled != 0) {
    entries_.push_back({Action::PLUGINS, tr(STR_PLUGINS), &icon_blocks_32});
  }
  if (OPDS_STORE.hasServers()) {
    entries_.push_back({Action::OPDS_BROWSER, tr(STR_OPDS_BROWSER), &icon_book_32});
  }
  entries_.push_back({Action::FILE_TRANSFER, tr(STR_FILE_TRANSFER), &icon_wifi_32});
  entries_.push_back({Action::SETTINGS_MENU, tr(STR_SETTINGS_TITLE), &icon_settings_32});

  rowItems_.resize(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    rowItems_[i].label = entries_[i].label;
    rowItems_[i].icon = fui::bitmapFromIcon(*entries_[i].icon);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

void MoreActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;

  const auto& theme = screen.theme();
  const int16_t textHeight = static_cast<int16_t>(screen.target().lineHeight(theme.bodyText.font) +
                                                  screen.target().lineHeight(theme.smallText.font));
  const int16_t paddingY = mappedInput.hasTouch() ? theme.listTouchRowPaddingY : theme.listRowPaddingY;
  props.rowHeight = static_cast<int16_t>(std::max<int16_t>(textHeight, ICON_SIZE) + 2 * paddingY);
  syncListViewport(screen, props);
  screen.list(props);
}

void MoreActivity::activateIndex(const int index) {
  if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return;
  app.clearTapFlash();
  switch (entries_[index].action) {
    case Action::FILE_BROWSER:
      activityManager.goToFileBrowser();
      break;
    case Action::LIBRARY:
      activityManager.goToLibrary();
      break;
    case Action::BOOKORBIT:
      activityManager.goToBookOrbit();
      break;
    case Action::PLUGINS:
      activityManager.goToPlugins();
      break;
    case Action::OPDS_BROWSER:
      activityManager.goToBrowser();
      break;
    case Action::FILE_TRANSFER:
      activityManager.goToFileTransfer();
      break;
    case Action::SETTINGS_MENU:
      activityManager.goToSettings();
      break;
  }
}

void MoreActivity::onBackButton() { onGoHome(HomeMenuItem::MORE); }
