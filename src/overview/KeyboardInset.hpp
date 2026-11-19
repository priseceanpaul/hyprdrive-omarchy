#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>

// The on-screen keyboard's height on a monitor, so the overview and the dock
// can lay out above it. A layer surface named "omarchy-keyboard" is that
// keyboard; no such surface means 0 and nothing here changes anything.
// NixOs-dots docs/manual.md#the-keyboard-in-hyprdrives-search
namespace Hyprdrive::KeyboardInset {

// The mapped keyboard's logical height on `monitor`, any layer; 0 if none.
float rawL(PHLMONITOR monitor);

// rawL() eased over 220 ms, so the layout glides with the keyboard. Reading it
// advances the ease, so call it once per layout pass.
float easedL(PHLMONITOR monitor);

// Whether the ease is still moving on `monitor`: the caller asks frames.
bool  easing(PHLMONITOR monitor);

}
