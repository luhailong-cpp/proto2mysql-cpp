#pragma once

#include <mysql.h>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Length = unsigned long;
using RowLength = Length*;

// Owns every value, including embedded NUL bytes. NULL differs from empty text.
class ResultRow {
public:
    explicit ResultRow(std::vector<std::optional<std::string>> values)
        : values_(std::move(values)) {}
    ResultRow(MYSQL_ROW row, RowLength lengths, MYSQL_RES* owned_result, size_t size) {
        values_.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            if (row[i] == nullptr) values_.emplace_back(std::nullopt);
            else values_.emplace_back(std::string(row[i], lengths[i]));
        }
        if (owned_result != nullptr) mysql_free_result(owned_result);
    }
    size_t size() const { return values_.size(); }
    const char* operator[](size_t i) const {
        const auto& value = values_.at(i);
        return value ? value->data() : nullptr;
    }
    Length length(size_t i) const {
        const auto& value = values_.at(i);
        return value ? static_cast<Length>(value->size()) : 0;
    }
    const std::vector<std::optional<std::string>>& values() const { return values_; }
private:
    std::vector<std::optional<std::string>> values_;
};
