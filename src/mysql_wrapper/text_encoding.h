#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pbmysql {
namespace text_encoding_detail {
inline constexpr unsigned kContinuationPayloadBits = 6;
inline constexpr unsigned char kContinuationPrefix = 0x80;
inline constexpr unsigned char kContinuationMask = 0xc0;
inline constexpr unsigned char kContinuationPayloadMask = 0x3f;
inline constexpr unsigned char kTwoBytePrefix = 0xc0;
inline constexpr unsigned char kThreeBytePrefix = 0xe0;
inline constexpr unsigned char kFourBytePrefix = 0xf0;
inline constexpr unsigned char kFirstTwoByteLead = 0xc2;
inline constexpr unsigned char kLastTwoByteLead = 0xdf;
inline constexpr unsigned char kLastThreeByteLead = 0xef;
inline constexpr unsigned char kLastFourByteLead = 0xf4;
inline constexpr unsigned char kTwoByteLeadPayloadMask = 0x1f;
inline constexpr unsigned char kThreeByteLeadPayloadMask = 0x0f;
inline constexpr unsigned char kFourByteLeadPayloadMask = 0x07;
inline constexpr uint32_t kAsciiLimit = 0x80;
inline constexpr uint32_t kTwoByteCodePointLimit = 0x800;
inline constexpr uint32_t kThreeByteCodePointLimit = 0x10000;
inline constexpr uint32_t kUnicodeMaxCodePoint = 0x10ffff;
inline constexpr uint32_t kSurrogateFirst = 0xd800;
inline constexpr uint32_t kSurrogateLast = 0xdfff;
inline constexpr unsigned kHexDigitBits = 4;
inline constexpr std::string_view kHexDigits = "0123456789abcdef";
inline constexpr uint32_t kFnv1a32OffsetBasis = 2166136261u;
inline constexpr uint32_t kFnv1a32Prime = 16777619u;
inline constexpr std::size_t kHashHexWidth = std::numeric_limits<uint32_t>::digits / kHexDigitBits;
inline constexpr std::string_view kIdentifierHashSeparator = "_";
inline bool IsUnicodeScalar(uint32_t code_point) {
    return code_point <= kUnicodeMaxCodePoint &&
           (code_point < kSurrogateFirst || code_point > kSurrogateLast);
}
} // namespace text_encoding_detail

// Strict Unicode scalar decoding rejects overlong encodings, surrogate code
// points, invalid continuation bytes and truncated sequences.
inline uint32_t DecodeUtf8(std::string_view text, std::size_t &offset) {
    using namespace text_encoding_detail;
    if (offset >= text.size())
        throw std::invalid_argument("truncated UTF-8 value");
    const auto first = static_cast<unsigned char>(text[offset++]);
    uint32_t code_point = first;
    unsigned continuation_count = 0;
    uint32_t minimum = 0;
    if (first < kAsciiLimit)
        return code_point;
    if (first >= kFirstTwoByteLead && first <= kLastTwoByteLead) {
        code_point = first & kTwoByteLeadPayloadMask;
        continuation_count = 1;
        minimum = kAsciiLimit;
    } else if (first >= kThreeBytePrefix && first <= kLastThreeByteLead) {
        code_point = first & kThreeByteLeadPayloadMask;
        continuation_count = 2;
        minimum = kTwoByteCodePointLimit;
    } else if (first >= kFourBytePrefix && first <= kLastFourByteLead) {
        code_point = first & kFourByteLeadPayloadMask;
        continuation_count = 3;
        minimum = kThreeByteCodePointLimit;
    } else
        throw std::invalid_argument("invalid UTF-8 leading byte");
    for (unsigned i = 0; i < continuation_count; ++i) {
        if (offset == text.size())
            throw std::invalid_argument("truncated UTF-8 value");
        const auto next = static_cast<unsigned char>(text[offset++]);
        if ((next & kContinuationMask) != kContinuationPrefix)
            throw std::invalid_argument("invalid UTF-8 continuation byte");
        code_point = (code_point << kContinuationPayloadBits) | (next & kContinuationPayloadMask);
    }
    if (code_point < minimum || !IsUnicodeScalar(code_point))
        throw std::invalid_argument("invalid UTF-8 code point");
    return code_point;
}
inline void AppendUtf8(std::string &output, uint32_t code_point) {
    using namespace text_encoding_detail;
    if (!IsUnicodeScalar(code_point))
        throw std::invalid_argument("invalid Unicode code point");
    unsigned continuation_count = 0;
    unsigned char prefix = 0;
    if (code_point < kAsciiLimit) {
        output += static_cast<char>(code_point);
        return;
    }
    if (code_point < kTwoByteCodePointLimit) {
        continuation_count = 1;
        prefix = kTwoBytePrefix;
    } else if (code_point < kThreeByteCodePointLimit) {
        continuation_count = 2;
        prefix = kThreeBytePrefix;
    } else {
        continuation_count = 3;
        prefix = kFourBytePrefix;
    }
    output +=
        static_cast<char>(prefix | (code_point >> (continuation_count * kContinuationPayloadBits)));
    while (continuation_count > 0) {
        --continuation_count;
        output += static_cast<char>(
            kContinuationPrefix | ((code_point >> (continuation_count * kContinuationPayloadBits)) &
                                   kContinuationPayloadMask));
    }
}
inline std::size_t Utf8CodePointCount(std::string_view text) {
    std::size_t count = 0, offset = 0;
    while (offset < text.size()) {
        DecodeUtf8(text, offset);
        ++count;
    }
    return count;
}
inline std::string HexEncode(std::string_view bytes) {
    using namespace text_encoding_detail;
    static_assert(CHAR_BIT == 2 * kHexDigitBits, "hex encoding requires octet bytes");
    constexpr unsigned char low_mask = (1u << kHexDigitBits) - 1;
    std::string output;
    for (unsigned char byte : bytes) {
        output += kHexDigits[byte >> kHexDigitBits];
        output += kHexDigits[byte & low_mask];
    }
    return output;
}
inline uint32_t Fnv1a32(std::string_view bytes) {
    uint32_t hash = text_encoding_detail::kFnv1a32OffsetBasis;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= text_encoding_detail::kFnv1a32Prime;
    }
    return hash;
}
inline std::string Fnv1a32Hex(std::string_view bytes) {
    using namespace text_encoding_detail;
    auto hash = Fnv1a32(bytes);
    std::string result(kHashHexWidth, '0');
    for (auto position = result.rbegin(); position != result.rend(); ++position) {
        *position = kHexDigits[hash % kHexDigits.size()];
        hash /= static_cast<uint32_t>(kHexDigits.size());
    }
    return result;
}
inline std::string TruncateIdentifier(std::string_view name, std::size_t max_code_points) {
    using namespace text_encoding_detail;
    if (Utf8CodePointCount(name) <= max_code_points)
        return std::string(name);
    const auto suffix = std::string(kIdentifierHashSeparator) + Fnv1a32Hex(name);
    if (max_code_points < suffix.size())
        throw std::invalid_argument("identifier limit cannot fit hash suffix");
    const auto prefix_length = max_code_points - suffix.size();
    std::size_t end = 0;
    for (std::size_t count = 0; count < prefix_length; ++count)
        DecodeUtf8(name, end);
    return std::string(name.substr(0, end)) + suffix;
}
} // namespace pbmysql
