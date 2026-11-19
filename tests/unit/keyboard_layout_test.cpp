// Host test for KeyboardLayout.hpp and KeyboardModel.hpp: the two layers key
// for key, the box maths, and what each key does to the keyboard and types.
#include "KeyboardModel.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace Hyprdrive::KeyboardModel;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static bool near(float a, float b) {
    return std::fabs(a - b) < 0.01F;
}

// The first key of a layer with this label (and kind, when given).
static const SKey* find(bool symbols, const char* label) {
    for (const auto& row : rows(symbols))
        for (const auto& key : row)
            if (std::strcmp(key.label, label) == 0)
                return &key;
    return nullptr;
}

int main() {
    // Five rows, every one 12.5 units wide, so the columns line up.
    for (const bool SYMBOLS : {false, true}) {
        CHECK(rows(SYMBOLS).size() == 5);
        for (const auto& row : rows(SYMBOLS))
            CHECK(near(rowUnits(row), 12.5F));
    }

    // Both layers share the frame: a key for a key, so the pad's selection stays put.
    for (size_t r = 0; r < 5; ++r)
        CHECK(rows(false)[r].size() == rows(true)[r].size());

    // The letters are all there, once.
    {
        std::set<std::string> seen;
        for (const auto& row : rows(false))
            for (const auto& key : row)
                if (key.kind == EKind::Text && std::strlen(key.label) == 1 && key.label[0] >= 'a' && key.label[0] <= 'z')
                    CHECK(seen.insert(key.label).second);
        CHECK(seen.size() == 26);
    }

    // Every named key is a keysym name the keymap lookup will be handed.
    for (const bool SYMBOLS : {false, true})
        for (const auto& row : rows(SYMBOLS))
            for (const auto& key : row)
                if (key.kind == EKind::Key)
                    CHECK(key.keysym[0] != 0);

    // Geometry: the grid fits its box, rows fill the height, keys stay apart.
    {
        const auto& R    = rows(false);
        const auto  GRID = solve(R, 1000.F, 400.F, 6.F, 10.F);
        CHECK(GRID.keys.size() == 5);
        for (const auto& line : GRID.keys) {
            CHECK(line.front().x >= 10.F - 0.01F);
            CHECK(line.back().x + line.back().w <= 1000.F - 10.F + 0.01F);
            for (size_t i = 1; i < line.size(); ++i)
                CHECK(near(line[i].x, line[i - 1].x + line[i - 1].w + 6.F));
        }
        CHECK(near(GRID.keys.front().front().y, 10.F));
        CHECK(near(GRID.keys.back().front().y + GRID.keys.back().front().h, 400.F - 10.F));
        // Every row is the same width, so the right edges line up.
        CHECK(near(GRID.keys[0].back().x + GRID.keys[0].back().w, GRID.keys[2].back().x + GRID.keys[2].back().w));
        CHECK(GRID.unit > 0.F);
    }
    // The compact board on a phone (906x1088) and a TV (1280x720): keys fill the 820 board and the label floor holds.
    {
        const float PHONEH = boardHeightL(1088.F, 4.F), TVH = boardHeightL(720.F, 4.F);
        for (const float H : {PHONEH, TVH}) {
            const auto GRID = solve(rows(false), 820.F, H, 4.F, 6.F);
            CHECK(near(GRID.keys[0].front().x, 6.F));
            CHECK(near(GRID.keys[0].back().x + GRID.keys[0].back().w, 820.F - 6.F));
            CHECK(GRID.keys[0][1].h >= MIN_LABEL_L + 4.F);
        }
    }
    // A board narrower than the screen is centred by its caller; inside it the block is centred too.
    {
        const auto GRID  = solve(rows(false), 600.F, 400.F, 4.F, 6.F);
        const float LEFT  = GRID.keys[0].front().x;
        const float RIGHT = 600.F - (GRID.keys[0].back().x + GRID.keys[0].back().w);
        CHECK(near(LEFT, RIGHT));
    }
    // A narrow portrait box: width binds, nothing leaves the box.
    {
        const auto GRID = solve(rows(true), 360.F, 220.F, 4.F, 6.F);
        for (const auto& line : GRID.keys)
            CHECK(line.back().x + line.back().w <= 360.F - 6.F + 0.01F);
    }

    // The board's height: 19% of the screen between 150 and 260, never past the screen less its margins.
    CHECK(near(boardHeightL(1088.F, 4.F), 206.72F));
    CHECK(near(boardHeightL(720.F, 4.F), 150.F));
    CHECK(near(boardHeightL(2000.F, 4.F), 260.F));
    CHECK(near(boardHeightL(100.F, 4.F), 92.F));
    // Its width: the screen less a margin each side, at most 820.
    CHECK(near(boardWidthL(906.F, 4.F), 820.F));
    CHECK(near(boardWidthL(600.F, 4.F), 592.F));
    CHECK(near(boardWidthL(1920.F, 4.F), 820.F));

    // Hit testing: a key's centre finds it, a gap finds nothing, outside finds nothing.
    {
        const auto GRID = solve(rows(false), 1000.F, 400.F, 6.F, 10.F);
        const auto& K   = GRID.keys[1][2];
        const auto  HIT = keyAt(GRID, K.x + K.w / 2.F, K.y + K.h / 2.F);
        CHECK(HIT && HIT->first == 1 && HIT->second == 2);
        const auto& A = GRID.keys[1][2];
        const auto& B = GRID.keys[1][3];
        CHECK(!keyAt(GRID, (A.x + A.w + B.x) / 2.F, A.y + A.h / 2.F));
        CHECK(!keyAt(GRID, -5.F, 5.F));
        CHECK(!keyAt(GRID, 500.F, 9999.F));
    }

    // Typing: a letter, shifted once, then back.
    {
        SState S;
        const auto* Q = find(false, "q");
        auto        o = press(S, *Q);
        CHECK(o.effect == EEffect::Stroke && o.stroke.ch == 'q' && !o.stroke.shift && o.stroke.mods == 0);
        press(S, *find(false, "⇧"));
        CHECK(S.shift);
        CHECK(displayLabel(*Q, S) == "Q");
        o = press(S, *Q);
        CHECK(o.stroke.ch == 'Q' && !S.shift);
        o = press(S, *Q);
        CHECK(o.stroke.ch == 'q');
    }
    // The bar, the digits and punctuation type themselves; the bar shows nothing.
    {
        SState S;
        const SKey* SPACE = nullptr;
        for (const auto& key : rows(false)[4])
            if (key.space)
                SPACE = &key;
        CHECK(SPACE);
        CHECK(press(S, *SPACE).stroke.ch == ' ');
        CHECK(displayLabel(*SPACE, S).empty());
        CHECK(press(S, *find(false, "7")).stroke.ch == '7');
        CHECK(press(S, *find(false, ".")).stroke.ch == '.');
    }
    // A named key carries the latched modifiers and spends them.
    {
        SState S;
        press(S, *find(false, "ctrl"));
        press(S, *find(false, "alt"));
        CHECK(S.mods == (MOD_CTRL | MOD_ALT));
        CHECK(latched(*find(false, "ctrl"), S) && !latched(*find(false, "super"), S));
        press(S, *find(false, "⇧"));
        const auto o = press(S, *find(false, "tab"));
        CHECK(o.effect == EEffect::Stroke && std::strcmp(o.stroke.keysym, "Tab") == 0);
        CHECK(o.stroke.shift && o.stroke.mods == (MOD_CTRL | MOD_ALT));
        CHECK(S.mods == 0 && !S.shift);
    }
    // A modifier and a letter is a chord; a modifier and punctuation has none, so it types the text.
    {
        SState S;
        press(S, *find(false, "ctrl"));
        auto o = press(S, *find(false, "c"));
        CHECK(o.stroke.ch == 'c' && o.stroke.mods == MOD_CTRL && S.mods == 0);
        press(S, *find(false, "super"));
        o = press(S, *find(false, "-"));
        CHECK(o.stroke.ch == '-' && o.stroke.mods == 0 && S.mods == 0);
        press(S, *find(false, "ctrl"));
        press(S, *find(false, "⇧"));
        o = press(S, *find(false, "a"));
        CHECK(o.stroke.ch == 'a' && o.stroke.shift && o.stroke.mods == MOD_CTRL);
    }
    // Paste is Ctrl+V and spends nothing; the layer key swaps layers and drops shift; close closes.
    {
        SState S;
        press(S, *find(false, "alt"));
        const auto o = press(S, *find(false, "paste"));
        CHECK(o.effect == EEffect::Stroke && o.stroke.ch == 'v' && o.stroke.mods == MOD_CTRL && S.mods == MOD_ALT);
        press(S, *find(false, "⇧"));
        press(S, *find(false, "?123"));
        CHECK(S.symbols && !S.shift);
        CHECK(find(true, "abc") && press(S, *find(true, "abc")).effect == EEffect::None && !S.symbols);
        CHECK(press(S, *find(false, "✕")).effect == EEffect::Close);
    }
    // Only backspace repeats.
    CHECK(repeats(*find(false, "⌫")));
    CHECK(!repeats(*find(false, "tab")));
    CHECK(!repeats(*find(false, "a")));
    CHECK(!repeats(*find(false, "⏎")));

    // The selected key: rows clamp, a row change keeps the column where the new row allows, a column wraps.
    {
        const auto& R = rows(false);
        int         row = 0, col = 0;
        moveSelection(R, row, col, -1, 0);
        CHECK(row == 0 && col == static_cast<int>(R[0].size()) - 1);
        moveSelection(R, row, col, 1, 0);
        CHECK(row == 0 && col == 0);
        moveSelection(R, row, col, 0, -1);
        CHECK(row == 0 && col == 0);
        row = static_cast<int>(R.size()) - 1;
        moveSelection(R, row, col, 0, 1);
        CHECK(row == static_cast<int>(R.size()) - 1);
        row = 0;
        col = static_cast<int>(R[0].size()) - 1;
        for (int i = 0; i < 20; ++i)
            moveSelection(R, row, col, 0, 1);
        CHECK(row == static_cast<int>(R.size()) - 1 && col < static_cast<int>(R[row].size()));
        // A stale selection from the longer layer is clamped by a no-op move.
        row = 0;
        col = 99;
        moveSelection(R, row, col, 0, 0);
        CHECK(col == static_cast<int>(R[0].size()) - 1);
    }

    std::printf("keyboard_layout_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
