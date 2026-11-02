#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <mysql.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Length = unsigned long;
using RowLength = Length *;

// Owns every value, including embedded NUL bytes. NULL differs from empty text.
class ResultRow {
  public:
    explicit ResultRow(std::vector<std::optional<std::string>> values)
        : values_(std::move(values)) {}
    ResultRow(MYSQL_ROW row, const Length *lengths, MYSQL_RES *owned_result, size_t size) {
        // Release the transferred result even if allocation or input validation
        // throws while copying its cells.
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result(owned_result,
                                                                        &mysql_free_result);
        if (size != 0 && (!row || !lengths))
            throw std::invalid_argument("MySQL row values and lengths are required");
        values_.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            if (row[i] == nullptr)
                values_.emplace_back(std::nullopt);
            else
                values_.emplace_back(std::string(row[i], lengths[i]));
        }
    }
    size_t size() const { return values_.size(); }
    const char *operator[](size_t i) const {
        const auto &value = values_.at(i);
        return value ? value->data() : nullptr;
    }
    Length length(size_t i) const {
        const auto &value = values_.at(i);
        if (value && value->size() > std::numeric_limits<Length>::max())
            throw std::length_error("MySQL row value exceeds client length range");
        return value ? static_cast<Length>(value->size()) : 0;
    }
    const std::vector<std::optional<std::string>> &values() const { return values_; }

  private:
    std::vector<std::optional<std::string>> values_;
};
