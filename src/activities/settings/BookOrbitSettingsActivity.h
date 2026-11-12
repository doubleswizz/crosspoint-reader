#pragma once

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * Submenu for BookOrbit settings.
 * Controls Server URL, API Key, auto-sync, and connection status.
 */
class BookOrbitSettingsActivity final : public UiListActivity {
 public:
  explicit BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 7;

  void render(RenderLock&&) override;

 private:
  OptionPopup optionPopup;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
