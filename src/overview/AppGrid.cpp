#include "AppGrid.hpp"
#include "AppSearch.hpp"
#include "Config.hpp"
#include "Dock.hpp"
#include "IOverview.hpp"
#include "KeyboardInset.hpp"
#include "OverviewRender.hpp"
#include "SearchSession.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace Hyprdrive::AppGrid {

namespace {

// Logical px, scaled per monitor. NixOs-dots docs/manual.md#the-hyprdrive-app-grid
constexpr float BASE_CELL_W   = 148.F;
constexpr float BASE_CELL_H   = 146.F;
constexpr float BASE_ICON     = 72.F;
constexpr float BASE_ICON_TOP = 16.F;
constexpr float BASE_LABEL_Y  = 10.F; // below the icon, leaving the running dots room
constexpr float BASE_PAD      = 24.F;
constexpr float BAND_GAP = 10.F; // inside the band, above and below: the container's
constexpr float SIDE     = 0.08F;
constexpr float FILL_SIDE = 0.03F; // a fill monitor's side margin, as a share of its width
constexpr float MOMENTUM_S = 0.25F; // how far a flick carries, in seconds of its release speed
constexpr float FADE_MS  = 170.F;
constexpr int   MAX_COLS = 12;

// Omarchy's launcher hide list: the grid shows what Menu -> Apps shows.
constexpr const char* SYSTEM_HIDES = "/run/current-system/sw/share/omarchy/default/omarchy/launcher.hides";

// apps:scale, or this monitor's "name:scale" entry in apps:scale_monitors.
// NixOs-dots docs/manual.md#one-app-drawer
float userScale(const PHLMONITOR& monitor) {
    float scale = Hyprdrive::Config::getValue<float>("plugin:hyprdrive:apps:scale");
    if (!monitor)
        return scale;
    const std::string LIST = Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:apps:scale_monitors");
    for (size_t at = 0; at < LIST.size();) {
        size_t end = LIST.find(',', at);
        if (end == std::string::npos)
            end = LIST.size();
        const std::string ENTRY = LIST.substr(at, end - at);
        at                      = end + 1;
        const auto COLON        = ENTRY.rfind(':');
        if (COLON == std::string::npos)
            continue;
        auto trim = [](std::string v) {
            const auto B = v.find_first_not_of(" \t");
            const auto E = v.find_last_not_of(" \t");
            return B == std::string::npos ? std::string{} : v.substr(B, E - B + 1);
        };
        if (trim(ENTRY.substr(0, COLON)) != monitor->m_name)
            continue;
        try {
            scale = std::clamp(std::stof(trim(ENTRY.substr(COLON + 1))), 0.5F, 3.F);
        } catch (...) {}
        break;
    }
    return std::clamp(scale, 0.5F, 3.F);
}

// Whether `monitor` is named in apps:fill_monitors ("a,b"): its drawer takes the
// screen. NixOs-dots docs/manual.md#one-app-drawer
bool fillMonitor(const PHLMONITOR& monitor) {
    if (!monitor)
        return false;
    const std::string LIST = Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:apps:fill_monitors") + ",";
    std::string       item;
    for (const char C : LIST) {
        if (C != ',') {
            if (C != ' ' && C != '\t')
                item.push_back(C);
            continue;
        }
        if (item == monitor->m_name)
            return true;
        item.clear();
    }
    return false;
}

struct SApp {
    std::string id, name;
};

struct SState {
    WP<IOverview>   owner;
    MONITORID       monitor     = MONITOR_INVALID;
    bool            shown       = false;
    bool            fromDesktop = false;
    Time::steady_tp changedAt;
    int             firstRow  = 0;
    int             selection = -1;
    int             pressed   = -1;
    double          axisAccum = 0.0;

    // Last frame's geometry, which the pointer and the keyboard read.
    CBox   plate;
    CBox   pill;
    double x0 = 0, y0 = 0, cellW = 1, cellH = 1, restX0 = 0;
    float  roomL = 0;      // the band's whole room once the tiles are out, logical px
    bool   full  = false;  // past the first stage: the tiles shrank and are now out
    int    cols = 1, rowsVisible = 1;

