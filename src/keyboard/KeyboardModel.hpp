#pragma once

// What a key does to the keyboard and what it types: pure, no compositor.
// Panel.qml's send(), one rule for one rule. NixOs-dots docs/manual.md#the-keyboard-in-hyprdrives-search

#include "KeyboardLayout.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace Hyprdrive::KeyboardModel {

    using namespace KeyboardLayout;

    struct SState {
        bool    symbols = false;
        bool    shift   = false;
        uint8_t mods    = 0; // EMod bits, one-shot
    };

    // One key to type: `keysym` by name, else the character `ch`. `shift` and
    // `mods` are held around it.
    struct SStroke {
        const char* keysym = nullptr;
        char        ch     = 0;
        bool        shift  = false;
        uint8_t     mods   = 0;
    };

    enum class EEffect : uint8_t { None, Stroke, Close };

    struct SOutcome {
        EEffect effect = EEffect::None;
        SStroke stroke;
    };

    inline char upperAscii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
    inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

    // The text a key types: a space for the bar, uppercase while shift is latched.
    inline char typedChar(const SKey& key, const SState& state) {
        if (key.space)
            return ' ';
        const char C = key.label[0];
        return state.shift ? upperAscii(C) : C;
    }

    // With a modifier latched a text key is a chord, so it must be a key that
    // exists as one: space, period and the letters and digits.
    inline char chordChar(const SKey& key) {
        if (key.space)
            return ' ';
        const char C = lowerAscii(key.label[0]);
        const bool OK = std::strlen(key.label) == 1 && (C == '.' || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'));
        return OK ? C : 0;
    }

    inline std::string displayLabel(const SKey& key, const SState& state) {
        if (key.space)
            return "";
        if (key.kind == EKind::Text && state.shift)
            return std::string(1, upperAscii(key.label[0]));
        return key.label;
    }

    inline bool latched(const SKey& key, const SState& state) {
        return (key.kind == EKind::Shift && state.shift) || (key.kind == EKind::Mod && (state.mods & key.mod));
    }

    // Only BackSpace repeats under a held finger.
    inline bool repeats(const SKey& key) { return key.kind == EKind::Key && std::strcmp(key.keysym, "BackSpace") == 0; }

    // The selected key (row, col) after a pad move: a row change clamps the
    // column to the new row, a column change wraps along its row. `rows` is the
    // layer in view. NixOs-dots docs/manual.md#one-keyboard
    inline void moveSelection(const Rows& rows, int& row, int& col, int dx, int dy) {
        const int N = static_cast<int>(rows.size());
        row         = std::clamp(row + dy, 0, N - 1);
        col         = std::min(col, static_cast<int>(rows[row].size()) - 1);
        if (dx) {
            const int LEN = static_cast<int>(rows[row].size());
            col           = ((col + dx) % LEN + LEN) % LEN;
        }
    }

    inline SOutcome press(SState& state, const SKey& key) {
        switch (key.kind) {
            case EKind::Mod: state.mods ^= key.mod; return {};
            case EKind::Shift: state.shift = !state.shift; return {};
            case EKind::Layer:
                state.symbols = !state.symbols;
                state.shift   = false;
                return {};
            case EKind::Close: return {EEffect::Close, {}};
            case EKind::Paste: return {EEffect::Stroke, {nullptr, 'v', false, MOD_CTRL}};
            case EKind::Key: {
                const SOutcome OUT{EEffect::Stroke, {key.keysym, 0, state.shift, state.mods}};
                state.mods  = 0;
                state.shift = false;
                return OUT;
            }
            case EKind::Text: break;
        }

        if (state.mods) {
            if (const char C = chordChar(key)) {
                const SOutcome OUT{EEffect::Stroke, {nullptr, C, state.shift, state.mods}};
                state.mods  = 0;
                state.shift = false;
                return OUT;
            }
            state.mods = 0; // no chord for this one: the modifier is spent, the text typed
        }
        const SOutcome OUT{EEffect::Stroke, {nullptr, typedChar(key, state), false, 0}};
        state.shift = false;
        return OUT;
    }
}
