#pragma once

#include <cstddef>
#include <cstdint>

namespace pbmysql {

// Shared limits and defaults used by SQL generation and execution. Wire field
// numbers come from protoc; server error codes and ports come from the MySQL SDK.
inline constexpr std::size_t kBatchSize = 1000;
inline constexpr int kConditionalSaveMaxRounds = 2;
inline constexpr std::size_t kDefaultKeyLength = 191;
inline constexpr std::size_t kMaxIndexBytes = 3072;
inline constexpr std::size_t kMaxIdentifierLength = 64;
inline constexpr std::size_t kUtf8MaxBytesPerCodePoint = 4;
inline constexpr std::uint32_t kMaxTiDBShardRowIDBits = 15;
inline constexpr int kTimestampFractionalDigits = 6;

} // namespace pbmysql