    // Touch scrolling: the rows follow a finger, then glide to a row boundary.
    // `frac` is how far the first row is scrolled up, render px. NixOs-dots docs/manual.md#one-app-drawer
    double          frac     = 0.0;
    double          extraTop = 0.0; // a fill monitor's taller cells hold the icon in their middle
    bool            fill     = false;
    CBox            view;
    double          velocity = 0.0; // px/s of offset, from the finger
    Time::steady_tp lastMove;
    bool            settling = false;
    double          settleFrom = 0.0, settleTo = 0.0;
    float           settleMs = 1.F;
    Time::steady_tp settleAt;
};

SState            g_state;
bool              g_hiding = false; // the window tiles are out, until the grid is
std::vector<SApp> g_apps;

float msSince(const Time::steady_tp& t) {
    return std::chrono::duration<float, std::milli>(Time::steadyNow() - t).count();
}

float progress() {
    const float T = std::clamp(msSince(g_state.changedAt) / FADE_MS, 0.F, 1.F);
    const float E = 1.F - std::pow(1.F - T, 3.F);
    return g_state.shown ? E : 1.F - E;
}

std::unordered_set<std::string> readHides() {
    std::unordered_set<std::string> out;
    std::vector<std::string>        files{SYSTEM_HIDES};
    if (const char* HOME = std::getenv("HOME"))
        files.emplace_back(std::string(HOME) + "/.config/omarchy/launcher.hides");
    for (const auto& FILE : files) {
        std::ifstream in(FILE);
        for (std::string line; std::getline(in, line);)
            if (!line.empty() && line[0] != '#')
                out.insert(line);
    }
    return out;
}

void reloadApps() {
    const auto HIDES = readHides();
    g_apps.clear();
    for (const auto& E : AppSearch::inventory())
        if (!E.id.empty() && !HIDES.contains(E.id) && E.id.rfind("omarchy-menu", 0) != 0)
            g_apps.push_back({E.id, E.name.empty() ? E.id : E.name});

    const auto LOWER = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return sc<char>(std::tolower(c)); });
        return s;
    };
    std::ranges::sort(g_apps, [&](const SApp& a, const SApp& b) { return LOWER(a.name) < LOWER(b.name); });
}

SP<IOverview> ownerOn(PHLMONITOR monitor) {
    if (!monitor || g_state.monitor != monitor->m_id)
        return nullptr;
    const auto OVERVIEW = g_state.owner.lock();
    return OVERVIEW && OVERVIEW == scrollOverviewForMonitor(monitor) ? OVERVIEW : nullptr;
}

void frame(PHLMONITOR monitor) {
    if (const auto OVERVIEW = ownerOn(monitor)) {
        OVERVIEW->damage();
        OVERVIEW->requestInputFrame();
    }
}

// Window tiles leave while the grid is open, as a query that matches none does.
void syncHiding(PHLMONITOR monitor) {
    if (const bool WANT = isOpen(monitor) && g_state.full; WANT != g_hiding) {
        g_hiding = WANT;
        Membership::bump("app grid");
    }
}

// Back to the overview, the grid fading out over it.
void hide(PHLMONITOR monitor) {
    g_state.shown     = false;
    g_state.changedAt = Time::steadyNow();
    g_state.pressed   = -1;
    syncHiding(monitor);
    frame(monitor);
}

int totalRows() {
    return (sc<int>(g_apps.size()) + g_state.cols - 1) / std::max(1, g_state.cols);
}

void clampScroll() {
    const int LAST = std::max(0, totalRows() - g_state.rowsVisible);
    g_state.firstRow = std::clamp(g_state.firstRow, 0, LAST);
    g_state.frac     = g_state.firstRow >= LAST ? 0.0 : std::clamp(g_state.frac, 0.0, std::max(0.0, g_state.cellH - 1.0));
}

double offsetPx() {
    return g_state.firstRow * g_state.cellH + g_state.frac;
}

double maxOffsetPx() {
    return std::max(0, totalRows() - g_state.rowsVisible) * g_state.cellH;
}

void setOffset(double px) {
    px               = std::clamp(px, 0.0, maxOffsetPx());
    g_state.firstRow = sc<int>(std::floor(px / std::max(1.0, g_state.cellH)));
    g_state.frac     = px - g_state.firstRow * g_state.cellH;
    clampScroll();
}

