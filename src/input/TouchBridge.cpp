// Touches on the dock or its menu become clicks: tap = left, a still hold = right.
// NixOs-dots docs/manual.md#holding-in-hyprdrive
#include "TouchBridge.hpp"
#include "Dock.hpp"
#include "IOverview.hpp"
#include "Config.hpp"

#include <cmath>
#include <linux/input-event-codes.h>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/pointer/PointerController.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace Hyprdrive::TouchBridge {
    namespace {
        // Read live each time. NixOs-dots docs/manual.md#holding-in-hyprdrive
        int    holdMs() { return Hyprdrive::Config::getValue<int>("plugin:hyprdrive:touch:hold_ms"); }
        double slopPx() { return Hyprdrive::Config::getValue<int>("plugin:hyprdrive:touch:slop"); }

        CHyprSignalListener g_down, g_motion, g_up, g_cancel;
        wl_event_source*    g_timer = nullptr;

        struct {
            bool         owned = false;
            int32_t      id    = 0;
            bool         moved = false;
            bool         fired = false;
            PHLMONITORREF mon;
            Vector2D     start; // layout px
        } g_t;

        void button(uint32_t code, bool pressed) {
            IPointer::SButtonEvent b;
            b.timeMs = Time::millis(Time::steadyNow());
            b.button = code;
            b.state  = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
            g_pInputManager->onMouseButton(b, g_pInputManager->m_pointers.empty() ? nullptr : g_pInputManager->m_pointers.front());
        }

        void click(uint32_t code) {
            Pointer::pointerController()->warpTo(g_t.start, true);
            g_pInputManager->simulateMouseMovement();
            button(code, true);
            button(code, false);
        }

        void release() {
            if (g_timer)
                wl_event_source_timer_update(g_timer, 0);
            g_t = {};
        }

        int onTimer(void*) {
            if (g_t.owned && !g_t.moved && !g_t.fired) {
                g_t.fired = true;
                click(BTN_RIGHT);
            }
            return 0;
        }
    }

    void init() {
        if (!g_timer)
            g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, nullptr);

        g_down = Event::bus()->m_events.input.touch.down.listen([](ITouch::SDownEvent e, Event::SCallbackInfo& info) {
            if (info.cancelled || g_pSessionLockManager->isSessionLocked() || !Hyprdrive::Config::getValue<bool>("plugin:hyprdrive:touch:dock"))
                return;
            if (g_t.owned) { // a second finger while one is held on the dock is swallowed too
                info.cancelled = true;
                return;
            }
            auto mon = State::monitorState()->query().name(!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "").run();
            if (!mon)
                mon = Desktop::focusState()->monitor();
            if (!mon)
                return;
            const Vector2D POINT = mon->m_position + e.pos * mon->m_size;
            const Vector2D LOCAL = (POINT - mon->m_position) * mon->m_scale;
            if (!Hyprdrive::Dock::occupiesPointer(mon, LOCAL) && !Hyprdrive::Dock::menuContains(mon, LOCAL))
                return;

            info.cancelled = true;
            g_t            = {true, e.touchID, false, false, mon, POINT};
            Pointer::pointerController()->warpTo(POINT, true);
            g_pInputManager->simulateMouseMovement();
            if (g_timer)
                wl_event_source_timer_update(g_timer, holdMs());
        });

        g_motion = Event::bus()->m_events.input.touch.motion.listen([](ITouch::SMotionEvent e, Event::SCallbackInfo& info) {
            if (!g_t.owned || e.touchID != g_t.id)
                return;
            info.cancelled = true;
            const auto mon = g_t.mon.lock();
            if (!mon || g_t.moved)
                return;
            if ((mon->m_position + e.pos * mon->m_size).distance(g_t.start) > slopPx()) {
                g_t.moved = true;
                if (g_timer)
                    wl_event_source_timer_update(g_timer, 0);
            }
        });

        g_up = Event::bus()->m_events.input.touch.up.listen([](ITouch::SUpEvent e, Event::SCallbackInfo& info) {
            if (!g_t.owned || e.touchID != g_t.id)
                return;
            info.cancelled = true;
            const bool TAP = !g_t.moved && !g_t.fired;
            if (TAP)
                click(BTN_LEFT);
            release();
        });

        g_cancel = Event::bus()->m_events.input.touch.cancel.listen([](ITouch::SCancelEvent, Event::SCallbackInfo& info) {
            if (!g_t.owned)
                return;
            info.cancelled = true;
            release();
        });
    }

    void shutdown() {
        g_down.reset();
        g_motion.reset();
        g_up.reset();
        g_cancel.reset();
        if (g_timer)
            wl_event_source_remove(g_timer);
        g_timer = nullptr;
        g_t     = {};
    }
}
