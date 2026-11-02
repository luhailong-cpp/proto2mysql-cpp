#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>

namespace pbmysql {
// Unicode simple case folding, matching Go strings.EqualFold without depending
// on the host locale, ICU or platform-specific wide-character behavior.
inline std::string FoldIdentifier(const std::string &text) {
    struct Fold {
        uint32_t from;
        uint32_t to;
    };
    static constexpr Fold folds[] = {
#include "src/mysql_wrapper/unicode_simple_fold.inc"
    };
    std::string out;
    for (size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset++]);
        uint32_t rune = first;
        unsigned remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            rune = first & 31;
            remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            rune = first & 15;
            remaining = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            rune = first & 7;
            remaining = 3;
        } else if (first >= 0x80)
            throw std::invalid_argument("invalid UTF-8 identifier");
        for (unsigned i = 0; i < remaining; ++i) {
            if (offset == text.size())
                throw std::invalid_argument("invalid UTF-8 identifier");
            auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0) != 0x80)
                throw std::invalid_argument("invalid UTF-8 identifier");
            rune = rune * 64 + (next & 63);
        }
        if ((remaining == 1 && rune < 128) || (remaining == 2 && rune < 2048) ||
            (remaining == 3 && rune < 65536) || rune > 0x10ffff ||
            (rune >= 0xd800 && rune <= 0xdfff))
            throw std::invalid_argument("invalid UTF-8 identifier");
        const auto *found =
            std::lower_bound(std::begin(folds), std::end(folds), rune,
                             [](const Fold &entry, uint32_t value) { return entry.from < value; });
        if (found != std::end(folds) && found->from == rune)
            rune = found->to;
        if (rune < 128)
            out += static_cast<char>(rune);
        else if (rune < 2048) {
            out += static_cast<char>(0xc0 | (rune >> 6));
            out += static_cast<char>(0x80 | (rune & 63));
        } else if (rune < 65536) {
            out += static_cast<char>(0xe0 | (rune >> 12));
            out += static_cast<char>(0x80 | ((rune >> 6) & 63));
            out += static_cast<char>(0x80 | (rune & 63));
        } else {
            out += static_cast<char>(0xf0 | (rune >> 18));
            out += static_cast<char>(0x80 | ((rune >> 12) & 63));
            out += static_cast<char>(0x80 | ((rune >> 6) & 63));
            out += static_cast<char>(0x80 | (rune & 63));
        }
    }
    return out;
}
} // namespace pbmysql