// A key or the wheel moves whole rows: any touch glide or half row is over.
void rowScroll() {
    g_state.frac     = 0.0;
    g_state.settling = false;
}

void scrollTo(int index) {
    rowScroll();
    const int ROW = index / std::max(1, g_state.cols);
    if (ROW < g_state.firstRow)
        g_state.firstRow = ROW;
    else if (ROW >= g_state.firstRow + g_state.rowsVisible)
        g_state.firstRow = ROW - g_state.rowsVisible + 1;
    clampScroll();
}

// Only whole rows are drawn, so a visible index always has a full cell.
bool visible(int index) {
    const int FIRST = g_state.firstRow * g_state.cols;
    const int ROWS  = g_state.rowsVisible + (g_state.frac > 0.0 ? 2 : 0); // the two a half-scrolled view shows
    return index >= FIRST && index < std::min(sc<int>(g_apps.size()), FIRST + ROWS * g_state.cols);
}

CBox cellBox(int index) {
    const int SLOT = index - g_state.firstRow * g_state.cols;
    return {g_state.x0 + sc<double>(SLOT % g_state.cols) * g_state.cellW, g_state.y0 - g_state.frac + sc<double>(SLOT / g_state.cols) * g_state.cellH, g_state.cellW, g_state.cellH};
}

CBox iconBox(const CBox& cell, float scale) {
    const double SIZE = std::round(BASE_ICON * scale);
    return {std::round(cell.x + (cell.width - SIZE) / 2.0), std::round(cell.y + g_state.extraTop + BASE_ICON_TOP * scale), SIZE, SIZE};
}

int indexAt(const Vector2D& point) {
    if (!g_state.plate.containsPoint(point) || !g_state.view.containsPoint(point))
        return -1;
    for (int i = g_state.firstRow * g_state.cols; i < sc<int>(g_apps.size()) && visible(i); ++i)
        if (cellBox(i).containsPoint(point))
            return i;
    return -1;
}

Dock::SItem itemFor(PHLMONITOR monitor, int index) {
    auto item    = Dock::appItems({g_apps[sc<size_t>(index)].id}).front();
    item.iconBox = iconBox(cellBox(index), std::max(monitor->m_scale, 0.01F) * userScale(monitor));
    return item;
}

void activate(PHLMONITOR monitor, int index) {
    Dock::activateApp(monitor, itemFor(monitor, index));
    // A launch onto an empty desktop leaves the overview up to show it land.
    if (isOpen(monitor))
        hide(monitor);
}

void openMenu(PHLMONITOR monitor, int index) {
    if (Dock::openMenuFor(monitor, itemFor(monitor, index), cellBox(index)))
        frame(monitor);
}

bool isModifier(uint32_t keysym) {
    return (keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R) || keysym == XKB_KEY_ISO_Level3_Shift || keysym == XKB_KEY_ISO_Level5_Shift ||
        keysym == XKB_KEY_Num_Lock;
}

void addRect(const CBox& box, const CHyprColor& color, int round, const CBox& clip = {}, float power = -1.F) {
    CRectPassElement::SRectData rect;
    rect.box           = box;
    rect.clipBox       = clip;
    rect.color         = color;
    rect.round         = round;
    rect.roundingPower = power > 0.F ? power : Hyprdrive::Config::themeRoundingPower();
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(rect));
}

void addTex(const SP<Render::ITexture>& tex, const CBox& box, float alpha, const CBox& clip = {}) {
    if (!tex || !tex->ok())
        return;
    CTexPassElement::SRenderData data;
    data.tex     = tex;
    data.clipBox = clip;
    data.box = box;
    data.a   = alpha;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
}

// The grid's shape on this monitor, shared by the band it asks for and the
// render that fills it.
int colsFor(PHLMONITOR monitor, bool fill) {
    const double W  = monitor->m_size.x;
    const double MG = fill ? std::max(12.0, W * FILL_SIDE) : std::max(48.0, W * SIDE);
    const float  U  = userScale(monitor);
    return std::clamp(sc<int>((W - 2 * MG - 2 * BASE_PAD * U) / (BASE_CELL_W * U)), 1, MAX_COLS);
}

