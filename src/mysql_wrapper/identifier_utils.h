#pragma once

#include "src/mysql_wrapper/text_encoding.h"

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
        auto rune = DecodeUtf8(text, offset);
        const auto *found =
            std::lower_bound(std::begin(folds), std::end(folds), rune,
                             [](const Fold &entry, uint32_t value) { return entry.from < value; });
        if (found != std::end(folds) && found->from == rune)
            rune = found->to;
        AppendUtf8(out, rune);
    }
    return out;
}
} // namespace pbmysql
