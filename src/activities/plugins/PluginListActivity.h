#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "plugins/PluginRegistry.h"

// Plugins menu: installed plugins (from /plugins/*/plugin.json) followed by
// built-in runtimes that can be installed. Confirm opens an enabled plugin or
// installs an available one; long-press Confirm offers open / enable-disable /
// remove.
class PluginListActivity final : public UiListActivity {
 public:
  PluginListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("Plugins", renderer, mappedInput, true) {}

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  struct Row {
    bool installed;
    int index;  // into installed or available
  };

  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleButtons() override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  void refresh();
  void open(const plugins::Manifest& manifest);
  void showActions(int installedIndex);
  void install(int availableIndex);
  void confirmRemove(int installedIndex);

  OptionPopup popup;
  std::vector<plugins::Manifest> installed;
  std::vector<const plugins::Runtime*> available;
  std::vector<Row> rows;
  std::vector<std::string> subtitles;
  std::vector<freeink::ui::ListItem> rowItems;
  const char* deferredMessage = nullptr;
  int pendingRemove = -1;
};