// apps:label_px, the tile label size in logical px before apps:scale.
float labelPx() {
    return std::clamp(Hyprdrive::Config::getValue<float>("plugin:hyprdrive:apps:label_px"), 9.F, 16.F);
}

// A fill monitor's whole room: under the search row down to the dock, or the
// keyboard. NixOs-dots docs/manual.md#one-app-drawer
float fillRoomL(PHLMONITOR monitor) {
    const float  H     = monitor->m_size.y;
    const CBox   PILL  = Dock::layout(monitor).pill;
    const float  DOCKL = PILL.empty() || PILL.y < H * monitor->m_scale / 2.0 ? 0.F : H - sc<float>(PILL.y / monitor->m_scale);
    return std::max(0.F, H - std::max(DOCKL, Hyprdrive::KeyboardInset::easedL(monitor)) - OVERVIEW_SEARCH_ROWHL);
}

} // namespace

float roomL() {
    return g_state.roomL;
}

float bottomL(PHLMONITOR monitor) {
    if (!monitor || !fillMonitor(monitor))
        return -1.F;
    const CBox PILL = Dock::layout(monitor).pill;
    const float H   = monitor->m_size.y;
    const float DOCKL = PILL.empty() || PILL.y < H * monitor->m_scale / 2.0 ? 0.F : H - sc<float>(PILL.y / monitor->m_scale);
    return H - std::max(DOCKL, Hyprdrive::KeyboardInset::easedL(monitor));
}

bool containerUp(PHLMONITOR monitor) {
    return ownerOn(monitor) != nullptr;
}

float widthL(PHLMONITOR monitor) {
    if (!monitor)
        return 0.F;
    const double W = monitor->m_size.x;
    if (fillMonitor(monitor))
        return sc<float>(W - 2 * std::max(12.0, W * FILL_SIDE));
    const float U = userScale(monitor);
    return sc<float>(colsFor(monitor, false) * BASE_CELL_W * U + 2 * BASE_PAD * U);
}

float coverFade(PHLMONITOR monitor) {
    if (!monitor || !g_state.fill || monitor->m_id != g_state.monitor || !ownerOn(monitor))
        return 0.F;
    return progress();
}

float bandL(PHLMONITOR monitor, float withTilesL, float aloneL, float bandNowL) {
    if (!g_state.shown || !ownerOn(monitor))
        return 0.F;
    if (fillMonitor(monitor))
        aloneL = std::max(aloneL, fillRoomL(monitor));
    g_state.roomL = aloneL;
    g_state.fill  = fillMonitor(monitor);
    // One ease to the whole room; the tiles shrink until it passes theirs, then leave.
    if (!g_state.full && bandNowL >= withTilesL - 1.F) {
        g_state.full = true;
        syncHiding(monitor);
    }
    return aloneL;
}

bool hidesWindow(const PHLWINDOW& window) {
    const auto MONITOR = g_hiding && window ? window->m_monitor.lock() : nullptr;
    return MONITOR && MONITOR->m_id == g_state.monitor;
}

bool isOpen(PHLMONITOR monitor) {
    const auto OVERVIEW = ownerOn(monitor);
    return g_state.shown && OVERVIEW && !OVERVIEW->isClosing();
}

bool ownsPointer(PHLMONITOR monitor, const Vector2D& point) {
    return isOpen(monitor) && g_state.plate.containsPoint(point) && !Dock::uiContains(monitor, point) && !g_state.pill.containsPoint(point);
}

void open(PHLMONITOR monitor) {
    if (!monitor || isOpen(monitor))
        return;

    // The query goes either way: a results pane would sit on top of the grid.
    resetSearchSession();
    auto       overview    = openScrollOverviewForMonitor(monitor);
    const bool FROMDESKTOP = !overview;
    if (!overview) {
        if (!openScrollOverview(monitor) || !(overview = openScrollOverviewForMonitor(monitor)))
            return;
    }

    Dock::closeMenu();
    g_state             = SState{};
    g_state.owner       = overview;
    g_state.monitor     = monitor->m_id;
    g_state.shown       = true;
    g_state.fromDesktop = FROMDESKTOP;
    g_state.changedAt   = Time::steadyNow();
    reloadApps();
    syncHiding(monitor);
    frame(monitor);
}

