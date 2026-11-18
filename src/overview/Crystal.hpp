#pragma once

#include "globals.hpp"

#include <hyprland/src/render/pass/PassElement.hpp>

// Cut crystal behind menus and panels, in place of their blur: the frame behind a
// plate split into shards, each shifted its own way (docs/manual.md#crystal-windows)
namespace Hyprdrive::Crystal {
    // plugin:hyprdrive:crystal, which True Glass turns on while it is on
    bool enabled();
    // How a pass looks: the blur behind (passes, 0 none, -1 from Glass's frost), then milk
    // (even), vary (shard by shard), edge light and shift, each 1 for a plate
    struct SLook {
        int   blurPasses = -1;
        float milk = 1.F, vary = 1.F, edge = 1.F, shift = 1.F;
        bool  rim = true;   // True Glass's edge: the bend and its colour split
        bool  bevel = true; // and its lit bevel line (a plate draws it after its tint: bevel())
        bool  tone = true;  // True Glass's light or dark tint (plugin:hyprdrive:crystal_tone)
    };
    // A plate: its bevel comes after the tint, which would otherwise cover it
    inline constexpr SLook PLATE{.bevel = false};
    // `box` in monitor-local render px, as the plate helpers take it
    void queue(const CBox& box, int rounding, float alpha, const SLook& look = {});
    // A plate's bevel line alone, queued after its tint
    void bevel(const CBox& box, int rounding, float alpha);
    // the same pass as an element, for callers that add their own (overview previews)
    UP<IPassElement> element(const CBox& box, int rounding, float alpha, const SLook& look = {});
    // The overview's backdrop, cut from the sharp wallpaper: lighter frost, one even milk
    inline constexpr SLook BACKDROP{.blurPasses = 2, .milk = 5.0F, .vary = 0.625F, .edge = 1.1F, .shift = 1.F, .rim = false, .tone = false};
    // its darkening while the crystal is on, in place of the overview's 80 % (the panel's setting)
    float backdropDim();
    // The plate's tint over crystal: nearly none, as True Glass's cards
    float tintAlpha(float a);
    void shutdown();
    inline constexpr const char* PASS_NAME = "CCrystalPassElement";
}
