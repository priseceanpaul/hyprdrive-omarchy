#include "Keyboard.hpp"
#include "KeyboardModel.hpp"
#include "Config.hpp"
#include "Crystal.hpp"
#include "Dock.hpp"
#include "DockStacks.hpp"
#include "FramePump.hpp"
#include "FunctionHooks.hpp"
#include "IOverview.hpp"
#include "MotionClock.hpp"
#include "MotionSites.hpp"
#include "OverviewRender.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <linux/input-event-codes.h>
#include <unordered_map>
#include <vector>

#include <aquamarine/input/Input.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/devices/ITouch.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <xkbcommon/xkbcommon.h>

namespace Hyprdrive::Keyboard {
    namespace KL = KeyboardLayout;
    namespace KM = KeyboardModel;

    namespace {
        // Geometry in logical px. The margin is the dock's EDGE_MARGIN (DockParts.hpp).
        constexpr float MARGIN_L = 4.F;
        constexpr float PAD_L    = 6.F;
        constexpr float GAP_L    = 4.F;

        // The reveal runs on the dock's curve; a press fades out over PRESS_MS.
        constexpr float REVEAL_MS = 260.F;
        constexpr float TAIL_MS   = 40.F; // one more frame, so the last one is drawn clean
        constexpr float PRESS_MS  = 220.F;
        constexpr int   REPEAT_DELAY_MS = 400;
        constexpr int   REPEAT_MS       = 60;

        struct SBoard {
            bool            shown     = false;
            bool            automatic = false; // raised by the search
            float           from      = 0.F;   // the reveal where the last flip found it
            Time::steady_tp changedAt = Time::steadyNow();
            KM::SState      state;

            // The last key pressed: lit while a finger is on it, fading after.
            int             pressRow = -1, pressCol = -1;
            bool            pressSymbols = false;
            bool            held         = false;
            Time::steady_tp pressedAt    = Time::steadyNow();
        };

        std::unordered_map<MONITORID, SBoard> g_boards;

        // Space reserved at the bottom of a monitor while the keyboard is up on the
        // plain desktop, and the fullscreen windows squeezed to make room for it.
        // NixOs-dots docs/manual.md#the-keyboard-in-hyprdrives-search
        std::unordered_map<MONITORID, bool> g_reserved;
        std::vector<PHLWINDOWREF>           g_squeezed;
        std::unordered_map<MONITORID, bool> g_syncing; // a reserve change already queued
        CFunctionHook*                      g_arrangeHook = nullptr;

        struct STouch {
            int32_t   id = 0;
            MONITORID mon = 0;
            int       row = -1, col = -1;
            bool      symbols = false;
            bool      moved   = false;
            bool      fired   = false; // the repeat took over: no press on up
            Vector2D  start;           // layout px
        };
        std::vector<STouch> g_touches;

        struct SMouse {
            bool      active = false;
            MONITORID mon    = 0;
            int       row = -1, col = -1;
            bool      symbols = false;
            bool      fired   = false;
        } g_mouse;
        std::vector<uint32_t> g_swallowed; // buttons whose press was ours, so their release is

        // Who the repeat timer is pressing for.
        struct SRepeat {
            bool      armed = false;
            MONITORID mon   = 0;
            int       row = -1, col = -1;
            bool      symbols = false;
            int32_t   touch   = -1; // -1: the pointer
        } g_repeat;

        CHyprSignalListener g_down, g_motion, g_up, g_cancel, g_button;
        wl_event_source*    g_timer = nullptr;

        SBoard* board(MONITORID id) {
            const auto IT = g_boards.find(id);
            return IT == g_boards.end() ? nullptr : &IT->second;
        }

        float target(const SBoard& b) {
            return b.shown ? 1.F : 0.F;
        }

        float progress(const SBoard& b) {
            const float T = std::clamp(Motion::Clock::animMs(b.changedAt) / REVEAL_MS, 0.F, 1.F);
            return b.from + (target(b) - b.from) * Motion::curve(Motion::ESite::DOCK_REVEAL, T);
        }