void close(PHLMONITOR monitor) {
    if (!isOpen(monitor))
        return;
    Dock::closeMenu();
    // Opened from the desktop, the grid was the whole errand: it fades out
    // with the overview instead of revealing it.
    if (g_state.fromDesktop) {
        // Tiles back first, so the overview's close has windows to zoom into.
        g_state.shown = false;
        syncHiding(monitor);
        if (const auto OVERVIEW = ownerOn(monitor))
            OVERVIEW->close();
        return;
    }
    hide(monitor);
}

void toggle(PHLMONITOR monitor) {
    if (isOpen(monitor))
        close(monitor);
    else
        open(monitor);
}

void render(PHLMONITOR monitor, float fade, const CBox& searchPill, const CBox& container, const CBox& rest) {
    const auto OVERVIEW = ownerOn(monitor);
    if (!OVERVIEW)
        return;

    // The overview can close under the grid (a launch, its own toggle).
    syncHiding(monitor);
    if (msSince(g_state.changedAt) < FADE_MS)
        frame(monitor);
    // The plate is the search's own container (it grows out of the pill and folds back into it);
    // only the cells fade. NixOs-dots docs/manual.md#one-app-drawer
    const float ALPHA = progress() * fade;
    if (ALPHA < 0.005F || container.width < 2 || container.height < 2 || rest.width < 2 || rest.height < 2)
        return;

    const float  S   = std::max(monitor->m_scale, 0.01F) * userScale(monitor);
    const double CH0 = std::round(BASE_CELL_H * S), P = std::round(BASE_PAD * S);

    g_state.pill = searchPill;
    // Cells are laid out against the container's resting box, so they never reflow while it grows or
    // folds; its growing edge clips them. The last open shape is kept while folding back.
    if (g_state.shown) {
        g_state.fill = fillMonitor(monitor);
        const double INW = rest.width - 2 * P, INH = rest.height - 2 * P;
        g_state.cols     = std::clamp(sc<int>((INW + 1.0) / (BASE_CELL_W * S)), 1, MAX_COLS);
        g_state.rowsVisible = std::max(1, sc<int>(INH / CH0));
        g_state.cellW    = std::max(1.0, std::floor(INW / g_state.cols));
        g_state.cellH    = g_state.fill && INH >= CH0 ? std::floor(INH / g_state.rowsVisible) : CH0;
        g_state.extraTop = std::round((g_state.cellH - CH0) / 2.0);
        g_state.restX0   = std::round(rest.x + P + (INW - g_state.cols * g_state.cellW) / 2.0);
    }
    const double CH = g_state.cellH;
    if (container.height < 2 * P)
        return;

    g_state.plate = container;
    g_state.x0    = g_state.restX0;
    g_state.y0    = container.y + P;
    g_state.view  = CBox{container.x, g_state.y0, container.width, std::max(0.0, container.height - 2 * P)};

    // A touch glide runs here, a frame at a time, toward its row boundary.
    if (g_state.settling) {
        const float T = std::clamp(msSince(g_state.settleAt) / g_state.settleMs, 0.F, 1.F);
        setOffset(g_state.settleFrom + (g_state.settleTo - g_state.settleFrom) * (1.F - std::pow(1.F - T, 3.F)));
        if (T >= 1.F)
            g_state.settling = false;
        frame(monitor);
    }
    clampScroll();

    // The dock's radius family: the tile highlight takes two thirds of it.
    const int ROUND = std::max(2, sc<int>(std::round(Hyprdrive::Config::dockCornerRadius() * monitor->m_scale * 2.F / 3.F)));

    const auto       INK = Hyprdrive::Config::themeForeground();
    const CHyprColor LABEL{INK.r, INK.g, INK.b, 0.95F};
    const auto       MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
    const int        HOVER = Dock::menuOpen() ? -1 : indexAt(MOUSE);
    const int        FONT  = std::max(1, sc<int>(std::round(labelPx() * S)));

    // Any row that reaches into the view, clipped to it: the container is still growing, or mid-scroll.
    const double     VIEWEND  = g_state.view.y + g_state.view.height;
    std::vector<int> indices;
    for (int i = g_state.firstRow * g_state.cols; i < sc<int>(g_apps.size()) && visible(i); ++i)
        if (cellBox(i).y + CH > g_state.view.y && cellBox(i).y < VIEWEND)
            indices.push_back(i);

    std::vector<std::string> ids;
    ids.reserve(indices.size());
    for (const int I : indices)
        ids.push_back(g_apps[sc<size_t>(I)].id);
    auto items = Dock::appItems(ids);

    for (size_t k = 0; k < indices.size(); ++k) {
        const int  I    = indices[k];
        const CBox CELL = cellBox(I);
        auto&      item = items[k];
        item.iconBox    = iconBox(CELL, S);

        if (I == HOVER || I == g_state.selection) {
            const double INSET = std::round(4.F * S);
            addRect(CBox{CELL.x + INSET, CELL.y + INSET, CELL.width - 2 * INSET, CELL.height - 2 * INSET}, INK.modifyA((I == g_state.selection ? 0.16F : 0.10F) * ALPHA), ROUND, g_state.view, 2.F);
        }

        addTex(OverviewRender::appIcon(item.icon.empty() ? "application-x-executable" : item.icon), item.iconBox, ALPHA, g_state.view);

        const auto TEXT = OverviewRender::text(g_apps[sc<size_t>(I)].name, LABEL, FONT, false, "", sc<int>(CELL.width - std::round(12.F * S)), 500);
        if (TEXT && TEXT->ok()) {
            const double TW = TEXT->m_size.x, TH = TEXT->m_size.y;
            addTex(TEXT, CBox{std::round(CELL.x + (CELL.width - TW) / 2.0), std::round(item.iconBox.y + item.iconBox.height + BASE_LABEL_Y * S), TW, TH}, ALPHA, g_state.view);
        }
    }
    // (the dots are not clipped: a row cut by the view has none)
    for (size_t k = items.size(); k-- > 0;)
        if (cellBox(indices[k]).y < g_state.view.y || cellBox(indices[k]).y + CH > VIEWEND)
            items.erase(items.begin() + sc<long>(k));
    Dock::renderAppDots(monitor, items, ALPHA);

    // A thin scroll thumb when not every row fits.
    if (const int TOTAL = totalRows(); TOTAL > g_state.rowsVisible) {
        const double TRACK = g_state.plate.height - 2 * P, BARW = std::round(4.F * S);
        const double THUMB = std::max(std::round(24.0 * S), TRACK * g_state.rowsVisible / TOTAL);
        const double Y     = g_state.plate.y + P + (TRACK - THUMB) * offsetPx() / std::max(1.0, maxOffsetPx());
        addRect(CBox{g_state.plate.x + g_state.plate.width - P / 2.0 - BARW / 2.0, std::round(Y), BARW, std::round(THUMB)}, INK.modifyA(0.35F * ALPHA), std::min(ROUND, sc<int>(BARW / 2.0)));
    }
}

