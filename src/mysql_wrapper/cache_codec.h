#pragma once

#include <algorithm>
#include <cstdint>
#include <google/protobuf/message.h>
#include <memory>
#include <set>
#include <string>

namespace pbmysql {

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
    while (value >= 128) {
        out += static_cast<char>((value & 127) | 128);
        value >>= 7;
    }
    out += static_cast<char>(value);
}

inline std::string EncodeCacheEntry(const google::protobuf::Message &message) {
    std::set<int> fields;
    const auto *descriptor = message.GetDescriptor();
    for (int i = 0; i < descriptor->field_count(); ++i)
        fields.insert(descriptor->field(i)->number());
    std::string data("P2MC\x01", 5);
    AppendCacheVarint(data, fields.size());
    for (int number : fields)
        AppendCacheVarint(data, number);
    return data + message.SerializeAsString();
}

// Same envelope as Go: a newer reader must never accept an older writer's
// incomplete row. Parse into a scratch message so a corrupt cache preserves PK.
inline bool DecodeCacheEntry(const std::string &data, google::protobuf::Message &message) {
    if (data.size() < 6 || data.compare(0, 5, std::string("P2MC\x01", 5)) != 0)
        return false;
    size_t pos = 5;
    auto read = [&](uint64_t &value) {
        value = 0;
        for (unsigned shift = 0; shift < 64 && pos < data.size(); shift += 7) {
            auto byte = static_cast<unsigned char>(data[pos++]);
            if (shift == 63 && byte > 1)
                return false;
            value |= static_cast<uint64_t>(byte & 127) << shift;
            if (!(byte & 128))
                return true;
        }
        return false;
    };
    uint64_t count;
    if (!read(count) || count > 65536 || count > data.size() - pos)
        return false;
    std::set<int> fields;
    uint64_t previous = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t number;
        if (!read(number) || number == 0 || number > 536870911 || number <= previous)
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
