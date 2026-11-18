// NixOS dots: the drag hold draws the window smaller through its animated values, which
// are protected; opened as Dock.cpp opens them, standard headers first.
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#define private public
#define protected public
#include <hyprland/src/desktop/view/Window.hpp>
#undef protected
#undef private
// After the block, as FlightController.cpp includes it: the flight's own card shadow.
#include "OverviewChrome.hpp"

#include "HotCorner.hpp"
#include "HotCornerModel.hpp"

#include "Config.hpp"
#include "Dock.hpp"
#include "DockDrop.hpp"
#include "IOverview.hpp"
#include "Ripple.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/protocols/PointerConstraints.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace Hyprdrive::HotCorner {

    namespace {
        constexpr const char* KEY_ENABLED = "plugin:hyprdrive:hot_corner:enabled";
        constexpr const char* KEY_CORNER  = "plugin:hyprdrive:hot_corner:corner";
        constexpr const char* KEY_CORNER2 = "plugin:hyprdrive:hot_corner:corner2";
        constexpr const char* KEY_DELAY   = "plugin:hyprdrive:hot_corner:delay_ms";
        constexpr const char* KEY_RIPPLE  = "plugin:hyprdrive:hot_corner:ripple";

        CHyprSignalListener g_move, g_button, g_dragRender;
        wl_event_source*    g_timer = nullptr;

        int  g_buttonsDown = 0;
        // Waiting out the delay for the pointer resting in the corner.
        bool g_pending = false;
        // Fired (or cancelled by a click) on this visit: the pointer must leave
        // the corner before it can fire again, or an overview closed with the
        // pointer still parked there would reopen at once.
        bool g_latched = false;

        SRect rectOf(const CBox& box) {
            return SRect{box.x, box.y, box.w, box.h};
        }

        // The monitor whose OUTER chosen corner the pointer is in, or nullptr.
        PHLMONITOR cornerMonitor(const Vector2D& point, ECorner corner) {
            for (const auto& M : State::monitorState()->monitors()) {
                if (!M || !M->m_enabled)
                    continue;
                const auto BOX = rectOf(M->logicalBox());
                if (!inCorner(point.x, point.y, BOX, corner))
                    continue;
                for (const auto& P : probes(BOX, corner))
                    for (const auto& OTHER : State::monitorState()->monitors())
                        if (OTHER && OTHER != M && OTHER->m_enabled && OTHER->logicalBox().containsPoint(Vector2D{P.x, P.y}))
                            return nullptr;
                return M;
            }
            return nullptr;
        }

        // The corner the pointer is in, of the two the settings name, and its screen.
        struct SCornerHit {
            PHLMONITOR monitor;
            ECorner    corner;
        };
        SCornerHit cornerHit(const Vector2D& point) {
            for (const char* KEY : {KEY_CORNER, KEY_CORNER2}) {
                const auto NAME = Config::getValue<std::string>(KEY);
                if (NAME.empty())
                    continue;
                const auto CORNER = cornerFromName(NAME);
                if (const auto M = cornerMonitor(point, CORNER))
                    return {M, CORNER};
            }
            return {nullptr, ECorner{}};
        }

        bool pointerLocked() {
            if (!g_pInputManager)
                return false;
            return std::ranges::any_of(g_pInputManager->m_constraints, [](const auto& c) {
                const auto C = c.lock();
                return C && C->isActive();
            });
        }

        // An overview is up: the corner closes it (user, 2026-10-13: "hitting it
        // again should close overview if overview is open"), as GNOME's does.
        bool overviewUp() {
            return !scrollOverviews().empty();
        }

        // Every reason the corner must stay quiet, asked again when it fires:
        // the world can change during the delay.
        bool blocked(const PHLMONITOR& monitor, const Vector2D& point) {
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return true;
            // A held button is a drag (a window in the overview, a file, a
            // selection) on its way somewhere, never a trip to the corner.
            if (g_buttonsDown > 0)
                return true;
            if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return true;
            if (g_layoutManager && g_layoutManager->dragController() && g_layoutManager->dragController()->target())
                return true;
            if (DockDrop::dndInProgress())
                return true;
            if (overviewUp()) {
                // Closing already, or mid-swipe: a toggle now would reverse it.
                for (const auto& OV : scrollOverviews())
                    if (OV && (OV->isClosing() || OV->m_isSwiping))
                        return true;
                return false;
            }
            if (pointerLocked())
                return true;
            // A fullscreen video or game owns that screen's corners too.
            if (Fullscreen::controller()->hasFullscreen(monitor))
                return true;
            const Vector2D LOCAL = (point - monitor->m_position) * monitor->m_scale;
            return Dock::occupiesPointer(monitor, LOCAL);
        }

        void cancel() {
            g_pending = false;
            if (g_timer)
                wl_event_source_timer_update(g_timer, 0);
        }

        void toggleOverview() {
            // Outside the motion emit (a fresh overview registers its own motion
            // listener), and through the dispatcher so it is exactly a Super tap:
            // opens it everywhere, or closes it where it is open everywhere.
            if (!g_pKeybindManager)
                return;
            const auto IT = g_pKeybindManager->m_dispatchers.find("hyprdrive:overview");
            if (IT != g_pKeybindManager->m_dispatchers.end())
                IT->second("toggle all");
        }

        // The screen corner itself (global logical px): where the ripple starts.
        Vector2D cornerPoint(const PHLMONITOR& monitor, ECorner corner) {
            const auto BOX  = monitor->logicalBox();
            const bool LEFT = corner == ECorner::TopLeft || corner == ECorner::BottomLeft;
            const bool TOP  = corner == ECorner::TopLeft || corner == ECorner::TopRight;
            return Vector2D{LEFT ? BOX.x : BOX.x + BOX.w, TOP ? BOX.y : BOX.y + BOX.h};
        }

        int fire(void*) {
            if (!g_pending)
                return 0;
            g_pending = false;
            if (!Config::getValue<bool>(KEY_ENABLED) || !g_pInputManager)
                return 0;
            const auto POINT   = g_pInputManager->getMouseCoordsInternal();
            const auto HIT = cornerHit(POINT);
            if (!HIT.monitor || blocked(HIT.monitor, POINT))
                return 0;
            g_latched = true;
            if (Config::getValue<bool>(KEY_RIPPLE))
                Ripple::trigger(HIT.monitor, cornerPoint(HIT.monitor, HIT.corner));
            toggleOverview();
            return 0;
        }

        void onMove() {
            if (!g_pInputManager || !Config::getValue<bool>(KEY_ENABLED)) {
                cancel();
                g_latched = false;
                return;
            }
            const auto POINT   = g_pInputManager->getMouseCoordsInternal();
            const auto HIT     = cornerHit(POINT);
            const auto MONITOR = HIT.monitor;
            if (!MONITOR) {
                cancel();
                g_latched = false;
                return;
            }
            if (g_latched || g_pending || blocked(MONITOR, POINT))
                return;
            g_pending = true;
            // 0 disarms a wl timer, so "no delay" is the next loop turn.
            wl_event_source_timer_update(g_timer, std::clamp<int>(Config::getValue<int>(KEY_DELAY), 1, 2000));
        }

        // DRAG TO THE SIDE (NixOS dots): a window carried near the sides island's zone opens it
        // on its workspaces and shrinks as Hyprdrive's drag does. docs/manual.md#dragging-a-window-to-the-sides
        constexpr int    DRAG_TICK_MS = 16;
        constexpr double DRAG_NEAR_PX = 120.0; // logical: how close to the zone opens the island
        constexpr double DRAG_LEAVE   = 0.45;  // of the monitor's width: further in closes it again
        wl_event_source* g_dragTimer  = nullptr;
        bool             g_islandDrag = false; // the island is open on this drag
        CBox             g_dragBox;            // the monitor it opened on, logical
        bool             g_dragRight = true;   // which outer edge of it the sides island sits on
        Vector2D         g_dragLast;           // the pointer as last told to the island
        float            g_miniFrom = 0.F, g_miniTo = 0.F; // 0 full size, 1 mini
        double           g_dragK    = 0.0; // the island's cell/monitor ratio; 0 until it says, then drag_mini_scale
        Time::steady_tp  g_miniAt;
        PHLWINDOWREF     g_dragShrunk; // drawn smaller than its layout says, until restored

        PHLMONITOR islandMonitor(const Vector2D& point, double reach);

        // The window Hyprland's own move bind is carrying, if any.
        PHLWINDOW movedWindow() {
            const auto* CTRL = g_layoutManager ? g_layoutManager->dragController().get() : nullptr;
            if (!CTRL || CTRL->mode() != MBIND_MOVE || g_buttonsDown < 1)
                return nullptr;
            const auto TARGET = CTRL->target();
            return TARGET ? TARGET->window() : nullptr;
        }

        bool dragBlocked(const PHLMONITOR& monitor) {
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return true;
            if (overviewUp() || DockDrop::dndInProgress())
                return true;
            if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return true;
            return Fullscreen::controller()->hasFullscreen(monitor);
        }

        // How far to mini, on Hyprdrive's drag_mini_ms and its reflow-style ease.
        float miniProgress() {
            const int   MS = Config::getDragMiniMs();
            const float T  = MS <= 0 ? 1.F : std::clamp(std::chrono::duration<float, std::milli>(Time::steadyNow() - g_miniAt).count() / MS, 0.F, 1.F);
            return g_miniFrom + (g_miniTo - g_miniFrom) * sc<float>(1.0 - std::pow(1.0 - T, 3.0));
        }
        void miniTo(float to) {
            if (to == g_miniTo)
                return;
            g_miniFrom = miniProgress();
            g_miniTo   = to;
            g_miniAt   = Time::steadyNow();
            if (g_dragTimer)
                wl_event_source_timer_update(g_dragTimer, 1);
        }

        // Back to the size and place its layout already holds.
        void dragRestore() {
            if (const auto WINDOW = g_dragShrunk.lock(); WINDOW && WINDOW->m_realSize && WINDOW->m_realPosition) {
                g_pHyprRenderer->damageWindow(WINDOW);
                WINDOW->m_realSize->warp();
                WINDOW->m_realPosition->warp();
                g_pHyprRenderer->damageWindow(WINDOW);
            }
            g_dragShrunk.reset();
        }

        void dragStop() {
            if (g_islandDrag && g_pEventManager)
                g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdriveislanddragend", .data = ""});
            g_islandDrag = false;
            g_dragK      = 0.0;
            g_miniFrom = g_miniTo = 0.F;
            if (g_dragTimer)
                wl_event_source_timer_update(g_dragTimer, 0);
            dragRestore();
        }

        // Drawn, not laid out: the values move, the goals the drag sets stay, as the dock's tear-out does.
        void dragShrink(const PHLWINDOW& window, const Vector2D& pointer, float progress, bool damage = true) {
            if (!window->m_realSize || !window->m_realPosition)
                return;
            const Vector2D SIZE = window->m_realSize->goal(), POS = window->m_realPosition->goal();
            // What lands in a cell, as Hyprdrive sizes its proxy over the strip.
            const double   MINI  = g_dragK > 0.0 ? g_dragK : Config::getDragMiniScale();
            const double   SCALE = 1.0 - (1.0 - MINI) * progress;
            if (damage)
                g_pHyprRenderer->damageWindow(window);
            window->m_realSize->m_Value     = SIZE * SCALE;
            window->m_realPosition->m_Value = pointer - (pointer - POS) * SCALE; // about the pointer: the grab stays put
            if (damage)
                g_pHyprRenderer->damageWindow(window);
            g_dragShrunk = window;
        }

        int dragFire(void*) {
            const auto WINDOW = movedWindow();
            if (!WINDOW || !g_pInputManager) {
                dragStop();
                return 0;
            }
            const float P = miniProgress();
            if (P <= 0.F && g_miniTo <= 0.F) {
                dragRestore();
                return 0;
            }
            dragShrink(WINDOW, g_pInputManager->getMouseCoordsInternal(), P);
            wl_event_source_timer_update(g_dragTimer, DRAG_TICK_MS);
            return 0;
        }

        // THE SIDES (NixOS dots, docs/manual.md#the-hyprdrive-sides): the middle band of one monitor's
        // outer left or right edge, which the user chose. Only the outermost edge counts: a monitor
        // beside it puts the island where a window would be dropped on the other screen, so it is refused.
        constexpr const char* KEY_SIDES        = "plugin:hyprdrive:sides:enabled";
        constexpr const char* KEY_SIDES_LAYOUT = "plugin:hyprdrive:sides:layout";
        constexpr const char* KEY_SIDES_HEIGHT = "plugin:hyprdrive:sides:zone_height";
        constexpr const char* KEY_SIDES_DELAY  = "plugin:hyprdrive:sides:delay_ms";

        // The layout names the sides each screen's island is on, as "DP-1:left,DP-2:both".
        bool layoutHas(const PHLMONITOR& monitor, bool right) {
            const auto LAYOUT = Config::getValue<std::string>(KEY_SIDES_LAYOUT);
            size_t     pos    = 0;
            while (pos <= LAYOUT.size()) {
                auto end = LAYOUT.find(',', pos);
                if (end == std::string::npos)
                    end = LAYOUT.size();
                const auto ENTRY = LAYOUT.substr(pos, end - pos);
                pos              = end + 1;
                const auto COLON = ENTRY.rfind(':');
                if (COLON == std::string::npos || ENTRY.substr(0, COLON) != monitor->m_name)
                    continue;
                const auto SIDE = ENTRY.substr(COLON + 1);
                if (SIDE == "both" || SIDE == (right ? "right" : "left"))
                    return true;
            }
            return false;
        }

        // The middle band of one outer edge, for a side the layout names, that the pointer is on.
        bool sideAt(const Vector2D& point, double reach, PHLMONITOR& monitorOut, bool& rightOut) {
            if (!Config::getValue<bool>(KEY_SIDES))
                return false;
            for (const auto& M : State::monitorState()->monitors()) {
                if (!M || !M->m_enabled)
                    continue;
                const auto BOX     = M->logicalBox();
                const double MID_Y = BOX.y + BOX.h / 2.0;
                const double HALF  = std::max(40, Config::getValue<int>(KEY_SIDES_HEIGHT)) / 2.0 + reach;
                if (std::abs(point.y - MID_Y) > HALF || !BOX.containsPoint(point))
                    continue;
                for (const bool RIGHT : {false, true}) {
                    if (!layoutHas(M, RIGHT))
                        continue;
                    if (RIGHT ? point.x < BOX.x + BOX.w - reach : point.x > BOX.x + reach)
                        continue;
                    // Only an outer edge: a screen beside it would take the island's place.
                    const Vector2D PROBE{RIGHT ? BOX.x + BOX.w + 1 : BOX.x - 1, MID_Y};
                    bool           taken = false;
                    for (const auto& OTHER : State::monitorState()->monitors())
                        if (OTHER && OTHER != M && OTHER->m_enabled && OTHER->logicalBox().containsPoint(PROBE))
                            taken = true;
                    if (taken)
                        continue;
                    monitorOut = M;
                    rightOut   = RIGHT;
                    return true;
                }
            }
            return false;
        }

        // The event's data: the screen and the side, "DP-1,right".
        std::string sideData(const PHLMONITOR& monitor, bool right) {
            return monitor->m_name + (right ? ",right" : ",left");
        }

        void dragMove() {
            const auto WINDOW = movedWindow();
            if (!WINDOW || !g_pInputManager) {
                if (g_islandDrag || g_dragShrunk)
                    dragStop();
                return;
            }
            const auto POINT = g_pInputManager->getMouseCoordsInternal();

            if (!g_islandDrag) {
                PHLMONITOR MONITOR;
                bool       RIGHT = true;
                if (!sideAt(POINT, DRAG_NEAR_PX, MONITOR, RIGHT) || dragBlocked(MONITOR) || !g_pEventManager)
                    return;
                g_islandDrag = true;
                g_dragBox    = MONITOR->logicalBox();
                g_dragRight  = RIGHT;
                g_dragLast   = POINT;
                g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdriveislanddrag", .data = sideData(MONITOR, RIGHT)});
                miniTo(1.F);
                return;
            }

            // Carried well inward, away from the edge: it closes and the window grows back.
            const double INWARD = g_dragRight ? g_dragBox.x + g_dragBox.w - POINT.x : POINT.x - g_dragBox.x;
            if (INWARD > g_dragBox.w * DRAG_LEAVE) {
                if (g_pEventManager)
                    g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdriveislanddragend", .data = ""});
                g_islandDrag = false;
                miniTo(0.F);
                return;
            }
            if (POINT != g_dragLast && g_pEventManager) {
                g_dragLast = POINT;
                g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdriveislanddraghover", .data = std::format("{:.0f},{:.0f}", POINT.x, POINT.y)});
            }
        }
        // THE ISLAND (NixOS dots, docs/manual.md#the-hyprdrive-island): the
        // pointer resting on the middle of a top edge tells the shell to open it.
        constexpr const char* KEY_ISLAND       = "plugin:hyprdrive:island:enabled";
        constexpr const char* KEY_ISLAND_DELAY = "plugin:hyprdrive:island:delay_ms";
        constexpr const char* KEY_ISLAND_WIDTH = "plugin:hyprdrive:island:zone_width";

        wl_event_source* g_islandTimer   = nullptr;
        bool             g_islandPending = false;
        bool             g_islandLatched = false;

        // The monitor whose outer top edge the pointer is on, within the middle band.
        PHLMONITOR islandMonitor(const Vector2D& point, double reach) {
            const double HALF = std::max(40, Config::getValue<int>(KEY_ISLAND_WIDTH)) / 2.0 + reach;
            for (const auto& M : State::monitorState()->monitors()) {
                if (!M || !M->m_enabled)
                    continue;
                const auto BOX = M->logicalBox();
                if (!BOX.containsPoint(point) || point.y > BOX.y + EDGE_PX + reach || std::abs(point.x - (BOX.x + BOX.w / 2.0)) > HALF)
                    continue;
                for (const auto& OTHER : State::monitorState()->monitors())
                    if (OTHER && OTHER != M && OTHER->m_enabled && OTHER->logicalBox().containsPoint(Vector2D{point.x, BOX.y - 1}))
                        return nullptr;
                return M;
            }
            return nullptr;
        }

        bool islandBlocked(const PHLMONITOR& monitor) {
            if (g_pSessionLockManager && g_pSessionLockManager->isSessionLocked())
                return true;
            if (g_buttonsDown > 0 || overviewUp() || pointerLocked() || DockDrop::dndInProgress())
                return true;
            if (g_pUnifiedWorkspaceSwipe && g_pUnifiedWorkspaceSwipe->isGestureInProgress())
                return true;
            if (g_layoutManager && g_layoutManager->dragController() && g_layoutManager->dragController()->target())
                return true;
            return Fullscreen::controller()->hasFullscreen(monitor);
        }

        void islandCancel() {
            g_islandPending = false;
            if (g_islandTimer)
                wl_event_source_timer_update(g_islandTimer, 0);
        }

        int islandFire(void*) {
            if (!g_islandPending)
                return 0;
            g_islandPending = false;
            if (!Config::getValue<bool>(KEY_ISLAND) || !g_pInputManager || !g_pEventManager)
                return 0;
            const auto MONITOR = islandMonitor(g_pInputManager->getMouseCoordsInternal(), 0.0);
            if (!MONITOR || islandBlocked(MONITOR))
                return 0;
            g_islandLatched = true;
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdriveisland", .data = MONITOR->m_name});
            return 0;
        }

        // THE SIDES AT REST (NixOS dots, docs/manual.md#the-hyprdrive-sides): the pointer resting on the
        // chosen edge's middle opens the sides island, as the notch opens from its edge. Once per visit.
        wl_event_source* g_sidesTimer   = nullptr;
        bool             g_sidesPending = false;
        bool             g_sidesLatched = false;

        void sidesCancel() {
            g_sidesPending = false;
            if (g_sidesTimer)
                wl_event_source_timer_update(g_sidesTimer, 0);
        }

        int sidesFire(void*) {
            if (!g_sidesPending)
                return 0;
            g_sidesPending = false;
            if (!Config::getValue<bool>(KEY_SIDES) || !g_pInputManager || !g_pEventManager)
                return 0;
            PHLMONITOR MONITOR;
            bool       RIGHT = true;
            if (!sideAt(g_pInputManager->getMouseCoordsInternal(), EDGE_PX, MONITOR, RIGHT) || islandBlocked(MONITOR))
                return 0;
            g_sidesLatched = true;
            g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdrivesides", .data = sideData(MONITOR, RIGHT)});
            return 0;
        }

        void sidesMove() {
            PHLMONITOR MONITOR;
            bool       RIGHT = true;
            if (!g_pInputManager || !sideAt(g_pInputManager->getMouseCoordsInternal(), EDGE_PX, MONITOR, RIGHT)) {
                sidesCancel();
                g_sidesLatched = false;
                return;
            }
            if (g_sidesLatched || g_sidesPending || islandBlocked(MONITOR))
                return;
            g_sidesPending = true;
            wl_event_source_timer_update(g_sidesTimer, std::clamp<int>(Config::getValue<int>(KEY_SIDES_DELAY), 1, 2000));
        }

        void islandMove() {
            const auto MONITOR = g_pInputManager && Config::getValue<bool>(KEY_ISLAND) ? islandMonitor(g_pInputManager->getMouseCoordsInternal(), 0.0) : nullptr;
            if (!MONITOR) {
                islandCancel();
                g_islandLatched = false;
                return;
            }
            if (g_islandLatched || g_islandPending || islandBlocked(MONITOR))
                return;
            g_islandPending = true;
            wl_event_source_timer_update(g_islandTimer, std::clamp<int>(Config::getValue<int>(KEY_ISLAND_DELAY), 1, 2000));
        }
    }

    static void renderDragProxy(const PHLMONITOR& monitor);

    void init() {
        if (!g_timer)
            g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, fire, nullptr);
        if (!g_islandTimer)
            g_islandTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, islandFire, nullptr);
        if (!g_dragTimer)
            g_dragTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, dragFire, nullptr);
        if (!g_sidesTimer)
            g_sidesTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, sidesFire, nullptr);

        g_dragRender = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
            // Hyprland's move puts the window back to full size on every motion; each frame shrinks it again.
            if (stage == RENDER_PRE && g_pInputManager)
                if (const auto WINDOW = g_dragShrunk.lock())
                    dragShrink(WINDOW, g_pInputManager->getMouseCoordsInternal(), miniProgress(), false);
            if (stage == RENDER_LAST_MOMENT && g_dragShrunk)
                renderDragProxy(g_pHyprRenderer->m_renderData.pMonitor.lock());
        });

        g_move = Event::bus()->m_events.input.mouse.move.listen([](Vector2D, Event::SCallbackInfo&) {
            onMove();
            islandMove();
            sidesMove();
            dragMove();
        });

        g_button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent e, Event::SCallbackInfo&) {
            if (e.state == WL_POINTER_BUTTON_STATE_PRESSED) {
                ++g_buttonsDown;
                // A click in the corner was meant for whatever is there (the
                // bar's menu button): this visit is spent.
                if (g_pending) {
                    cancel();
                    g_latched = true;
                }
                if (g_islandPending) {
                    islandCancel();
                    g_islandLatched = true;
                }
                if (g_sidesPending) {
                    sidesCancel();
                    g_sidesLatched = true;
                }
                return;
            }
            // Before Hyprland's own release handling, which ends its drag after this.
            islandRelease();
            g_buttonsDown = std::max(0, g_buttonsDown - 1);
        });
    }

    // Let go over the island: it moves the window where it landed, which grows back
    // where Hyprland drops it. The dock calls this before ending its own tear-out drag.
    void islandRelease() {
        if (g_islandDrag && g_pEventManager && g_pInputManager) {
            const auto WINDOW = movedWindow();
            const auto POINT  = g_pInputManager->getMouseCoordsInternal();
            if (WINDOW)
                g_pEventManager->postEvent(SHyprIPCEvent{.event = "hyprdriveislanddrop",
                                                         .data  = std::format("0x{:x},{:.0f},{:.0f}", reinterpret_cast<uintptr_t>(WINDOW.get()), POINT.x, POINT.y)});
        }
        if (g_islandDrag || g_dragShrunk) {
            g_islandDrag = false;
            dragStop();
        }
    }

    // The carried window again, over every layer: the island is a top layer and Hyprland
    // draws windows beneath those. Drawn as a flight's proxy is. docs/manual.md#dragging-a-window-to-the-sides
    static void renderDragProxy(const PHLMONITOR& monitor) {
        const auto WINDOW = g_dragShrunk.lock();
        if (!WINDOW || !monitor || !WINDOW->m_realSize || !WINDOW->m_realPosition)
            return;
        const float S = std::max(monitor->m_scale, 0.01F);
        CBox box{(WINDOW->m_realPosition->value() - monitor->m_position) * S, WINDOW->m_realSize->value() * S};
        if (box.width < 2 || box.height < 2 || box.intersection(CBox{{}, monitor->m_pixelSize}).empty())
            return;
        box.round();
        OvInternal::renderOverviewWorkspaceShadow(monitor, box, 1.F, false, 1.F);
        Dock::drawFlightSurface(monitor, WINDOW, box, 1.F, Time::steadyNow(), false);
        if (const int BORDER = std::max(0, Config::getValue<int>("general:border_size")); BORDER > 0) {
            CBorderPassElement::SBorderData border;
            border.box        = box;
            border.grad1      = WINDOW->m_realBorderColor;
            border.borderSize = BORDER;
            border.round      = sc<int>(sc<float>(Config::themeRounding()) * S);
            g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(border));
        }
    }

    SDispatchResult onDragScale(std::string arg) {
        try {
            g_dragK = std::clamp(std::stod(arg), 0.0, 1.0);
        } catch (...) { return {.success = false, .error = "expected a number"}; }
        return {};
    }

    void shutdown() {
        g_move.reset();
        g_dragRender.reset();
        g_button.reset();
        if (g_timer)
            wl_event_source_remove(g_timer);
        g_timer       = nullptr;
        if (g_islandTimer)
            wl_event_source_remove(g_islandTimer);
        g_islandTimer   = nullptr;
        if (g_dragTimer)
            wl_event_source_remove(g_dragTimer);
        g_dragTimer   = nullptr;
        if (g_sidesTimer)
            wl_event_source_remove(g_sidesTimer);
        g_sidesTimer   = nullptr;
        g_sidesPending = false;
        g_sidesLatched = false;
        g_islandDrag  = false;
        dragRestore();
        g_islandPending = false;
        g_islandLatched = false;
        g_pending     = false;
        g_latched     = false;
        g_buttonsDown = 0;
    }
}
