#pragma once

#include <cstdint>
#include <google/protobuf/message.h>
#include <string>
#include <variant>
#include <vector>

namespace pbmysql {

// Strings carry either UTF-8 text or opaque, length-preserved binary data.
using SqlValue = std::variant<std::nullptr_t, int64_t, uint64_t, double, std::string>;
struct Statement {
    std::string sql;
    std::vector<SqlValue> args;
};

SqlValue FieldValue(const google::protobuf::Message &message,
                    const google::protobuf::FieldDescriptor *field);
// Predicate binding restores FLOAT to its exact float32 value in a double
// container, matching MySQL's promotion of the stored FLOAT during comparison.
SqlValue FieldComparisonValue(const google::protobuf::Message &message,
                              const google::protobuf::FieldDescriptor *field);
SqlValue NormalizeComparisonValue(const google::protobuf::FieldDescriptor *field,
                                  const SqlValue &value);
// Column order is descriptor field order. Parsing is atomic: errors leave the
// caller's message unchanged, and successful reads clear previous field values.
void ParseRow(google::protobuf::Message &message, const std::vector<SqlValue> &row);
std::string ValueText(const SqlValue &value);
bool IsNumericField(const google::protobuf::FieldDescriptor *field);

} // namespace pbmysql
