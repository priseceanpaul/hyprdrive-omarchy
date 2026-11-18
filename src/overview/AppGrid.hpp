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
// The grid is up and the pointer is on its plate, not the dock or the search field.
bool ownsPointer(PHLMONITOR monitor, const Vector2D& point);
// The results band while the grid is shown: the room alone; the tiles leave as
// the band passes the room beside them. 0 hidden. Logical px.
float bandL(PHLMONITOR monitor, float withTilesL, float aloneL, float bandNowL);
// The grid is open on this window's monitor, so its tile is out of the overview.
bool hidesWindow(const PHLWINDOW& window);

// After the search, before the bar and dock, so the dock's menu lands on top.
// band: the results card's box this frame, render px.
void render(PHLMONITOR monitor, float fade, const CBox& searchPill, const CBox& band);

// The overview's input hooks; true = claimed. Monitor-local render px.
bool handleButton(PHLMONITOR monitor, const Vector2D& point, uint32_t button, bool pressed, uint32_t mainButton, uint32_t menuButton);
bool handleAxis(PHLMONITOR monitor, double delta, bool wheel);
bool handleKey(PHLMONITOR monitor, uint32_t keysym);

} // namespace Hyprdrive::AppGrid
