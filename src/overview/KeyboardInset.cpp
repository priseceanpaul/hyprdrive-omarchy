#include "KeyboardInset.hpp"
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

    float rawL(PHLMONITOR monitor) {
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

    float easedL(PHLMONITOR monitor) {
        if (!monitor)
            return 0.F;

        const float RAW = rawL(monitor);
        const auto  NOW = std::chrono::steady_clock::now();
        const auto  IT  = eases().find(sc<uint64_t>(monitor->m_id));
        if (IT == eases().end()) {
            if (RAW == 0.F)
                return 0.F; // the common case allocates nothing
            eases().emplace(sc<uint64_t>(monitor->m_id), SEase{.from = 0.F, .to = RAW, .start = NOW});
            return 0.F;
        }

        auto& e = IT->second;
        if (RAW != e.to) {
            e.from  = value(e, NOW);
            e.to    = RAW;
            e.start = NOW;
        }
        return value(e, NOW);
    }

    bool easing(PHLMONITOR monitor) {
        if (!monitor)
            return false;
        const auto IT = eases().find(sc<uint64_t>(monitor->m_id));
        return IT != eases().end() && progress(IT->second, std::chrono::steady_clock::now()) < 1.F;
    }

}
