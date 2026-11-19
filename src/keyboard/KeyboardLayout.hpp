#pragma once

// The on-screen keyboard's keys and where they sit: pure, no compositor.
// The two layers are the shell keyboard's (Layout.js) key for key; the box
// maths is Hyprdrive's own. NixOs-dots docs/manual.md#the-keyboard-in-hyprdrives-search

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Hyprdrive::KeyboardLayout {

    enum class EKind : uint8_t {
        Text,  // typed literally; uppercased while shift is latched
        Key,   // an XKB keysym by name; BackSpace repeats on hold
        Mod,   // a one-shot modifier for the next key
        Shift, // a one-shot shift
        Layer, // letters <-> symbols
        Close, // hides the keyboard
        Paste, // Ctrl+V
    };

    enum EMod : uint8_t { MOD_CTRL = 1, MOD_ALT = 2, MOD_SUPER = 4 };

    struct SKey {
        EKind       kind    = EKind::Text;
        const char* label   = "";
        const char* keysym  = ""; // Key only
        uint8_t     mod     = 0;  // Mod only
        float       width   = 1.F; // in key units
        bool        space   = false;
    };

    using Row  = std::vector<SKey>;
    using Rows = std::vector<Row>;

    inline SKey text(const char* label, float width = 1.F) { return {EKind::Text, label, "", 0, width, false}; }
    inline SKey named(const char* label, const char* keysym, float width = 1.F) { return {EKind::Key, label, keysym, 0, width, false}; }

    inline const Rows& rows(bool symbols) {
        static const Row TOP = {named("esc", "Escape"), text("1"), text("2"), text("3"), text("4"), text("5"), text("6"), text("7"), text("8"), text("9"), text("0"),
                                named("⌫", "BackSpace", 1.5F)};
        static const Row BOTTOM = {{EKind::Mod, "ctrl", "", MOD_CTRL, 1.25F, false},
                                   {EKind::Mod, "super", "", MOD_SUPER, 1.25F, false},
                                   {EKind::Mod, "alt", "", MOD_ALT, 1.25F, false},
                                   {EKind::Text, " ", "", 0, 4.5F, true},
                                   {EKind::Close, "✕", "", 0, 1.25F, false},
                                   named("←", "Left"),
                                   named("↓", "Down"),
                                   named("→", "Right")};
        static const SKey TAB   = named("tab", "Tab", 1.5F);
        static const SKey ENTER = named("⏎", "Return", 1.75F);
        static const SKey UP    = named("↑", "Up");
        static const SKey SHIFT = {EKind::Shift, "⇧", "", 0, 2.F, false};

        static const Rows LETTERS = {
            TOP,
            {TAB, text("q"), text("w"), text("e"), text("r"), text("t"), text("y"), text("u"), text("i"), text("o"), text("p"), text("-")},
            {{EKind::Layer, "?123", "", 0, 1.75F, false}, text("a"), text("s"), text("d"), text("f"), text("g"), text("h"), text("j"), text("k"), text("l"), ENTER},
            {SHIFT, text("z"), text("x"), text("c"), text("v"), text("b"), text("n"), text("m"), text(".", 1.5F), UP, {EKind::Paste, "paste", "", 0, 1.F, false}},
            BOTTOM,
        };
        static const Rows SYMBOLS = {
            TOP,
            {TAB, text("@"), text("#"), text("$"), text("%"), text("&"), text("*"), text("("), text(")"), text("-"), text("_"), text("^")},
            {{EKind::Layer, "abc", "", 0, 1.75F, false}, text("/"), text("\\"), text(":"), text(";"), text("'"), text("\""), text("!"), text("?"), text("="), ENTER},
            {SHIFT, text("+"), text(","), text("~"), text("`"), text("|"), text("<"), text(">"), text("[", 1.5F), UP, text("]")},
            BOTTOM,
        };
        return symbols ? SYMBOLS : LETTERS;
    }

    inline float rowUnits(const Row& row) {
        float units = 0.F;
        for (const auto& key : row)
            units += key.width;
        return units;
    }

    inline float maxUnits(const Rows& all) {
        float most = 1.F;
        for (const auto& row : all)
            most = std::max(most, rowUnits(row));
        return most;
    }

    struct SRect {
        float x = 0.F, y = 0.F, w = 0.F, h = 0.F;
        bool  contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    };

    struct SGrid {
        std::vector<std::vector<SRect>> keys; // box-local, same units as the inputs
        float                           unit = 0.F; // the smaller side of a key: what labels follow
    };

    // The board's height in logical px: `ratio` of the screen (keyboard:height_ratio,
    // 19% by default) between 150 and 260, never more than the screen less its margins.
    inline float boardHeightL(float monitorHeightL, float marginL, float ratio = 0.19F) {
        return std::clamp(std::clamp(monitorHeightL * ratio, 150.F, 260.F), 1.F, std::max(1.F, monitorHeightL - 2.F * marginL));
    }

    // Its width: the screen less a margin each side, and no wider than `maxWidth`
    // (keyboard:max_width, 820 by default).
    inline float boardWidthL(float monitorWidthL, float marginL, float maxWidth = 820.F) {
        return std::max(1.F, std::min(monitorWidthL - 2.F * marginL, maxWidth));
    }

    // The smallest a label is drawn, in logical px: keys shrink before this does.
    inline constexpr float MIN_LABEL_L = 13.F;

    // Rows fill the height exactly and keys fill the width, so the board's own
    // size (boardWidthL) is what keeps them from stretching. `pad` is the inset
    // all round, `gap` what lies between keys.
    inline SGrid solve(const Rows& all, float width, float height, float gap, float pad) {
        SGrid out;
        const int   N     = static_cast<int>(all.size());
        const float U     = maxUnits(all);
        const float UNITH = std::max(1.F, (height - 2.F * pad - gap * static_cast<float>(N - 1)) / static_cast<float>(std::max(1, N)));
        const float UNITW = std::max(1.F, (width - 2.F * pad - gap * (U - 1.F)) / U);
        const float BLOCK = UNITW * U + gap * (U - 1.F);
        const float X0    = pad + std::max(0.F, (width - 2.F * pad - BLOCK) / 2.F);

        out.unit = std::min(UNITW, UNITH);
        for (int r = 0; r < N; ++r) {
            const float Y = pad + static_cast<float>(r) * (UNITH + gap);
            float       x = X0;
            auto&       line = out.keys.emplace_back();
            for (const auto& key : all[r]) {
                const float W = UNITW * key.width + gap * (key.width - 1.F);
                line.push_back({x, Y, W, UNITH});
                x += W + gap;
            }
        }
        return out;
    }

    // The key under a box-local point, as {row, column}. The gaps belong to no key.
    inline std::optional<std::pair<int, int>> keyAt(const SGrid& grid, float x, float y) {
        for (size_t r = 0; r < grid.keys.size(); ++r)
            for (size_t c = 0; c < grid.keys[r].size(); ++c)
                if (grid.keys[r][c].contains(x, y))
                    return std::pair{static_cast<int>(r), static_cast<int>(c)};
        return std::nullopt;
    }
}
