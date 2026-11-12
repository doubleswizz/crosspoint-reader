#pragma once

#include <array>

#include "SettingsSection.h"
#include "activities/UiListActivity.h"

// Settings root: one row per SettingsSection plus top-level BookOrbit
// settings, each opening its respective settings screen.
class SettingsListActivity final : public UiListActivity {
  // Section opened on the first loop pass (End = none), e.g. returning to
  // Network after the Wi-Fi rows' silent restart.
  SettingsSection pendingSection;

  static constexpr size_t BOOKORBIT_ROW_INDEX = SETTINGS_SECTION_COUNT;
  static constexpr size_t ROW_COUNT = SETTINGS_SECTION_COUNT + 1;

  std::array<freeink::ui::ListItem, ROW_COUNT> rowItems_{};

  void rebuildRows();
  void openSection(SettingsSection section);
  void openBookOrbitSettings();

  int listCount() const override { return static_cast<int>(ROW_COUNT); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  void drawChrome() override;

 public:
  explicit SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                SettingsSection openSection = SettingsSection::End);
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