bool handleButton(PHLMONITOR monitor, const Vector2D& point, uint32_t button, bool pressed, uint32_t mainButton, uint32_t menuButton) {
    if (!isOpen(monitor))
        return false;
    // The dock and the search field stay live over the grid.
    if (Dock::uiContains(monitor, point) || g_state.pill.containsPoint(point))
        return false;

    const int INDEX = indexAt(point);
    // Outside the plate is the overview's: the click lands and the grid steps aside.
    if (!g_state.plate.containsPoint(point)) {
        if (pressed) {
            g_state.fromDesktop = false;
            hide(monitor);
        }
        return false;
    }
    if (INDEX < 0)
        return true;

    if (button == menuButton) {
        if (pressed)
            openMenu(monitor, INDEX);
    } else if (button == mainButton) {
        if (pressed)
            g_state.pressed = INDEX;
        else if (g_state.pressed == INDEX)
            activate(monitor, INDEX);
    } else if (button == BTN_MIDDLE && !pressed) {
        Dock::middleClickApp(monitor, itemFor(monitor, INDEX));
        if (isOpen(monitor))
            hide(monitor);
    }
    if (!pressed)
        g_state.pressed = -1;
    frame(monitor);
    return true;
}

bool handleAxis(PHLMONITOR monitor, double delta, bool wheel) {
    if (!isOpen(monitor) || !g_state.plate.containsPoint((g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale))
        return false;
    // A notch is a row; a touchpad scrolls a row per ~40 units of travel.
    int rows = 0;
    if (wheel)
        rows = delta > 0 ? 1 : delta < 0 ? -1 : 0;
    else {
        g_state.axisAccum += delta;
        rows = sc<int>(g_state.axisAccum / 40.0);
        g_state.axisAccum -= rows * 40.0;
    }
    if (rows) {
        rowScroll();
        g_state.firstRow += rows;
        clampScroll();
        frame(monitor);
    }
    return true;
}

bool touchScrollable(PHLMONITOR monitor, const Vector2D& point) {
    return ownsPointer(monitor, point) && totalRows() > g_state.rowsVisible;
}

void touchScroll(PHLMONITOR monitor, double dy) {
    const auto NOW = Time::steadyNow();
    const double MS = std::chrono::duration<double, std::milli>(NOW - g_state.lastMove).count();
    // The release speed is a smoothed finger speed; a pause (a stale reading) forgets it.
    const double INSTANT = MS > 0.5 && MS < 100.0 ? -dy / MS * 1000.0 : 0.0;
    g_state.velocity     = MS < 100.0 ? 0.6 * INSTANT + 0.4 * g_state.velocity : 0.0;
    g_state.lastMove     = NOW;
    g_state.settling     = false;
    setOffset(offsetPx() - dy);
    frame(monitor);
}

void touchScrollEnd(PHLMONITOR monitor) {
    const double SPEED = msSince(g_state.lastMove) > 80.F ? 0.0 : g_state.velocity;
    g_state.velocity   = 0.0;
    const double NOW   = offsetPx();
    const double CELL  = std::max(1.0, g_state.cellH);
    const double TO    = std::clamp(std::round((NOW + SPEED * MOMENTUM_S) / CELL) * CELL, 0.0, maxOffsetPx());
    if (std::abs(TO - NOW) < 1.0) {
        setOffset(TO);
    } else {
        g_state.settling   = true;
        g_state.settleFrom = NOW;
        g_state.settleTo   = TO;
        g_state.settleAt   = Time::steadyNow();
        g_state.settleMs   = std::clamp(180.F + sc<float>(std::abs(TO - NOW) / CELL) * 45.F, 180.F, 420.F);
    }
    frame(monitor);
}

bool handleKey(PHLMONITOR monitor, uint32_t keysym) {
    if (!isOpen(monitor))
        return false;
    if (Dock::menuOpen()) {
        if (keysym == XKB_KEY_Escape) {
            Dock::closeMenu();
            frame(monitor);
        }
        return true;
    }

    const int N    = sc<int>(g_apps.size());
    const int COLS = std::max(1, g_state.cols);
    const int PAGE = COLS * std::max(1, g_state.rowsVisible);
    const auto move = [&](int by) {
        g_state.selection = g_state.selection < 0 ? 0 : std::clamp(g_state.selection + by, 0, std::max(0, N - 1));
        scrollTo(g_state.selection);
        frame(monitor);
        return true;
    };

    switch (keysym) {
        case XKB_KEY_Escape: close(monitor); return true;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left: return move(-1);
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right: return move(1);
        case XKB_KEY_Up:
        case XKB_KEY_KP_Up: return move(-COLS);
        case XKB_KEY_Down:
        case XKB_KEY_KP_Down: return move(COLS);
        case XKB_KEY_Page_Up: return move(-PAGE);
        case XKB_KEY_Page_Down: return move(PAGE);
        case XKB_KEY_Home: return move(-N);
        case XKB_KEY_End: return move(N);
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (g_state.selection >= 0 && g_state.selection < N && visible(g_state.selection))
                activate(monitor, g_state.selection);
            return true;
        case XKB_KEY_Menu:
            if (g_state.selection >= 0 && g_state.selection < N && visible(g_state.selection))
                openMenu(monitor, g_state.selection);
            return true;
        default: break;
    }

    if (isModifier(keysym))
        return false;
    // Anything else is typing: the grid steps aside and the search takes the key.
    g_state.fromDesktop = false;
    hide(monitor);
    return false;
}

} // namespace Hyprdrive::AppGrid