        float heightL(PHLMONITOR monitor) {
            return KL::boardHeightL(sc<float>(monitor->m_size.y), MARGIN_L);
        }

        float widthL(PHLMONITOR monitor) {
            return KL::boardWidthL(sc<float>(monitor->m_size.x), MARGIN_L);
        }

        // The plate at rest, centred, monitor-local RENDER px; `slid` is where it is now.
        CBox restingPlate(PHLMONITOR monitor) {
            const float S = monitor->m_scale;
            return CBox{std::round((sc<float>(monitor->m_size.x) - widthL(monitor)) / 2.F * S), std::round((sc<float>(monitor->m_size.y) - MARGIN_L - heightL(monitor)) * S),
                        std::round(widthL(monitor) * S), std::round(heightL(monitor) * S)};
        }

        CBox slidPlate(PHLMONITOR monitor, float reveal) {
            CBox plate = restingPlate(monitor);
            plate.y += std::round((1.F - reveal) * (heightL(monitor) + MARGIN_L) * monitor->m_scale);
            return plate;
        }

        KL::SGrid gridFor(PHLMONITOR monitor, const SBoard& b, const CBox& plate) {
            const float S = monitor->m_scale;
            return KL::solve(KL::rows(b.state.symbols), sc<float>(plate.w), sc<float>(plate.h), GAP_L * S, PAD_L * S);
        }

        bool inMonitor(PHLMONITOR monitor, const Vector2D& layoutPoint) {
            return monitor && monitor->logicalBox().containsPoint(layoutPoint);
        }

        Vector2D toLocal(PHLMONITOR monitor, const Vector2D& layoutPoint) {
            return (layoutPoint - monitor->m_position) * monitor->m_scale;
        }

        std::optional<std::pair<int, int>> keyAtLocal(PHLMONITOR monitor, const Vector2D& local) {
            const auto* B = board(monitor->m_id);
            if (!B || !B->shown)
                return std::nullopt;
            const CBox PLATE = slidPlate(monitor, progress(*B));
            return KL::keyAt(gridFor(monitor, *B, PLATE), sc<float>(local.x - PLATE.x), sc<float>(local.y - PLATE.y));
        }

        void poke(PHLMONITOR monitor) {
            if (monitor)
                FramePump::request(monitor, CBox{0.0, std::floor((sc<double>(monitor->m_size.y) - heightL(monitor) - MARGIN_L - 2.0) * monitor->m_scale),
                                                  sc<double>(monitor->m_size.x) * monitor->m_scale, (heightL(monitor) + MARGIN_L + 2.0) * monitor->m_scale + 4.0});
        }

        PHLMONITOR monitorById(MONITORID id) {
            for (const auto& M : State::monitorState()->monitors()) {
                if (M->m_id == id)
                    return M;
            }
            return nullptr;
        }

        // ---- reserved space ----------------------------------------------------

        // Like a layer's exclusive zone: Hyprland's arrangeLayersForMonitor resets
        // and re-adds the RESERVED_DYNAMIC_TYPE_LS slot, and this adds the keyboard
        // to it afterwards, so tiled and maximized windows reflow above the plate.
        void hkArrange(void* self, const MONITORID& id) {
            using orig = void (*)(void*, const MONITORID&);
            rc<orig>(g_arrangeHook->m_original)(self, id);
            const auto IT = g_reserved.find(id);
            if (IT == g_reserved.end() || !IT->second)
                return;
            if (const auto MON = monitorById(id)) {
                MON->m_reservedArea.addType(Desktop::RESERVED_DYNAMIC_TYPE_LS, Vector2D{0.0, 0.0}, Vector2D{0.0, sc<double>(heightL(MON) + MARGIN_L)});
                g_layoutManager->invalidateMonitorGeometries(MON);
            }
        }

        std::string squeezedFile() {
            const char* DIR = std::getenv("XDG_RUNTIME_DIR");
            return DIR && *DIR ? std::string(DIR) + "/omarchy-keyboard.squeezed" : std::string{};
        }

