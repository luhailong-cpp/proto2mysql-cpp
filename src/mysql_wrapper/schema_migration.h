#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace pbmysql {

enum class SchemaErrorCode {
    InvalidTableOption,
    InvalidKeyValue,
    SchemaDrift,
    LegacyKeyColumn,
    UnsafeSchemaConversion,
    FieldNumberReused,
    ExpandOnlyViolation,
    IncompleteMetadata
};

class SchemaValidationError : public std::invalid_argument {
  public:
    SchemaValidationError(SchemaErrorCode code, const std::string &message)
        : std::invalid_argument(message), code_(code) {}
    SchemaErrorCode Code() const noexcept { return code_; }
    bool Is(SchemaErrorCode code) const noexcept { return code_ == code; }

  private:
    SchemaErrorCode code_;
};

// A manual migration is deliberately separate from the automatically executable
// alignment plan. Read the checks and warnings before executing any statement.
struct MigrationReport {
    bool shadow_rebuild = false;
    std::vector<std::string> reasons;
    std::vector<std::string> checks;
    std::vector<std::string> statements;
    std::vector<std::string> warnings;
    std::string ToString() const;
};

class SchemaError : public std::runtime_error {
  public:
    SchemaError(SchemaErrorCode code, std::string message, MigrationReport report = {});
    SchemaErrorCode Code() const noexcept { return code_; }
    bool Is(SchemaErrorCode code) const noexcept;
    const MigrationReport &Report() const noexcept { return report_; }

  private:
    SchemaErrorCode code_;
    MigrationReport report_;
};

// The first ALTER aligns ordinary columns and secondary indexes. A missing
// primary key and its AUTO_INCREMENT column are installed together in a second
// ALTER; neither statement drops a column, index, or existing primary key.
struct SchemaAlignmentPlan {
    std::vector<std::string> columns;
    std::vector<std::string> primary_key;
    std::vector<std::string> Statements(const std::string &table_name) const;
};

} // namespace pbmysql
