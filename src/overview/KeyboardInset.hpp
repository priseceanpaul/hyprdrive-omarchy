#pragma once

#include <hyprland/src/desktop/DesktopTypes.hpp>

// The on-screen keyboard's height on a monitor, so the overview and the dock
// can lay out above it: Hyprdrive's own (Keyboard.hpp), or a layer surface named
// "omarchy-keyboard"; neither means 0 and nothing here changes anything.
// NixOs-dots docs/manual.md#the-keyboard-in-hyprdrives-search
namespace Hyprdrive::KeyboardInset {

// The keyboard's logical height on `monitor`; 0 if none. Hyprdrive's is eased as
// it is drawn and never eased again here; the layer's is raw.
float rawL(PHLMONITOR monitor);

// The layer's height eased over 220 ms, Hyprdrive's as it stands, so the layout
// glides with either. Reading it advances the ease: once per layout pass.
float easedL(PHLMONITOR monitor);

// Whether the ease is still moving on `monitor`: the caller asks frames.
bool  easing(PHLMONITOR monitor);

}