        std::string addressOf(const PHLWINDOW& window) {
            return std::format("0x{:x}", rc<uintptr_t>(window.get()));
        }

        // Fullscreen ignores reserved space: while the keyboard is up, the
        // monitor's fullscreen window is maximized, and listed for the pad daemon.
        void squeeze(PHLMONITOR monitor) {
            const auto WS = monitor->m_activeWorkspace;
            const auto W  = WS ? ::Fullscreen::controller()->getFullscreenWindow(WS) : nullptr;
            if (!W || !W->m_isMapped || !::Fullscreen::controller()->isFullscreen(W, ::Fullscreen::FSMODE_FULLSCREEN))
                return;
            ::Fullscreen::controller()->setFullscreenMode(W, ::Fullscreen::FSMODE_MAXIMIZED, std::nullopt);
            g_squeezed.push_back(W);
            if (const auto FILE = squeezedFile(); !FILE.empty())
                std::ofstream(FILE, std::ios::app) << addressOf(W) << "\n";
        }

        // Back to fullscreen what is still maximized; a window the user changed meanwhile stays.
        void unsqueeze() {
            for (const auto& REF : g_squeezed) {
                if (const auto W = REF.lock(); W && W->m_isMapped && ::Fullscreen::controller()->isFullscreen(W, ::Fullscreen::FSMODE_MAXIMIZED))
                    ::Fullscreen::controller()->setFullscreenMode(W, ::Fullscreen::FSMODE_FULLSCREEN, std::nullopt);
            }
            g_squeezed.clear();
            if (const auto FILE = squeezedFile(); !FILE.empty())
                std::remove(FILE.c_str());
        }

        bool wantReserve(PHLMONITOR monitor) {
            const auto* B = monitor ? board(monitor->m_id) : nullptr;
            return g_arrangeHook && B && B->shown && !scrollOverviewForMonitor(monitor);
        }

        // Applies the reserve the keyboard's state now calls for, outside any frame.
        void syncReserve(PHLMONITOR monitor) {
            if (!monitor || !g_pEventLoopManager || wantReserve(monitor) == g_reserved[monitor->m_id] || g_syncing[monitor->m_id])
                return;
            g_syncing[monitor->m_id] = true;
            const MONITORID ID       = monitor->m_id;
            g_pEventLoopManager->doLater([ID] {
                g_syncing[ID] = false;
                const auto MON = monitorById(ID);
                if (!MON || wantReserve(MON) == g_reserved[ID])
                    return;
                g_reserved[ID] = wantReserve(MON);
                if (g_reserved[ID])
                    squeeze(MON);
                else
                    unsqueeze();
                g_pHyprRenderer->arrangeLayersForMonitor(ID);
            });
        }

        // ---- typing ------------------------------------------------------------

        // A real keyboard first: its key signal is the one Hyprland's own handler
        // listens to, so a key sent through it is a keystroke in every respect.
        SP<IKeyboard> typingKeyboard() {
            if (!g_pInputManager)
                return nullptr;
            for (const auto& K : g_pInputManager->m_keyboards) {
                if (K && K->m_enabled && !K->isVirtual() && K->aq())
                    return K;
            }
            return g_pInputManager->m_keyboards.empty() ? nullptr : g_pInputManager->m_keyboards.front();
        }

        // Aquamarine's key signal -> CKeyboard (pressed list) -> CInputManager::onKeyboardKey
        // (the bus hooks, binds, the seat) -> the xkb state, and with it the modifiers.
        void inject(const SP<IKeyboard>& keyboard, uint32_t code, bool pressed) {
            const uint32_t NOW = sc<uint32_t>(Time::millis(Time::steadyNow()));
            if (const auto AQ = keyboard->aq(); AQ && !keyboard->isVirtual()) {
                AQ->events.key.emit(Aquamarine::IKeyboard::SKeyEvent{.timeMs = NOW, .key = code, .pressed = pressed});
                return;
            }
            g_pInputManager->onKeyboardKey(IKeyboard::SKeyEvent{.timeMs = NOW, .keycode = code, .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED},
                                           keyboard);
        }

