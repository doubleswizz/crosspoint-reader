#pragma once

#include <FreeInkUIIcon.h>

#include <vector>

#include "activities/UiListActivity.h"

/**
 * Secondary home menu ("More") holding standard CrossPoint utilities
 * (Browse Files, Library, BookOrbit submenu, optional OPDS/Plugins,
 * File Transfer, and Settings) when BookOrbit actions occupy the home screen.
 */
class MoreActivity final : public UiListActivity {
  enum class Action { FILE_BROWSER, LIBRARY, BOOKORBIT, PLUGINS, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU };

  struct Entry {
    Action action;
    const char* label;
    const freeink::Icon* icon;
  };

  std::vector<Entry> entries_;
  std::vector<freeink::ui::ListItem> rowItems_;

  void rebuildRows();
  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  const char* headerTitle() const override { return "More"; }

 public:
  explicit MoreActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
};
