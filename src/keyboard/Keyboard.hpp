#pragma once

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/math/Math.hpp>

#include "globals.hpp"

#include <string>

// The on-screen keyboard, drawn by Hyprdrive as the dock's twin: a frosted plate
// that slides up from the bottom edge, for a phone streaming the desktop and a
// TV. On the plain desktop it reserves its height at the bottom of the monitor, as
// a layer's exclusive zone does. It types through Hyprland's keyboard path, so the overview's search and the
// focused client both get its keys. NixOs-dots docs/manual.md#the-keyboard-in-hyprdrives-search
namespace Hyprdrive::Keyboard {

    // The listeners (touch and pointer) and the repeat timer. Before TouchBridge's
    // and the overview's, so a touch on the keyboard is the keyboard's.
    void init();
    void shutdown();

    // `automatic`: raised by the search on a listed monitor, so its end hides it.
    void show(PHLMONITOR monitor, bool automatic = false);
    void hide(PHLMONITOR monitor);
    void toggle(PHLMONITOR monitor);
    bool shown(PHLMONITOR monitor);

    // How far up the keyboard (and its margin) covers the monitor, in logical px,
    // eased with its reveal: 0 when hidden. KeyboardInset reads it as it is.
    float visibleHeightL(PHLMONITOR monitor);

    // The reveal, 0 hidden to 1 up, on the dock's curve: the dock hides by it.
    float reveal(PHLMONITOR monitor);

    // Sliding, or still fading a press: the caller asks frames.
    bool animating(PHLMONITOR monitor);

    // Drawn above the dock, on the desktop (main.cpp) and over an overview (Overview.cpp).
    void render(PHLMONITOR monitor);

    // Whether `localRenderPx` (monitor-local, RENDER px) is on the shown keyboard.
    bool contains(PHLMONITOR monitor, const Vector2D& localRenderPx);

    // announceOverviewSearch's edge: search up on `monitorName`, or over.
    void onSearch(bool active, const std::string& monitorName);

    void forgetMonitor(MONITORID id);

    // hyprdrive:keyboard toggle|on|open|off|close, on the focused monitor.
    SDispatchResult dispatch(std::string arg);
}