        struct SCode {
            uint32_t evdev = 0;
            bool     shift = false; // the symbol sits on the key's second level
        };

        // The key that makes `sym` in this keymap: level 0 anywhere before level 1.
        std::optional<SCode> codeFor(const SP<IKeyboard>& keyboard, xkb_keysym_t sym) {
            const auto MAP = keyboard->m_xkbKeymap;
            if (!MAP || sym == XKB_KEY_NoSymbol)
                return std::nullopt;
            const xkb_layout_index_t LAYOUT = keyboard->m_activeLayout;
            for (xkb_level_index_t level = 0; level < 2; ++level) {
                for (xkb_keycode_t kc = std::max<xkb_keycode_t>(xkb_keymap_min_keycode(MAP), 9); kc <= xkb_keymap_max_keycode(MAP); ++kc) {
                    if (xkb_keymap_num_layouts_for_key(MAP, kc) <= LAYOUT || xkb_keymap_num_levels_for_key(MAP, kc, LAYOUT) <= level)
                        continue;
                    const xkb_keysym_t* syms = nullptr;
                    if (xkb_keymap_key_get_syms_by_level(MAP, kc, LAYOUT, level, &syms) == 1 && syms[0] == sym)
                        return SCode{kc - 8, level == 1};
                }
            }
            return std::nullopt;
        }

        void type(const KM::SStroke& stroke) {
            const auto KB = typingKeyboard();
            if (!KB)
                return;
            const xkb_keysym_t SYM = stroke.keysym ? xkb_keysym_from_name(stroke.keysym, XKB_KEYSYM_NO_FLAGS) : xkb_utf32_to_keysym(sc<uint32_t>(sc<unsigned char>(stroke.ch)));
            const auto         CODE = codeFor(KB, SYM);
            if (!CODE)
                return;

            std::vector<uint32_t> held;
            if (stroke.shift || CODE->shift)
                held.push_back(KEY_LEFTSHIFT);
            if (stroke.mods & KL::MOD_CTRL)
                held.push_back(KEY_LEFTCTRL);
            if (stroke.mods & KL::MOD_ALT)
                held.push_back(KEY_LEFTALT);
            if (stroke.mods & KL::MOD_SUPER)
                held.push_back(KEY_LEFTMETA);

            for (const auto CODE_HELD : held)
                inject(KB, CODE_HELD, true);
            inject(KB, CODE->evdev, true);
            inject(KB, CODE->evdev, false);
            for (auto it = held.rbegin(); it != held.rend(); ++it)
                inject(KB, *it, false);
        }

        // One press of the key at (row, col) on this layer: its effect on the
        // keyboard, its keystroke, and the light that fades after it.
        void pressKey(PHLMONITOR monitor, int row, int col, bool symbols) {
            auto* B = board(monitor->m_id);
            if (!B || !B->shown || B->state.symbols != symbols)
                return;
            const auto& ROWS = KL::rows(symbols);
            if (row < 0 || col < 0 || row >= sc<int>(ROWS.size()) || col >= sc<int>(ROWS[row].size()))
                return;

            B->pressRow     = row;
            B->pressCol     = col;
            B->pressSymbols = symbols;
            B->held         = false;
            B->pressedAt    = Time::steadyNow();

            const KM::SOutcome OUT = KM::press(B->state, ROWS[row][col]);
            if (OUT.effect == KM::EEffect::Close)
                hide(monitor);
            else if (OUT.effect == KM::EEffect::Stroke)
                type(OUT.stroke);
            poke(monitor);
        }

        void hold(PHLMONITOR monitor, int row, int col, bool symbols, bool on) {
            auto* B = board(monitor->m_id);
            if (!B || row < 0)
                return;
            B->pressRow     = row;
            B->pressCol     = col;
            B->pressSymbols = symbols;
            B->held         = on;
            B->pressedAt    = Time::steadyNow();
            poke(monitor);
        }

