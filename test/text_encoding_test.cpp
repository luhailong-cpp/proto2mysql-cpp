#include "src/mysql_wrapper/database_limits.h"
#include "src/mysql_wrapper/identifier_utils.h"
#include "src/mysql_wrapper/text_encoding.h"
#include <array>
#include <gtest/gtest.h>
#include <utility>

namespace {
using namespace pbmysql;
TEST(TextEncoding, ValidUtf8RoundTripsAcrossEverySequenceWidth) {
    const std::string text = "A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80";
    constexpr std::size_t kExpectedCodePoints = 4;
    EXPECT_EQ(Utf8CodePointCount(text), kExpectedCodePoints);
    std::string encoded;
    for (std::size_t offset = 0; offset < text.size();)
        AppendUtf8(encoded, DecodeUtf8(text, offset));
    EXPECT_EQ(encoded, text);
    const std::array<uint32_t, 8> scalar_boundaries{0,     0x7f,   0x80,    0x7ff,
                                                    0x800, 0xd7ff, 0x10000, 0x10ffff};
    for (auto scalar : scalar_boundaries) {
        std::string bytes;
        AppendUtf8(bytes, scalar);
        std::size_t offset = 0;
        EXPECT_EQ(DecodeUtf8(bytes, offset), scalar);
        EXPECT_EQ(offset, bytes.size());
        EXPECT_EQ(Utf8CodePointCount(bytes), 1u);
    }
}
TEST(TextEncoding, InvalidUtf8AndNonScalarCodePointsAreRejected) {
    for (const std::string bytes :
         {std::string("\x80"), std::string("\xc0\x80"), std::string("\xe0\x80\x80"),
          std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xf0\x9f"),
          std::string("\xc2x")}) {
        EXPECT_THROW(Utf8CodePointCount(bytes), std::invalid_argument);
        EXPECT_THROW(FoldIdentifier(bytes), std::invalid_argument);
    }
    std::string output;
    constexpr uint32_t kFirstSurrogate = 0xd800;
    constexpr uint32_t kAboveUnicodeRange = 0x110000;
    EXPECT_THROW(AppendUtf8(output, kFirstSurrogate), std::invalid_argument);
    EXPECT_THROW(AppendUtf8(output, kAboveUnicodeRange), std::invalid_argument);
    EXPECT_TRUE(output.empty());
}
TEST(TextEncoding, HexAndFnvUseStableStandardRepresentations) {
    EXPECT_EQ(HexEncode(std::string("\0\x01\x7f\xff", 4)), "00017fff");
    EXPECT_EQ(HexEncode(""), "");
    for (const auto &vector : std::array<std::pair<std::string, std::string>, 3>{
             {{"", "811c9dc5"}, {"a", "e40c292c"}, {"foobar", "bf9cf968"}}})
        EXPECT_EQ(Fnv1a32Hex(vector.first), vector.second);
}
TEST(TextEncoding, IdentifierTruncationCountsCodePointsAndPreservesHashSuffix) {
    constexpr std::size_t kShortIdentifierLimit = 12;
    const std::string character = "\xe4\xb8\xad";
    std::string long_name;
    for (std::size_t i = 0; i < kShortIdentifierLimit + 1; ++i)
        long_name += character;
    const auto shortened = TruncateIdentifier(long_name, kShortIdentifierLimit);
    EXPECT_EQ(Utf8CodePointCount(shortened), kShortIdentifierLimit);
    EXPECT_EQ(shortened, character + character + character + "_" + Fnv1a32Hex(long_name));
    EXPECT_EQ(TruncateIdentifier("short", kShortIdentifierLimit), "short");
    EXPECT_THROW(TruncateIdentifier(long_name, 1), std::invalid_argument);
}
TEST(TextEncoding, MySqlIdentifierBoundaryKeepsExistingGeneratedNames) {
    const std::string ascii_at_limit(kMaxIdentifierLength, 'a');
    EXPECT_EQ(TruncateIdentifier(ascii_at_limit, kMaxIdentifierLength), ascii_at_limit);
    const std::string character = "\xe4\xb8\xad";
    std::string unicode_above_limit;
    for (std::size_t i = 0; i <= kMaxIdentifierLength; ++i)
        unicode_above_limit += character;
    constexpr std::string_view expected_suffix = "_833549b4";
    const auto prefix_code_points = kMaxIdentifierLength - expected_suffix.size();
    std::string expected_prefix;
    for (std::size_t i = 0; i < prefix_code_points; ++i)
        expected_prefix += character;
    EXPECT_EQ(TruncateIdentifier(unicode_above_limit, kMaxIdentifierLength),
              expected_prefix + std::string(expected_suffix));
}
} // namespace
