#pragma once

#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>

// A surface that asked for the keyboard keeps it over the overview: an
// interactive TOP/OVERLAY layer, or a focus-grabbed bar popup. docs/manual.md#the-menu-over-hyprdrive
inline bool layerOwnsKeyboard() {
    const auto FOCUS = g_pSeatManager ? g_pSeatManager->m_state.keyboardFocus.lock() : nullptr;
    if (!FOCUS)
        return false;
    if (g_pSeatManager->m_seatGrab && g_pSeatManager->m_seatGrab->m_keyboard && g_pSeatManager->m_seatGrab->accepts(FOCUS))
        return true;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(FOCUS);
    const auto VIEW      = HLSURFACE ? HLSURFACE->view() : nullptr;
    if (!VIEW)
        return false;

    auto layer = Desktop::View::CLayerSurface::fromView(VIEW);
    if (!layer) {
        const auto POPUP = Desktop::View::CPopup::fromView(VIEW);
        const auto OWNER = POPUP ? POPUP->getT1Owner() : nullptr;
        if (OWNER && OWNER->view())
            layer = Desktop::View::CLayerSurface::fromView(OWNER->view());
    }

    return layer && layer->m_mapped && layer->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
        layer->m_interactivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}