        void stopRepeat() {
            if (g_timer)
                wl_event_source_timer_update(g_timer, 0);
            g_repeat = {};
        }

        void armRepeat(PHLMONITOR monitor, int row, int col, bool symbols, int32_t touch) {
            const auto& ROWS = KL::rows(symbols);
            if (!g_timer || row < 0 || !KM::repeats(ROWS[row][col]))
                return;
            g_repeat = {true, monitor->m_id, row, col, symbols, touch};
            wl_event_source_timer_update(g_timer, REPEAT_DELAY_MS);
        }

        int onTimer(void*) {
            if (!g_repeat.armed)
                return 0;
            const auto MON = monitorById(g_repeat.mon);
            if (!MON) {
                stopRepeat();
                return 0;
            }
            for (auto& T : g_touches) {
                if (T.id == g_repeat.touch)
                    T.fired = true;
            }
            if (g_repeat.touch < 0)
                g_mouse.fired = true;
            pressKey(MON, g_repeat.row, g_repeat.col, g_repeat.symbols);
            hold(MON, g_repeat.row, g_repeat.col, g_repeat.symbols, true);
            wl_event_source_timer_update(g_timer, REPEAT_MS);
            return 0;
        }

        bool locked() {
            return g_pSessionLockManager && g_pSessionLockManager->isSessionLocked();
        }

        STouch* touchById(int32_t id) {
            const auto IT = std::ranges::find_if(g_touches, [id](const STouch& T) { return T.id == id; });
            return IT == g_touches.end() ? nullptr : &*IT;
        }

        void letGo(const STouch& touch) {
            if (const auto MON = monitorById(touch.mon))
                hold(MON, touch.row, touch.col, touch.symbols, false);
            if (g_repeat.armed && g_repeat.touch == touch.id)
                stopRepeat();
        }

        std::vector<std::string> autoMonitors() {
            std::vector<std::string> out;
            std::string              item;
            const auto               LIST = Hyprdrive::Config::getValue<std::string>("plugin:hyprdrive:keyboard:auto_monitors") + ",";
            for (const char C : LIST) {
                if (C != ',') {
                    if (C != ' ' && C != '\t')
                        item.push_back(C);
                    continue;
                }
                if (!item.empty())
                    out.push_back(item);
                item.clear();
            }
            return out;
        }

        void onButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
            const bool PRESSED = event.state == WL_POINTER_BUTTON_STATE_PRESSED;

            // A press that was ours: its release is too, wherever the pointer went.
            if (!PRESSED) {
                if (const auto IT = std::ranges::find(g_swallowed, event.button); IT != g_swallowed.end()) {
                    g_swallowed.erase(IT);
                    info.cancelled = true;
                    if (event.button == BTN_LEFT && g_mouse.active) {
                        const auto MON = monitorById(g_mouse.mon);
                        const auto AT  = g_pInputManager->getMouseCoordsInternal();
                        if (MON && !g_mouse.fired && inMonitor(MON, AT)) {
                            if (const auto KEY = keyAtLocal(MON, toLocal(MON, AT)); KEY && KEY->first == g_mouse.row && KEY->second == g_mouse.col)
                                pressKey(MON, g_mouse.row, g_mouse.col, g_mouse.symbols);
                        }
                        if (MON)
                            hold(MON, g_mouse.row, g_mouse.col, g_mouse.symbols, false);
                        if (g_repeat.armed && g_repeat.touch < 0)
                            stopRepeat();
                        g_mouse = {};
                    }
                }
                return;
            }

