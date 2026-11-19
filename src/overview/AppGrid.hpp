#pragma once

#include "globals.hpp"

#include <cstdint>

// Where the overview's pointer goes while the grid owns it: on no tile or strip.
inline const Vector2D OVERVIEW_POINTER_PARKED{-1e6, -1e6};

// G5's full app grid, inside the overview. Every tile is a dock item: same dots,
// click and menu. NixOs-dots docs/manual.md#the-hyprdrive-app-grid
namespace Hyprdrive::AppGrid {

// From the desktop these open the overview too, and closing that grid closes it.
void toggle(PHLMONITOR monitor);
void open(PHLMONITOR monitor);
void close(PHLMONITOR monitor);
bool isOpen(PHLMONITOR monitor);
// 0..1, on the grid's own fade: how far an open grid on a fill monitor has
// replaced the overview's cards and strip. 0 anywhere else.
float coverFade(PHLMONITOR monitor);
// The grid is up and the pointer is on its plate, not the dock or the search field.
bool ownsPointer(PHLMONITOR monitor, const Vector2D& point);
// The results band while the grid is shown: the room alone; the tiles leave as
// the band passes the room beside them. 0 hidden. Logical px.
float bandL(PHLMONITOR monitor, float withTilesL, float aloneL, float bandNowL);
// The grid is open on this window's monitor, so its tile is out of the overview.
bool hidesWindow(const PHLWINDOW& window);

// The drawer lives in the search's results container, which grows out of the pill:
// containerUp says the grid owns this monitor's overview (open, or folding back),
// widthL what that container grows to, in logical px. NixOs-dots docs/manual.md#one-app-drawer
bool  containerUp(PHLMONITOR monitor);
// The band's resting room, logical px; and on a fill monitor the logical y the container
// reaches down to (above the dock or keyboard), -1 elsewhere. NixOs-dots docs/manual.md#one-app-drawer
float roomL();
float bottomL(PHLMONITOR monitor);
float widthL(PHLMONITOR monitor);

// After the search, before the bar and dock, so the dock's menu lands on top.
// container: the search's results container this frame, render px; rest: the box it grows to.
// The cells are laid out against rest and clipped to container.
void render(PHLMONITOR monitor, float fade, const CBox& searchPill, const CBox& container, const CBox& rest);

// The overview's input hooks; true = claimed. Monitor-local render px.
bool handleButton(PHLMONITOR monitor, const Vector2D& point, uint32_t button, bool pressed, uint32_t mainButton, uint32_t menuButton);
bool handleAxis(PHLMONITOR monitor, double delta, bool wheel);
// A finger dragging on the plate scrolls it: touchScrollable asks whether the point is one
// a vertical drag may scroll, Scroll follows the finger by dy render px, End lets
// it glide on and settle on a row.
bool touchScrollable(PHLMONITOR monitor, const Vector2D& point);
void touchScroll(PHLMONITOR monitor, double dy);
void touchScrollEnd(PHLMONITOR monitor);
bool handleKey(PHLMONITOR monitor, uint32_t keysym);

} // namespace Hyprdrive::AppGrid
