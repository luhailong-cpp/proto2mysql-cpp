#pragma once

#include <string>

namespace pbmysql {

// Cache is the cache-aside abstraction used by Database. Business code injects a
// concrete implementation (e.g. a Redis adapter); the library itself has no
// dependency on any cache backend. It mirrors the Cache interface of the Go
// proto2mysql library (Get / Set / Del with a cache-miss signal).
class Cache {
public:
    virtual ~Cache() = default;

    // Get returns true and fills *value on hit, false on miss.
    virtual bool Get(const std::string& key, std::string* value) = 0;

    // Set stores value under key with a time-to-live in seconds (0 == no expiry).
    virtual void Set(const std::string& key, const std::string& value, int ttl_seconds) = 0;

    // Del removes key. Missing keys are ignored.
    virtual void Del(const std::string& key) = 0;
};

}  // namespace pbmysql