            if (info.cancelled || locked())
                return;
            const auto AT = g_pInputManager->getMouseCoordsInternal();
            for (const auto& MON : State::monitorState()->monitors()) {
                if (!inMonitor(MON, AT) || !contains(MON, toLocal(MON, AT)))
                    continue;
                info.cancelled = true;
                g_swallowed.push_back(event.button);
                if (event.button == BTN_LEFT) {
                    const auto KEY = keyAtLocal(MON, toLocal(MON, AT));
                    const auto* B  = board(MON->m_id);
                    g_mouse = {true, MON->m_id, KEY ? KEY->first : -1, KEY ? KEY->second : -1, B && B->state.symbols, false};
                    if (KEY) {
                        hold(MON, KEY->first, KEY->second, g_mouse.symbols, true);
                        armRepeat(MON, KEY->first, KEY->second, g_mouse.symbols, -1);
                    }
                }
                return;
            }
        }
    }

    void init() {
        // Not fatal when missing: then the keyboard covers windows, as it did.
        g_arrangeHook = FunctionHooks::firstMatch("Keyboard", "arrangeLayersForMonitor", rc<void*>(hkArrange), "arrangeLayersForMonitor(");
        if (g_arrangeHook && !g_arrangeHook->hook())
            g_arrangeHook = nullptr;

        if (!g_timer)
            g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, nullptr);

        g_button = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { onButton(event, info); });

        g_down = Event::bus()->m_events.input.touch.down.listen([](ITouch::SDownEvent e, Event::SCallbackInfo& info) {
            if (info.cancelled || locked())
                return;
            auto mon = State::monitorState()->query().name(!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "").run();
            if (!mon)
                mon = Desktop::focusState()->monitor();
            if (!mon)
                return;
            const Vector2D POINT = mon->m_position + e.pos * mon->m_size;
            const Vector2D LOCAL = toLocal(mon, POINT);
            if (!contains(mon, LOCAL))
                return;

            info.cancelled = true;
            const auto KEY = keyAtLocal(mon, LOCAL);
            const auto* B  = board(mon->m_id);
            STouch      touch{.id = e.touchID, .mon = mon->m_id, .row = KEY ? KEY->first : -1, .col = KEY ? KEY->second : -1, .symbols = B && B->state.symbols, .start = POINT};
            g_touches.push_back(touch);
            if (KEY) {
                hold(mon, touch.row, touch.col, touch.symbols, true);
                armRepeat(mon, touch.row, touch.col, touch.symbols, touch.id);
            }
        });

        g_motion = Event::bus()->m_events.input.touch.motion.listen([](ITouch::SMotionEvent e, Event::SCallbackInfo& info) {
            auto* T = touchById(e.touchID);
            if (!T)
                return;
            info.cancelled = true;
            const auto mon = monitorById(T->mon);
            if (!mon || T->moved)
                return;
            if ((mon->m_position + e.pos * mon->m_size).distance(T->start) > Hyprdrive::Config::getValue<int>("plugin:hyprdrive:touch:slop")) {
                T->moved = true;
                letGo(*T);
            }
        });

        g_up = Event::bus()->m_events.input.touch.up.listen([](ITouch::SUpEvent e, Event::SCallbackInfo& info) {
            auto* T = touchById(e.touchID);
            if (!T)
                return;
            info.cancelled = true;
            const STouch TOUCH = *T;
            std::erase_if(g_touches, [&](const STouch& X) { return X.id == TOUCH.id; });
            if (const auto MON = monitorById(TOUCH.mon); MON && !TOUCH.moved && !TOUCH.fired)
                pressKey(MON, TOUCH.row, TOUCH.col, TOUCH.symbols);
            letGo(TOUCH);
        });

        g_cancel = Event::bus()->m_events.input.touch.cancel.listen([](ITouch::SCancelEvent, Event::SCallbackInfo& info) {
            if (g_touches.empty())
                return;
            info.cancelled = true;
            for (const auto& T : g_touches)
                letGo(T);
            g_touches.clear();
        });
    }

    void shutdown() {
        // Windows back to fullscreen and the space given back, before the hook goes.
        unsqueeze();
        for (auto& [ID, ON] : g_reserved) {
            ON = false;
            g_pHyprRenderer->arrangeLayersForMonitor(ID);
        }
        g_reserved.clear();
        g_syncing.clear();
        if (g_arrangeHook) {
            g_arrangeHook->unhook();
            g_arrangeHook = nullptr;
        }
        g_down.reset();
        g_motion.reset();
        g_up.reset();
        g_cancel.reset();
        g_button.reset();
        if (g_timer)
            wl_event_source_remove(g_timer);
        g_timer = nullptr;
        g_repeat = {};
        g_touches.clear();
        g_swallowed.clear();
        g_mouse = {};
    }

    void show(PHLMONITOR monitor, bool automatic) {
        if (!monitor)
            return;
        auto& B = g_boards[monitor->m_id];
        if (B.shown) {
            B.automatic = B.automatic && automatic; // the user's own raise keeps it
            return;
        }
        B.from      = progress(B);
        B.shown     = true;
        B.automatic = automatic;
        B.changedAt = Time::steadyNow();
        poke(monitor);
        syncReserve(monitor);
    }

    void hide(PHLMONITOR monitor) {
        auto* B = monitor ? board(monitor->m_id) : nullptr;
        if (!B)
            return;
        B->state.mods  = 0;
        B->state.shift = false;
        B->automatic   = false;
        B->held        = false;
        if (!B->shown)
            return;
        B->from      = progress(*B);
        B->shown     = false;
        B->changedAt = Time::steadyNow();
        if (g_repeat.armed && g_repeat.mon == monitor->m_id)
            stopRepeat();
        poke(monitor);
        syncReserve(monitor);
    }

    void toggle(PHLMONITOR monitor) {
        if (shown(monitor))
            hide(monitor);
        else
            show(monitor);
    }

    bool shown(PHLMONITOR monitor) {
        const auto* B = monitor ? board(monitor->m_id) : nullptr;
        return B && B->shown;
    }

    float reveal(PHLMONITOR monitor) {
        const auto* B = monitor ? board(monitor->m_id) : nullptr;
        return B ? progress(*B) : 0.F;
    }

    float visibleHeightL(PHLMONITOR monitor) {
        const auto* B = monitor ? board(monitor->m_id) : nullptr;
        return B ? (heightL(monitor) + MARGIN_L) * progress(*B) : 0.F;
    }

    bool animating(PHLMONITOR monitor) {
        const auto* B = monitor ? board(monitor->m_id) : nullptr;
        if (!B)
            return false;
        if (Motion::Clock::animMs(B->changedAt) < REVEAL_MS + TAIL_MS)
            return true;
        return B->pressRow >= 0 && (B->held || Motion::Clock::animMs(B->pressedAt) < PRESS_MS + TAIL_MS);
    }

    bool contains(PHLMONITOR monitor, const Vector2D& localRenderPx) {
        const auto* B = monitor ? board(monitor->m_id) : nullptr;
        return B && B->shown && slidPlate(monitor, progress(*B)).containsPoint(localRenderPx);
    }

    void render(PHLMONITOR monitor) {
        // An overview opening or going over a shown keyboard changes what is reserved.
        syncReserve(monitor);
        auto* B = monitor ? board(monitor->m_id) : nullptr;
        if (!B)
            return;
        const float REVEAL = progress(*B);
        if (REVEAL <= 0.002F) {
            if (!animating(monitor) && !B->shown)
                g_boards.erase(monitor->m_id); // nothing of it left: no state, no cost
            return;
        }

        const float S        = monitor->m_scale;
        const CBox  PLATE    = slidPlate(monitor, REVEAL);
        const int   ROUNDING = std::clamp(sc<int>(std::round(Hyprdrive::Config::dockCornerRadius() * S)), 2, sc<int>(PLATE.h / 4.0));
        if (!Stacks::drawableBox(PLATE))
            return;

        Dock::frostedPlate(monitor, PLATE, ROUNDING, REVEAL);

        if (!Crystal::enabled()) {
            CBorderPassElement::SBorderData border;
            border.box           = PLATE.copy().round();
            border.grad1.reset(Hyprdrive::Config::restingBorderColor().modifyA(1.F));
            border.a             = REVEAL;
            border.borderSize    = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
            border.round         = ROUNDING;
            border.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
        }

        const auto  GRID   = gridFor(monitor, *B, PLATE);
        const auto& ROWS   = KL::rows(B->state.symbols);
        const auto  ACCENT = Hyprdrive::Config::highlightColor();
        const auto  INK    = Hyprdrive::Config::themeForeground();
        const auto  ONLIT  = Hyprdrive::Config::highlightForeground();
        const float FADE   = B->pressRow < 0 || B->pressSymbols != B->state.symbols ? 0.F
                           : B->held        ? 1.F
                                            : std::clamp(1.F - Motion::Clock::animMs(B->pressedAt) / PRESS_MS, 0.F, 1.F);
        const int   FONT   = std::max(sc<int>(std::round(KL::MIN_LABEL_L * S)), sc<int>(std::round(GRID.unit * 0.42F)));
        const int   KEYR   = std::max(3, std::min(ROUNDING / 2, sc<int>(GRID.unit * 0.25F)));

        for (size_t r = 0; r < ROWS.size(); ++r) {
            for (size_t c = 0; c < ROWS[r].size(); ++c) {
                const auto& KEY  = ROWS[r][c];
                const auto& RECT = GRID.keys[r][c];
                const CBox  BOX  = CBox{PLATE.x + RECT.x, PLATE.y + RECT.y, RECT.w, RECT.h}.round();

                const bool  LATCHED = KM::latched(KEY, B->state);
                const float LIT     = LATCHED ? 1.F : (sc<int>(r) == B->pressRow && sc<int>(c) == B->pressCol ? FADE : 0.F);

                CRectPassElement::SRectData fill;
                fill.box           = BOX;
                fill.color         = LIT > 0.F ? ACCENT.modifyA(std::max(0.08F, 0.85F * LIT) * REVEAL) : INK.modifyA(0.08F * REVEAL);
                fill.round         = KEYR;
                fill.roundingPower = 2.F;
                if (Stacks::drawableBox(BOX))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(fill));

                const auto LABEL = KM::displayLabel(KEY, B->state);
                if (LABEL.empty())
                    continue;
                const auto TEX = OverviewRender::text(LABEL, LIT >= 0.5F ? ONLIT.modifyA(1.F) : INK.modifyA(1.F), FONT, false, "", std::max(1, sc<int>(BOX.w) - 6), 500);
                if (!TEX || !TEX->ok())
                    continue;
                CTexPassElement::SRenderData label;
                label.tex = TEX;
                label.box = CBox{std::round(BOX.x + (BOX.w - TEX->m_size.x) / 2.0), std::round(BOX.y + (BOX.h - TEX->m_size.y) / 2.0), sc<double>(TEX->m_size.x), sc<double>(TEX->m_size.y)};
                label.a   = REVEAL;
                if (Stacks::drawableBox(label.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(label));
            }
        }

        if (animating(monitor))
            poke(monitor);
    }

    void onSearch(bool active, const std::string& monitorName) {
        if (!active) {
            // Search over, or the overview closed under it: what it raised goes.
            for (const auto& M : State::monitorState()->monitors()) {
                if (const auto* B = board(M->m_id); B && B->automatic)
                    hide(M);
            }
            return;
        }
        const auto LIST = autoMonitors();
        if (std::ranges::find(LIST, monitorName) == LIST.end())
            return;
        if (const auto MON = State::monitorState()->query().name(monitorName).run())
            show(MON, true);
    }

    void forgetMonitor(MONITORID id) {
        g_boards.erase(id);
        g_reserved.erase(id);
        g_syncing.erase(id);
        std::erase_if(g_touches, [id](const STouch& T) { return T.mon == id; });
        if (g_repeat.armed && g_repeat.mon == id)
            stopRepeat();
        if (g_mouse.mon == id)
            g_mouse = {};
    }

    SDispatchResult dispatch(std::string arg) {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR)
            return {.success = false, .error = "no focused monitor"};
        if (arg == "on" || arg == "open")
            show(MONITOR);
        else if (arg == "off" || arg == "close")
            hide(MONITOR);
        else
            toggle(MONITOR);
        return {};
    }
}
