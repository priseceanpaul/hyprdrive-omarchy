// The overview's app grid asks the dock for its tiles, dots, menu and click policy.
// docs/manual.md#the-hyprdrive-app-grid
#include "DockParts.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {
// Defined in DockActivate.cpp and Dock.cpp, declared in no header.
void launchApp(PHLMONITOR monitor, const SItem& item, PHLWORKSPACE onWorkspace = nullptr, bool deliberate = false, const std::string& execRule = "");
void launchInEmptyWorkspace(PHLMONITOR monitor, const SItem& item);
void renderRunningDot(PHLMONITOR monitor, const SLayout& layout, const SItem& item, const STally& tally, int dotSize, const CHyprColor& dotColor);
}

// ------------------------------------------------------------ the app grid --
// AppGrid's tiles, built the way buildDockItems builds a favourite.
std::vector<SItem> appItems(const std::vector<std::string>& desktopIds) {
    const auto         TALLY = buildTally();
    const auto&        FAVS  = favorites();
    std::vector<SItem> out;
    out.reserve(desktopIds.size());
    for (const auto& ID : desktopIds) {
        SItem item;
        item.desktopId = ID;
        if (const auto* ENTRY = AppSearch::entryForId(ID))
            item.icon = ENTRY->icon;
        if (const auto* OVERRIDE = IconOverrides::forId(ID))
            item.icon = *OVERRIDE;
        item.favorite = std::ranges::contains(FAVS, ID);
        item.running  = itemWindowCount(item, TALLY) > 0;
        out.emplace_back(std::move(item));
    }
    return out;
}

// The dock's own dots under each running tile's iconBox: one per window.
void renderAppDots(PHLMONITOR monitor, const std::vector<SItem>& items, float alpha) {
    if (!monitor || !Config::getValue<bool>(KEY_SHOW_RUNNING))
        return;
    const auto       TALLY = buildTally();
    const int        SIZE  = Config::getValue<int>(KEY_DOT_SIZE);
    const CHyprColor COLOR = Hyprdrive::Config::dockIndicatorColor();
    for (const auto& ITEM : items)
        if (ITEM.running)
            renderRunningDot(monitor, SLayout{}, ITEM, TALLY, SIZE, COLOR.modifyA(COLOR.a * alpha));
}

// The dock's menu, rows and all, hung off a grid tile. The overview always draws
// the dock, so renderMenu runs; with no pill there is nowhere to draw it.
bool openMenuFor(PHLMONITOR monitor, const SItem& item, const CBox& anchor) {
    closeMenu();
    if (!monitor || item.desktopId.empty() || layout(monitor).pill.empty())
        return false;
    g_menu = SMenu{.monitor = monitor->m_id, .item = item, .anchor = anchor, .floating = true, .box = {}, .rows = buildRows(item, monitor)};
    return true;
}

// A grid click is "take me to this app": its last-used window, or a launch. Not
// the dock's minimize-when-focused, which from a launcher reads as a dead click.
void activateApp(PHLMONITOR monitor, const SItem& item) {
    const uint32_t MODS  = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
    const bool     EMPTY = (MODS & (HL_MODIFIER_META | HL_MODIFIER_ALT | HL_MODIFIER_CTRL)) != 0;
    const auto     ALL   = itemWindows(item);
    if (ALL.empty()) {
        if (EMPTY)
            launchInEmptyWorkspace(monitor, item);
        else
            launchApp(monitor, item);
        return;
    }

    const auto TARGET   = recentWindow(ALL);
    const auto OVERVIEW = scrollOverviewForMonitor(monitor);
    if (OVERVIEW && !Minimize::isMinimized(TARGET)) {
        OVERVIEW->selectAndClose(TARGET);
        return;
    }
    activateWindow(TARGET);
    if (OVERVIEW)
        OVERVIEW->close();
}

// Middle click: whatever dock:middle_click_action says, as on the dock.
void middleClickApp(PHLMONITOR monitor, const SItem& item) {
    const uint32_t MODS  = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
    const bool     SHIFT = (MODS & HL_MODIFIER_SHIFT) != 0;
    applyClickAction(monitor, item, Config::getValue<std::string>(SHIFT ? KEY_SHIFT_MIDDLE : KEY_MIDDLE_CLICK), SHIFT);
}

}
