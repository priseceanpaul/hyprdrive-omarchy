#include "KeyboardInset.hpp"
#include "Keyboard.hpp"
#include "Motion.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>

#include <chrono>
#include <unordered_map>

namespace Hyprdrive::KeyboardInset {

    namespace {
        constexpr float EASE_MS = 220.F;

        struct SEase {
            float                                 from = 0.F, to = 0.F;
            std::chrono::steady_clock::time_point start;
        };

        std::unordered_map<uint64_t, SEase>& eases() {
            static std::unordered_map<uint64_t, SEase> e;
            return e;
        }

        float progress(const SEase& e, std::chrono::steady_clock::time_point now) {
            return std::chrono::duration<float, std::milli>(now - e.start).count() / EASE_MS;
        }

        float value(const SEase& e, std::chrono::steady_clock::time_point now) {
            const float T = progress(e, now);
            return T >= 1.F ? e.to : e.from + (e.to - e.from) * Motion::houseFixed(true, T);
        }
    }

    namespace {
        // The shell keyboard's layer, if one is mapped: what rawL() was before
        // Hyprdrive drew its own.
        float layerL(PHLMONITOR monitor) {
            if (!monitor)
                return 0.F;

            float height = 0.F;
            for (const auto& level : monitor->m_layerSurfaceLayers) {
                for (const auto& ref : level) {
                    const auto LS = ref.lock();
                    if (!Desktop::View::validMapped(LS) || LS->m_namespace != "omarchy-keyboard")
                        continue;
                    height = std::max(height, sc<float>(LS->m_geometry.h));
                }
            }
            return std::min(height, sc<float>(monitor->m_size.y));
        }
    }

    float rawL(PHLMONITOR monitor) {
        return std::max(layerL(monitor), Hyprdrive::Keyboard::visibleHeightL(monitor));
    }

    float easedL(PHLMONITOR monitor) {
        if (!monitor)
            return 0.F;

        // Hyprdrive's own keyboard is eased already (Keyboard.cpp): it joins
        // after the layer's ease, never through it.
        const float OWN = Hyprdrive::Keyboard::visibleHeightL(monitor);
        const float RAW = layerL(monitor);
        const auto  NOW = std::chrono::steady_clock::now();
        const auto  IT  = eases().find(sc<uint64_t>(monitor->m_id));
        if (IT == eases().end()) {
            if (RAW == 0.F)
                return OWN; // the common case allocates nothing
            eases().emplace(sc<uint64_t>(monitor->m_id), SEase{.from = 0.F, .to = RAW, .start = NOW});
            return OWN;
        }

        auto& e = IT->second;
        if (RAW != e.to) {
            e.from  = value(e, NOW);
            e.to    = RAW;
            e.start = NOW;
        }
        return std::max(value(e, NOW), OWN);
    }

    bool easing(PHLMONITOR monitor) {
        if (!monitor)
            return false;
        if (Hyprdrive::Keyboard::animating(monitor))
            return true;
        const auto IT = eases().find(sc<uint64_t>(monitor->m_id));
        return IT != eases().end() && progress(IT->second, std::chrono::steady_clock::now()) < 1.F;
    }

}
