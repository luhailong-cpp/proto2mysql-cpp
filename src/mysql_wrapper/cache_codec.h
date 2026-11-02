#pragma once

#include <algorithm>
#include <cstdint>
#include <google/protobuf/message.h>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace pbmysql {

namespace cache_wire {
inline constexpr std::string_view kMagic = "P2MC";
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = kMagic.size() + sizeof(kVersion);
inline constexpr unsigned kVarintPayloadBits = 7;
inline constexpr unsigned kContinuationBit = 1u << kVarintPayloadBits;
inline constexpr unsigned kPayloadMask = kContinuationBit - 1;
inline constexpr unsigned kValueBits = std::numeric_limits<std::uint64_t>::digits;
inline constexpr unsigned kLastGroupShift =
    ((kValueBits - 1) / kVarintPayloadBits) * kVarintPayloadBits;
inline constexpr auto kLastGroupMaxValue =
    std::numeric_limits<std::uint64_t>::max() >> kLastGroupShift;
// Matches Go's bounded envelope field-set allocation policy.
inline constexpr std::uint64_t kMaxWriterFieldCount =
    std::uint64_t{std::numeric_limits<std::uint16_t>::max()} + 1;
} // namespace cache_wire

inline std::string EscapeCacheKeyPart(const std::string &value) {
    std::string result;
    for (char ch : value) {
        if (ch == '%')
            result += "%25";
        else if (ch == ':')
            result += "%3A";
        else
            result += ch;
    }
    return result;
}

inline void AppendCacheVarint(std::string &out, uint64_t value) {
    while (value >= cache_wire::kContinuationBit) {
        out += static_cast<char>((value & cache_wire::kPayloadMask) | cache_wire::kContinuationBit);
        value >>= cache_wire::kVarintPayloadBits;
    }
    out += static_cast<char>(value);
}

inline std::string EncodeCacheEntry(const google::protobuf::Message &message) {
    std::set<int> fields;
    const auto *descriptor = message.GetDescriptor();
    for (int i = 0; i < descriptor->field_count(); ++i)
        fields.insert(descriptor->field(i)->number());
    std::string data(cache_wire::kMagic);
    data.push_back(static_cast<char>(cache_wire::kVersion));
    AppendCacheVarint(data, fields.size());
    for (int number : fields)
        AppendCacheVarint(data, number);
    return data + message.SerializeAsString();
}

// Same envelope as Go: a newer reader must never accept an older writer's
// incomplete row. Parse into a scratch message so a corrupt cache preserves PK.
inline bool DecodeCacheEntry(const std::string &data, google::protobuf::Message &message) {
    if (data.size() <= cache_wire::kHeaderSize ||
        data.compare(0, cache_wire::kMagic.size(), cache_wire::kMagic.data(),
                     cache_wire::kMagic.size()) != 0 ||
        static_cast<std::uint8_t>(data[cache_wire::kMagic.size()]) != cache_wire::kVersion)
        return false;
    size_t pos = cache_wire::kHeaderSize;
    auto read = [&](uint64_t &value) {
        value = 0;
        for (unsigned shift = 0; shift < cache_wire::kValueBits && pos < data.size();
             shift += cache_wire::kVarintPayloadBits) {
            auto byte = static_cast<unsigned char>(data[pos++]);
            if (shift == cache_wire::kLastGroupShift && byte > cache_wire::kLastGroupMaxValue)
                return false;
            value |= static_cast<uint64_t>(byte & cache_wire::kPayloadMask) << shift;
            if (!(byte & cache_wire::kContinuationBit))
                return true;
        }
        return false;
    };
    uint64_t count;
    if (!read(count) || count > cache_wire::kMaxWriterFieldCount || count > data.size() - pos)
        return false;
    std::set<int> fields;
    uint64_t previous = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t number;
        if (!read(number) || number == 0 ||
            number > google::protobuf::FieldDescriptor::kMaxNumber || number <= previous)
            return false;
        fields.insert(static_cast<int>(number));
        previous = number;
    }
    const auto *descriptor = message.GetDescriptor();
    for (int i = 0; i < descriptor->field_count(); ++i) {
        if (!fields.count(descriptor->field(i)->number()))
            return false;
    }
    std::unique_ptr<google::protobuf::Message> scratch(message.New());
    if (!scratch->ParseFromString(data.substr(pos)))
        return false;
    message.CopyFrom(*scratch);
    return true;
}

} // namespace pbmysql
