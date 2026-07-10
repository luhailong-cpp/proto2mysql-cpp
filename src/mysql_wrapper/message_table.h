#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <mysql.h>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"

#include "src/mysql_wrapper/mysql_result.h"

namespace pbmysql {

using StringVector = std::vector<std::string>;

// Index of the field treated as the default primary-key column when the
// message does not declare one via the (OptionPrimaryKey) option.
inline constexpr int32_t kDefaultPrimaryKeyIndex = 0;

// Fills a protobuf message from a single MySQL result row (column order matches
// the message field order used by the SELECT statements below).
void ParseFromRow(::google::protobuf::Message& message, const ResultRow& row);

// Escapes a string in place using the connection so it is safe to embed inside
// single quotes in a SQL statement.
void EscapeString(std::string& value, MYSQL* mysql);

// Serializes a single protobuf field to its MySQL literal representation
// (already escaped, without surrounding quotes).
std::string SerializeFieldAsString(const ::google::protobuf::Message& message,
    const ::google::protobuf::FieldDescriptor* field, MYSQL* mysql);

// Returns the MySQL column type for a protobuf field, honouring the
// auto-increment option (map/list/message columns are stored as blobs).
std::string GetMysqlFieldType(const ::google::protobuf::FieldDescriptor* field,
    const std::string& auto_increment_key);

// Returns true when a live MySQL column type is compatible with the target
// type generated from the proto definition (mirrors Go isTypeMatch).
bool IsTypeMatch(const std::string& current_type, const std::string& target_type);

// QueryOptions carries ORDER BY / LIMIT / OFFSET modifiers for list queries,
// mirroring the QueryOptions struct of the Go library. order_by is inlined into
// the SQL, so never pass untrusted input to it.
struct QueryOptions {
    std::string order_by;   // e.g. "id DESC"
    int limit = 0;          // <= 0 means no limit
    int offset = 0;         // only applied when limit > 0
};

// MessageTable maps one protobuf message type to a MySQL table and builds every
// SQL statement the library needs for that table. It mirrors the responsibility
// of *MessageTable in the Go proto2mysql library.
class MessageTable {
public:
    // Live column metadata read from information_schema: MySQL type plus the
    // proto field number decoded from the column comment (0 when the column has
    // no "pb:N" comment, e.g. a table created by an older schema version).
    struct ColumnMeta {
        std::string type;
        int field_num = 0;
    };
    using ColumnMap = std::unordered_map<std::string, ColumnMeta>;

    explicit MessageTable(const ::google::protobuf::Message& default_instance);

    void SetAutoIncrement(uint64_t auto_increment) { auto_increment_ = auto_increment; }
    void SetDbName(const std::string& db_name) { db_name_ = db_name; }

    // Registry key: always the protobuf full name (stable across renames).
    const std::string& RegistryKey() const { return registry_key_; }
    // SQL table name: the (OptionTableName) option if present, else full name.
    const std::string& TableName() const { return table_name_; }

    const ::google::protobuf::Message& DefaultInstance() const { return default_instance_; }
    const ::google::protobuf::Descriptor* Descriptor() const { return descriptor_; }
    const ::google::protobuf::FieldDescriptor* PrimaryKeyField() const { return primary_key_field_; }
    const StringVector& PrimaryKeys() const { return primary_key_; }

    // True when the message declares a field with the given name.
    bool HasField(const std::string& field_name) const {
        return descriptor_->FindFieldByName(field_name) != nullptr;
    }

    // Schema management ------------------------------------------------------
    std::string CreateTableSql() const;
    std::string SelectColumnsSql() const;
    // Resets the cached live-column metadata before a fresh information_schema
    // scan (call once before feeding rows to OnSelectColumn).
    void ClearExistingColumns() { existing_columns_.clear(); }
    // Records one existing DB column (name / type / pb:N comment).
    bool OnSelectColumn(const MYSQL_ROW& row, const unsigned long* lengths, uint32_t size);
    // Builds the individual ALTER TABLE clauses (ADD / MODIFY / CHANGE COLUMN)
    // needed to align the live table (from OnSelectColumn) to the proto schema.
    StringVector BuildAlterClauses() const;
    // Full "ALTER TABLE t c1, c2;" statement, or "" when already aligned.
    std::string AlterTableSql() const;

