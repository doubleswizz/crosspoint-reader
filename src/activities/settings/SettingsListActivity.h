#pragma once

#include <array>
#include <memory>

#include "SettingsSection.h"
#include "activities/UiListActivity.h"

// Settings root: one row per SettingsSection plus top-level BookOrbit and
// firmware Update entries, each opening its respective screen.
class SettingsListActivity final : public UiListActivity {
  // Section opened on the first loop pass (End = none), e.g. returning to
  // Network after the Wi-Fi rows' silent restart.
  SettingsSection pendingSection;

  static constexpr size_t BOOKORBIT_ROW_INDEX = SETTINGS_SECTION_COUNT;
  static constexpr size_t UPDATE_ROW_INDEX = SETTINGS_SECTION_COUNT + 1;
  static constexpr size_t MAX_ROW_COUNT = SETTINGS_SECTION_COUNT + 2;

  std::array<freeink::ui::ListItem, MAX_ROW_COUNT> rowItems_{};
  size_t rowCount_ = BOOKORBIT_ROW_INDEX + 1;

  void rebuildRows();
  void openSection(SettingsSection section);
  void launchRootActivity(size_t rowIndex, std::unique_ptr<Activity> activity, const char* activityName);

  int listCount() const override { return static_cast<int>(rowCount_); }
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
