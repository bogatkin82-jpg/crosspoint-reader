#include "PluginListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
}

void PluginListActivity::onEnter() {
  UiListActivity::onEnter();
  refresh();
  nav.selected = 0;
}

void PluginListActivity::refresh() {
  // installed/rows/rowItems are read by the render task.
  RenderLock lock;
  installed = plugins::scanInstalled();

  // Offer each built-in runtime that has no installed plugin yet.
  available.clear();
  size_t count = 0;
  const plugins::Runtime* const* all = plugins::runtimes(count);
  for (size_t i = 0; i < count; i++) {
    bool present = false;
    for (const auto& m : installed) present |= m.runtime == all[i]->id;
    if (!present) available.push_back(all[i]);
  }

  rows.clear();
  rows.reserve(installed.size() + available.size());
  subtitles.assign(installed.size() + available.size(), std::string());
  for (int i = 0; i < static_cast<int>(installed.size()); i++) rows.push_back({true, i});
  for (int i = 0; i < static_cast<int>(available.size()); i++) rows.push_back({false, i});

  rowItems.clear();
  rowItems.reserve(rows.size());
  for (size_t r = 0; r < rows.size(); r++) {
    fui::ListItem item;
    item.actionValue = static_cast<int16_t>(r);
    if (rows[r].installed) {
      const auto& m = installed[rows[r].index];
      item.label = m.name.c_str();
      if (!plugins::findRuntime(m.runtime)) {
        subtitles[r] = std::string(tr(STR_PLUGIN_UNSUPPORTED)) + ": " + m.runtime;
      } else if (!m.enabled) {
        subtitles[r] = tr(STR_PLUGIN_DISABLED);
      } else {
        subtitles[r] = m.description;
      }
      if (r == 0) item.sectionHeading = tr(STR_PLUGINS_INSTALLED);
    } else {
      const auto* rt = available[rows[r].index];
      item.label = I18N.get(rt->nameId);
      subtitles[r] = I18N.get(rt->descriptionId);
      item.value = tr(STR_PLUGIN_INSTALL);
      if (rows[r].index == 0) item.sectionHeading = tr(STR_PLUGINS_AVAILABLE);
    }
    item.subtitle = subtitles[r].empty() ? nullptr : subtitles[r].c_str();
    rowItems.push_back(item);
  }
  if (nav.selected >= listCount()) nav.selected = listCount() > 0 ? listCount() - 1 : 0;
}

const char* PluginListActivity::headerTitle() const { return tr(STR_PLUGINS); }

void PluginListActivity::onBackButton() { activityManager.goHome(HomeMenuItem::PLUGINS); }

void PluginListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_PLUGINS_EMPTY), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void PluginListActivity::render(RenderLock&& lock) {
  if (popup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

bool PluginListActivity::handleCustomInput() {
  if (deferredMessage && !popup.isActive()) {
    const char* options[] = {tr(STR_BACK)};
    popup.show(tr(STR_PLUGINS), deferredMessage, options, 1, 0, [](int) {});
    deferredMessage = nullptr;
    requestUpdate();
    return true;
  }
  if (pendingRemove >= 0 && !popup.isActive()) {
    const int idx = pendingRemove;
    pendingRemove = -1;
    confirmRemove(idx);
    return true;
  }
  return popup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool PluginListActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    onRowLongPress(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void PluginListActivity::activateIndex(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const Row row = rows[index];
  if (!row.installed) {
    install(row.index);
    return;
  }
  const auto& m = installed[row.index];
  if (m.enabled && plugins::findRuntime(m.runtime)) {
    open(m);
  } else {
    showActions(row.index);
  }
}

void PluginListActivity::onRowLongPress(const int index) {
  if (popup.isActive() || index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (rows[index].installed) {
    showActions(rows[index].index);
  } else {
    install(rows[index].index);
  }
}

void PluginListActivity::open(const plugins::Manifest& manifest) {
  const plugins::Runtime* rt = plugins::findRuntime(manifest.runtime);
  if (!rt) return;
  auto activity = rt->create(renderer, mappedInput, manifest);
  if (!activity) return;
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { refresh(); });
}

void PluginListActivity::showActions(const int installedIndex) {
  const auto& m = installed[installedIndex];
  const bool supported = plugins::findRuntime(m.runtime) != nullptr;
  const char* options[3];
  int count = 0;
  if (supported && m.enabled) options[count++] = tr(STR_PLUGIN_OPEN);
  options[count++] = m.enabled ? tr(STR_PLUGIN_DISABLE) : tr(STR_PLUGIN_ENABLE);
  options[count++] = tr(STR_PLUGIN_REMOVE);
  const bool hasOpen = supported && m.enabled;
  popup.show(tr(STR_PLUGIN_ACTIONS), m.name.c_str(), options, count, 0, [this, installedIndex, hasOpen](int idx) {
    if (hasOpen && idx == 0) {
      open(installed[installedIndex]);
      return;
    }
    if (hasOpen) idx--;
    if (idx == 0) {
      auto m = installed[installedIndex];
      m.enabled = !m.enabled;
      if (!plugins::saveManifest(m)) LOG_ERR("PLUG", "Cannot save manifest %s", m.dir.c_str());
      refresh();
      requestUpdate();
    } else if (idx == 1) {
      pendingRemove = installedIndex;  // confirmation opens from loop, outside this callback
    }
  });
  requestUpdate();
}

void PluginListActivity::confirmRemove(const int installedIndex) {
  if (installedIndex < 0 || installedIndex >= static_cast<int>(installed.size())) return;
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, installed[installedIndex].name,
                                                         tr(STR_PLUGIN_REMOVE_CONFIRM));
  if (!confirm) return;
  const std::string dir = installed[installedIndex].dir;
  startActivityForResult(std::move(confirm), [this, dir](const ActivityResult& result) {
    if (result.isCancelled) return;
    for (const auto& m : installed) {
      if (m.dir == dir) {
        plugins::removePlugin(m);
        break;
      }
    }
    refresh();
  });
}

void PluginListActivity::install(const int availableIndex) {
  if (availableIndex < 0 || availableIndex >= static_cast<int>(available.size())) return;
  plugins::Manifest m;
  if (!plugins::installBuiltin(*available[availableIndex], &m)) {
    deferredMessage = tr(STR_PLUGIN_INSTALL_FAILED);
    requestUpdate();
    return;
  }
  refresh();
  for (int r = 0; r < listCount(); r++) {
    if (rows[r].installed && installed[rows[r].index].dir == m.dir) {
      moveSelectionTo(r);
      break;
    }
  }
  requestUpdate();
}