    // Inserts ----------------------------------------------------------------
    std::string InsertSql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string InsertIgnoreSql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string InsertOnDupUpdateSql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string InsertOnDupKeyForPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string ReplaceSql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    // Multi-row INSERT ... VALUES (..),(..) built from a batch of messages.
    std::string BatchInsertSql(const std::vector<const ::google::protobuf::Message*>& messages, MYSQL* mysql) const;

    // Updates ----------------------------------------------------------------
    std::string UpdateSql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string UpdateByWhereSql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const std::string& where_clause) const;
    std::string UpdateKvByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const std::string& field, const std::string& value) const;
    // Partial update of only the named fields, matched by primary key. Returns
    // "" when a field is unknown.
    std::string UpdateFieldsByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const StringVector& fields) const;
    // Optimistic-lock CAS: SET <set-fields>, ver = ver + 1 WHERE pk AND ver = cur.
    // set-fields are the message's set fields (excluding pk and version).
    std::string UpdateIfVersionSql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const std::string& version_field) const;
    // Optimistic-lock CAS with an explicit field list (avoids proto3 implicit
    // presence skipping zero-value fields).
    std::string UpdateFieldsIfVersionSql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const std::string& version_field, const StringVector& fields) const;
    // Atomic delta on a numeric column by primary key (f = f + delta).
    std::string IncrByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const std::string& field, int64_t delta) const;
    // Atomic decrement guarded by f >= delta (f = f - delta ... AND f >= delta).
    std::string DecrByPrimaryKeyIfEnoughSql(const ::google::protobuf::Message& message, MYSQL* mysql,
        const std::string& field, int64_t delta) const;

    // Deletes ----------------------------------------------------------------
    std::string DeleteByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string DeleteByKvSql(const std::string& key, const std::string& value, MYSQL* mysql) const;
    std::string DeleteByWhereSql(const std::string& where_clause) const;
    std::string TruncateSql() const;

    // Selects ----------------------------------------------------------------
    std::string SelectByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string SelectByPrimaryKeyForUpdateSql(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string SelectByKvSql(const std::string& key, const std::string& value) const;
    std::string SelectByWhereSql(const std::string& where_clause) const;
    std::string SelectAllSql() const;
    std::string SelectAllByWhereSql(const std::string& where_clause) const;
    std::string SelectAllByPrimaryKeyInSql(const StringVector& values, MYSQL* mysql) const;
    std::string SelectAllByKvInSql(const std::string& key, const StringVector& values, MYSQL* mysql) const;
    std::string SelectAllWithOptionsSql(const std::string& where_clause, const QueryOptions& opts) const;
    std::string SelectOneWithOptionsSql(const std::string& where_clause, QueryOptions opts) const;

    // Aggregates -------------------------------------------------------------
    std::string CountSql() const;
    std::string CountByWhereSql(const std::string& where_clause) const;
    std::string ExistsByWhereSql(const std::string& where_clause) const;
    std::string ExistsByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const;

    // Builds the cache-aside key "pb:<full name>:<pk values>" for the message.
    std::string CacheKey(const ::google::protobuf::Message& message, MYSQL* mysql) const;

private:
    std::string ColumnListCsv() const;
    std::string UpdateSetClause(const ::google::protobuf::Message& message, MYSQL* mysql) const;
    std::string PrimaryKeyWhereClause(const ::google::protobuf::Message& message, MYSQL* mysql) const;

    const ::google::protobuf::Message& default_instance_;
    const ::google::protobuf::Descriptor* descriptor_{nullptr};
    const ::google::protobuf::MessageOptions& options_;
    const ::google::protobuf::FieldDescriptor* primary_key_field_{nullptr};

    std::string registry_key_;
    std::string table_name_;
    StringVector primary_key_;
    StringVector indexes_;
    std::string unique_keys_;
    std::string auto_increment_key_;
    uint64_t auto_increment_{0};
    std::string db_name_;

    // Existing DB columns discovered by OnSelectColumn (column name -> meta).
    ColumnMap existing_columns_;
};

}  // namespace pbmysql
