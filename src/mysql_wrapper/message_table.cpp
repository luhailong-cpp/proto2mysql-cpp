#include "src/mysql_wrapper/message_table.h"

#include <boost/algorithm/string.hpp>

#include "muduo/base/Logging.h"
#include "src/util/utility.h"

#include "common_proto/db_base.pb.h"

namespace pbmysql {

namespace {

// MySQL column type per protobuf CppType (index == FieldDescriptor::CppType).
const char* const kCppTypeToMysql[::google::protobuf::FieldDescriptor::MAX_CPPTYPE + 1] = {
    "",                             // 0 (unused)
    "int NOT NULL DEFAULT 0",       // CPPTYPE_INT32  = 1
    "bigint NOT NULL DEFAULT 0",    // CPPTYPE_INT64  = 2
    "int unsigned NOT NULL DEFAULT 0",     // CPPTYPE_UINT32 = 3
    "bigint unsigned NOT NULL DEFAULT 0",  // CPPTYPE_UINT64 = 4
    "double NOT NULL DEFAULT 0",    // CPPTYPE_DOUBLE = 5
    "float NOT NULL DEFAULT 0",     // CPPTYPE_FLOAT  = 6
    "tinyint NOT NULL DEFAULT 0",   // CPPTYPE_BOOL   = 7
    "int NOT NULL DEFAULT 0",       // CPPTYPE_ENUM   = 8
    "varchar(256) NOT NULL DEFAULT ''",  // CPPTYPE_STRING = 9
    "MEDIUMBLOB",                   // CPPTYPE_MESSAGE = 10
};

}  // namespace

void EscapeString(std::string& value, MYSQL* mysql) {
    std::string buffer;
    buffer.resize(value.size() * 2 + 1);
    unsigned long result_size = mysql_real_escape_string(
        mysql, buffer.data(), value.c_str(), static_cast<unsigned long>(value.size()));
    buffer.resize(result_size);
    value.swap(buffer);
}

void ParseFromRow(::google::protobuf::Message& message, const ResultRow& row) {
    const auto reflection = message.GetReflection();
    const auto descriptor = message.GetDescriptor();
    for (int32_t i = 0; i < descriptor->field_count(); ++i) {
        const ::google::protobuf::FieldDescriptor* field = descriptor->field(i);
        switch (field->cpp_type()) {
        case ::google::protobuf::FieldDescriptor::CPPTYPE_INT32:
            reflection->SetInt32(&message, field, StringToNumber<int32_t>(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_INT64:
            reflection->SetInt64(&message, field, StringToNumber<int64_t>(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
            reflection->SetUInt32(&message, field, StringToNumber<uint32_t>(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
            reflection->SetUInt64(&message, field, StringToNumber<uint64_t>(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
            reflection->SetFloat(&message, field, StringToNumber<float>(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
            reflection->SetDouble(&message, field, StringToNumber<double>(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_STRING:
            reflection->SetString(&message, field, row[i] == nullptr ? std::string() : std::string(row[i]));
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
            reflection->SetBool(&message, field,
                (row.length(i) != 0 && row[i] != nullptr) ? StringToNumber<bool>(row[i]) : false);
            break;
        case ::google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
            auto length = row.length(i);
            if (length != 0 && row[i] != nullptr) {
                ::google::protobuf::Message* sub = reflection->MutableMessage(&message, field);
                sub->ParsePartialFromArray(row[i], static_cast<int>(length));
            }
            break;
        }
        default:
            return;
        }
    }
}

std::string SerializeFieldAsString(const ::google::protobuf::Message& message,
    const ::google::protobuf::FieldDescriptor* field, MYSQL* mysql) {
    const auto reflect = message.GetReflection();
    std::string value;
    switch (field->cpp_type()) {
    case ::google::protobuf::FieldDescriptor::CPPTYPE_INT32:
        value = std::to_string(reflect->GetInt32(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
        value = std::to_string(reflect->GetUInt32(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_INT64:
        value = std::to_string(reflect->GetInt64(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_UINT64:
        value = std::to_string(reflect->GetUInt64(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_FLOAT:
        value = std::to_string(reflect->GetFloat(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
        value = std::to_string(reflect->GetDouble(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
        value = std::to_string(reflect->GetBool(message, field) ? 1 : 0);
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_ENUM:
        value = std::to_string(reflect->GetEnumValue(message, field));
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_STRING:
        value = reflect->GetString(message, field);
        break;
    case ::google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
        value = reflect->HasField(message, field)
                    ? reflect->GetMessage(message, field).SerializeAsString()
                    : std::string();
        break;
    default:
        return value;
    }
    EscapeString(value, mysql);
    return value;
}

std::string GetMysqlFieldType(const ::google::protobuf::FieldDescriptor* field,
    const std::string& auto_increment_key) {
    if (field->is_map() || field->is_repeated() ||
        field->cpp_type() == ::google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        return "MEDIUMBLOB";
    }
    std::string type = kCppTypeToMysql[field->cpp_type()];
    if (field->name() == auto_increment_key) {
        boost::replace_all(type, " DEFAULT 0", "");
        type += " AUTO_INCREMENT";
    }
    return type;
}

MessageTable::MessageTable(const ::google::protobuf::Message& default_instance)
    : default_instance_(default_instance),
      descriptor_(default_instance.GetDescriptor()),
      options_(descriptor_->options()) {
    registry_key_ = descriptor_->full_name();

    if (options_.HasExtension(OptionTableName)) {
        table_name_ = options_.GetExtension(OptionTableName);
    } else {
        table_name_ = registry_key_;
    }

    if (options_.HasExtension(OptionPrimaryKey)) {
        boost::split(primary_key_, options_.GetExtension(OptionPrimaryKey), boost::is_any_of(","));
    }
    if (primary_key_.empty() && descriptor_->field_count() > 0) {
        primary_key_.push_back(descriptor_->field(kDefaultPrimaryKeyIndex)->name());
    }
    if (options_.HasExtension(OptionIndex)) {
        boost::split(indexes_, options_.GetExtension(OptionIndex), boost::is_any_of(";"));
    }
    if (options_.HasExtension(OptionUniqueKey)) {
        unique_keys_ = options_.GetExtension(OptionUniqueKey);
    }
    if (options_.HasExtension(OptionAutoIncrementKey)) {
        auto_increment_key_ = options_.GetExtension(OptionAutoIncrementKey);
    }
    if (!primary_key_.empty()) {
        primary_key_field_ = descriptor_->FindFieldByName(primary_key_[0]);
    }
}

std::string MessageTable::ColumnListCsv() const {
    std::string csv;
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        if (i != 0) {
            csv += ", ";
        }
        csv += descriptor_->field(i)->name();
    }
    return csv;
}

std::string MessageTable::CreateTableSql() const {
    std::string sql = "CREATE TABLE IF NOT EXISTS " + table_name_ + " (";
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        const auto* field = descriptor_->field(i);
        if (i != 0) {
            sql += ", ";
        }
        sql += field->name();
        sql += " ";
        sql += GetMysqlFieldType(field, auto_increment_key_);
        // Record the proto field number so future migrations can align columns.
        sql += " COMMENT 'pb:" + std::to_string(field->number()) + "'";
    }

    if (!primary_key_.empty()) {
        sql += ", PRIMARY KEY (";
        sql += boost::join(primary_key_, ",");
        sql += ")";
    }
    if (!unique_keys_.empty()) {
        sql += ", UNIQUE KEY (";
        sql += unique_keys_;
        sql += ")";
    }
    for (const auto& index : indexes_) {
        if (index.empty()) {
            continue;
        }
        sql += ", INDEX (";
        sql += index;
        sql += ")";
    }
    sql += ") ENGINE = INNODB";
    if (!auto_increment_key_.empty()) {
        sql += " AUTO_INCREMENT=1";
    }
    return sql;
}

std::string MessageTable::AlterTableSql() const {
    StringVector clauses = BuildAlterClauses();
    if (clauses.empty()) {
        return "";
    }
    return "ALTER TABLE " + table_name_ + " " + boost::join(clauses, ", ") + ";";
}

namespace {

// Parsed representation of a MySQL column type, e.g. "varchar(256)" or
// "bigint unsigned".
struct MysqlTypeInfo {
    std::string base_type;
    int length = 0;
    int decimal = 0;
    bool is_unsigned = false;
};

MysqlTypeInfo ParseMysqlType(const std::string& col_type) {
    MysqlTypeInfo info;
    std::string lowered = boost::to_lower_copy(col_type);
    StringVector parts;
    boost::split(parts, lowered, boost::is_any_of(" \t"), boost::token_compress_on);
    if (parts.empty() || parts[0].empty()) {
        return info;
    }

    std::string base = parts[0];
    std::size_t open = base.find('(');
    if (open != std::string::npos) {
        info.base_type = base.substr(0, open);
        std::size_t close = base.find(')', open);
        std::string params = base.substr(open + 1,
            close == std::string::npos ? std::string::npos : close - open - 1);
        if (params.find(',') != std::string::npos) {
            StringVector nums;
            boost::split(nums, params, boost::is_any_of(","));
            if (!nums.empty()) {
                try { info.length = std::stoi(boost::trim_copy(nums[0])); } catch (...) {}
            }
            if (nums.size() >= 2) {
                try { info.decimal = std::stoi(boost::trim_copy(nums[1])); } catch (...) {}
            }
        } else if (!params.empty()) {
            try { info.length = std::stoi(boost::trim_copy(params)); } catch (...) {}
        }
    } else {
        info.base_type = base;
    }

    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (parts[i] == "unsigned") {
            info.is_unsigned = true;
            break;
        }
    }
    return info;
}

std::string NormalizeBaseType(const std::string& base) {
    static const std::unordered_map<std::string, std::string> kTypeMap = {
        {"bool", "tinyint"}, {"integer", "int"}, {"mediumtext", "mediumtext"},
        {"text", "text"}, {"blob", "blob"}, {"mediumblob", "mediumblob"},
        {"datetime", "datetime"}, {"timestamp", "datetime"},
        {"varchar", "varchar"}, {"char", "char"},
    };
    auto it = kTypeMap.find(base);
    return it != kTypeMap.end() ? it->second : base;
}

// Decodes the proto field number from a column comment such as "pb:3".
int ParseFieldNumFromComment(const std::string& comment) {
    static const std::string kPrefix = "pb:";
    if (comment.compare(0, kPrefix.size(), kPrefix) != 0) {
        return 0;
    }
    try {
        int n = std::stoi(comment.substr(kPrefix.size()));
        if (n >= 1 && n <= 536870911) {  // protowire valid field-number range
            return n;
        }
    } catch (...) {
    }
    return 0;
}

}  // namespace

bool IsTypeMatch(const std::string& current_type, const std::string& target_type) {
    MysqlTypeInfo current = ParseMysqlType(current_type);
    MysqlTypeInfo target = ParseMysqlType(target_type);

    std::string current_base = NormalizeBaseType(current.base_type);
    std::string target_base = NormalizeBaseType(target.base_type);
    if (current_base != target_base) {
        return false;
    }

    if (current_base == "varchar" || current_base == "char") {
        return target.length >= current.length;
    }
    if (current_base == "int" || current_base == "bigint" ||
        current_base == "tinyint" || current_base == "smallint") {
        return current.is_unsigned == target.is_unsigned;
    }
    if (current_base == "float" || current_base == "double") {
        return target.decimal >= current.decimal;
    }
    return true;
}

std::string MessageTable::SelectColumnsSql() const {
    return "SELECT COLUMN_NAME, COLUMN_TYPE, COLUMN_COMMENT FROM INFORMATION_SCHEMA.COLUMNS"
           " WHERE TABLE_SCHEMA = '" + db_name_ + "' AND TABLE_NAME = '" + table_name_ + "';";
}

bool MessageTable::OnSelectColumn(const MYSQL_ROW& row, const unsigned long* lengths, uint32_t size) {
    if (size == 0 || row[0] == nullptr) {
        return true;
    }
    ColumnMeta meta;
    if (size >= 2 && row[1] != nullptr) {
        meta.type.assign(row[1], lengths[1]);
    }
    if (size >= 3 && row[2] != nullptr) {
        meta.field_num = ParseFieldNumFromComment(std::string(row[2], lengths[2]));
    }
    existing_columns_.emplace(std::string(row[0], lengths[0]), meta);
    return true;
}

StringVector MessageTable::BuildAlterClauses() const {
    ColumnMap remaining = existing_columns_;
    std::unordered_map<int, std::string> by_field_num;
    for (const auto& entry : remaining) {
        if (entry.second.field_num != 0) {
            by_field_num[entry.second.field_num] = entry.first;
        }
    }

    StringVector clauses;
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        const auto* field = descriptor_->field(i);
        const std::string field_name = field->name();
        const int field_num = field->number();
        const std::string target_type = GetMysqlFieldType(field, auto_increment_key_);
        const std::string comment = " COMMENT 'pb:" + std::to_string(field_num) + "'";

        // 1) Exact column-name match: realign type / backfill field-number comment.
        auto exact = remaining.find(field_name);
        if (exact != remaining.end()) {
            if (!IsTypeMatch(exact->second.type, target_type) || exact->second.field_num != field_num) {
                clauses.push_back("MODIFY COLUMN " + field_name + " " + target_type + comment);
            }
            remaining.erase(exact);
            continue;
        }

        // 2) Field-number match (rename): CHANGE COLUMN keeps existing data.
        auto renamed = by_field_num.find(field_num);
        if (renamed != by_field_num.end() && remaining.count(renamed->second)) {
            clauses.push_back("CHANGE COLUMN " + renamed->second + " " + field_name +
                " " + target_type + comment);
            remaining.erase(renamed->second);
            continue;
        }

        // 3) Brand-new field.
        clauses.push_back("ADD COLUMN " + field_name + " " + target_type + comment);
    }
    return clauses;
}

std::string MessageTable::InsertSql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql = "INSERT INTO " + table_name_ + " (" + ColumnListCsv() + ") VALUES (";
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        sql += "'" + SerializeFieldAsString(message, descriptor_->field(i), mysql) + "'";
    }
    sql += ")";
    return sql;
}

std::string MessageTable::InsertIgnoreSql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql = InsertSql(message, mysql);
    sql.replace(0, std::string("INSERT INTO").size(), "INSERT IGNORE INTO");
    return sql;
}

std::string MessageTable::InsertOnDupUpdateSql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    return InsertSql(message, mysql) + " ON DUPLICATE KEY UPDATE " + UpdateSetClause(message, mysql);
}

std::string MessageTable::InsertOnDupKeyForPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql = InsertSql(message, mysql);
    sql += " ON DUPLICATE KEY UPDATE " + primary_key_field_->name() + "='";
    sql += SerializeFieldAsString(message, primary_key_field_, mysql);
    sql += "';";
    return sql;
}

std::string MessageTable::ReplaceSql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql = "REPLACE INTO " + table_name_ + " (" + ColumnListCsv() + ") VALUES (";
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        sql += "'" + SerializeFieldAsString(message, descriptor_->field(i), mysql) + "'";
    }
    sql += ")";
    return sql;
}

std::string MessageTable::BatchInsertSql(const std::vector<const ::google::protobuf::Message*>& messages,
    MYSQL* mysql) const {
    std::string sql = "INSERT INTO " + table_name_ + " (" + ColumnListCsv() + ") VALUES ";
    bool need_comma = false;
    for (const auto* message : messages) {
        if (message == nullptr) {
            continue;
        }
        if (need_comma) {
            sql += ", ";
        }
        need_comma = true;
        sql += "(";
        for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
            if (i != 0) {
                sql += ", ";
            }
            sql += "'" + SerializeFieldAsString(*message, descriptor_->field(i), mysql) + "'";
        }
        sql += ")";
    }
    return need_comma ? sql : std::string();
}

std::string MessageTable::UpdateSetClause(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql;
    const auto reflect = message.GetReflection();
    bool need_comma = false;
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        const auto* field = descriptor_->field(i);
        if (!reflect->HasField(message, field) && !field->is_repeated()) {
            continue;
        }
        if (need_comma) {
            sql += ", ";
        }
        need_comma = true;
        sql += field->name() + "='" + SerializeFieldAsString(message, field, mysql) + "'";
    }
    return sql;
}

std::string MessageTable::PrimaryKeyWhereClause(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql;
    bool need_and = false;
    for (const auto& key : primary_key_) {
        const auto* field = descriptor_->FindFieldByName(key);
        if (nullptr == field) {
            continue;
        }
        if (need_and) {
            sql += " and ";
        }
        need_and = true;
        sql += key + "='" + SerializeFieldAsString(message, field, mysql) + "'";
    }
    return sql;
}

std::string MessageTable::UpdateSql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string sql = "UPDATE " + table_name_ + " SET " + UpdateSetClause(message, mysql);
    sql += " where " + PrimaryKeyWhereClause(message, mysql);
    return sql;
}

std::string MessageTable::UpdateByWhereSql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const std::string& where_clause) const {
    std::string set_clause = UpdateSetClause(message, mysql);
    if (set_clause.empty() || where_clause.empty()) {
        return "";
    }
    return "UPDATE " + table_name_ + " SET " + set_clause + " where " + where_clause;
}

std::string MessageTable::UpdateKvByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const std::string& field, const std::string& value) const {
    std::string escaped = value;
    EscapeString(escaped, mysql);
    std::string sql = "UPDATE " + table_name_ + " SET " + field + "='" + escaped + "'";
    sql += " where " + PrimaryKeyWhereClause(message, mysql);
    return sql;
}

std::string MessageTable::UpdateFieldsByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const StringVector& fields) const {
    if (fields.empty()) {
        return "";
    }
    std::string set_clause;
    bool need_comma = false;
    for (const auto& name : fields) {
        const auto* field = descriptor_->FindFieldByName(name);
        if (nullptr == field) {
            return "";  // unknown field
        }
        if (need_comma) {
            set_clause += ", ";
        }
        need_comma = true;
        set_clause += name + "='" + SerializeFieldAsString(message, field, mysql) + "'";
    }
    return "UPDATE " + table_name_ + " SET " + set_clause +
           " where " + PrimaryKeyWhereClause(message, mysql);
}

std::string MessageTable::UpdateIfVersionSql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const std::string& version_field) const {
    const auto* version_desc = descriptor_->FindFieldByName(version_field);
    if (nullptr == version_desc) {
        return "";
    }
    const std::string current_version = SerializeFieldAsString(message, version_desc, mysql);

    std::unordered_map<std::string, bool> pk_set;
    for (const auto& pk : primary_key_) {
        pk_set[pk] = true;
    }

    const auto reflect = message.GetReflection();
    std::string set_clause;
    bool need_comma = false;
    for (int32_t i = 0; i < descriptor_->field_count(); ++i) {
        const auto* field = descriptor_->field(i);
        const std::string name = field->name();
        if (name == version_field || pk_set.count(name)) {
            continue;
        }
        if (!reflect->HasField(message, field) && !field->is_repeated()) {
            continue;
        }
        if (need_comma) {
            set_clause += ", ";
        }
        need_comma = true;
        set_clause += name + "='" + SerializeFieldAsString(message, field, mysql) + "'";
    }
    if (!need_comma) {
        return "";  // no fields to update
    }
    set_clause += ", " + version_field + "=" + version_field + "+1";

    return "UPDATE " + table_name_ + " SET " + set_clause +
           " where " + PrimaryKeyWhereClause(message, mysql) +
           " and " + version_field + "='" + current_version + "'";
}

std::string MessageTable::UpdateFieldsIfVersionSql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const std::string& version_field, const StringVector& fields) const {
    if (fields.empty()) {
        return "";
    }
    const auto* version_desc = descriptor_->FindFieldByName(version_field);
    if (nullptr == version_desc) {
        return "";
    }
    const std::string current_version = SerializeFieldAsString(message, version_desc, mysql);

    std::string set_clause;
    bool need_comma = false;
    for (const auto& name : fields) {
        if (name == version_field) {
            continue;  // handled by the +1 below
        }
        const auto* field = descriptor_->FindFieldByName(name);
        if (nullptr == field) {
            return "";
        }
        if (need_comma) {
            set_clause += ", ";
        }
        need_comma = true;
        set_clause += name + "='" + SerializeFieldAsString(message, field, mysql) + "'";
    }
    if (need_comma) {
        set_clause += ", ";
    }
    set_clause += version_field + "=" + version_field + "+1";

    return "UPDATE " + table_name_ + " SET " + set_clause +
           " where " + PrimaryKeyWhereClause(message, mysql) +
           " and " + version_field + "='" + current_version + "'";
}

std::string MessageTable::IncrByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const std::string& field, int64_t delta) const {
    if (nullptr == descriptor_->FindFieldByName(field)) {
        return "";
    }
    return "UPDATE " + table_name_ + " SET " + field + "=" + field + "+(" + std::to_string(delta) + ")" +
           " where " + PrimaryKeyWhereClause(message, mysql);
}

std::string MessageTable::DecrByPrimaryKeyIfEnoughSql(const ::google::protobuf::Message& message, MYSQL* mysql,
    const std::string& field, int64_t delta) const {
    if (nullptr == descriptor_->FindFieldByName(field)) {
        return "";
    }
    const std::string delta_str = std::to_string(delta);
    return "UPDATE " + table_name_ + " SET " + field + "=" + field + "-" + delta_str +
           " where " + PrimaryKeyWhereClause(message, mysql) +
           " and " + field + ">=" + delta_str;
}

std::string MessageTable::DeleteByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    return "delete from " + table_name_ + " where " + PrimaryKeyWhereClause(message, mysql);
}

std::string MessageTable::DeleteByKvSql(const std::string& key, const std::string& value, MYSQL* mysql) const {
    std::string escaped = value;
    EscapeString(escaped, mysql);
    return "delete from " + table_name_ + " where " + key + "='" + escaped + "'";
}

std::string MessageTable::DeleteByWhereSql(const std::string& where_clause) const {
    return "delete from " + table_name_ + " where " + where_clause;
}

std::string MessageTable::TruncateSql() const {
    return "TRUNCATE " + table_name_;
}

std::string MessageTable::SelectByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    return "select " + ColumnListCsv() + " from " + table_name_ +
           " where " + PrimaryKeyWhereClause(message, mysql);
}

std::string MessageTable::SelectByPrimaryKeyForUpdateSql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    return "select " + ColumnListCsv() + " from " + table_name_ +
           " where " + PrimaryKeyWhereClause(message, mysql) + " for update";
}

std::string MessageTable::SelectByKvSql(const std::string& key, const std::string& value) const {
    return "select " + ColumnListCsv() + " from " + table_name_ + " where " + key + " = '" + value + "'";
}

std::string MessageTable::SelectByWhereSql(const std::string& where_clause) const {
    return "select " + ColumnListCsv() + " from " + table_name_ + " where " + where_clause;
}

std::string MessageTable::SelectAllSql() const {
    return "select " + ColumnListCsv() + " from " + table_name_;
}

std::string MessageTable::SelectAllByWhereSql(const std::string& where_clause) const {
    return "select " + ColumnListCsv() + " from " + table_name_ + " where " + where_clause;
}

namespace {

// Builds a quoted, escaped, comma-separated list for a SQL IN(...) clause.
std::string BuildInList(const StringVector& values, MYSQL* mysql) {
    std::string list;
    bool need_comma = false;
    for (const auto& value : values) {
        std::string escaped = value;
        EscapeString(escaped, mysql);
        if (need_comma) {
            list += ", ";
        }
        need_comma = true;
        list += "'" + escaped + "'";
    }
    return list;
}

std::string BuildOptionsSuffix(const QueryOptions& opts) {
    std::string suffix;
    if (!opts.order_by.empty()) {
        suffix += " ORDER BY " + opts.order_by;
    }
    if (opts.limit > 0) {
        suffix += " LIMIT " + std::to_string(opts.limit);
        if (opts.offset > 0) {
            suffix += " OFFSET " + std::to_string(opts.offset);
        }
    }
    return suffix;
}

}  // namespace

std::string MessageTable::SelectAllByPrimaryKeyInSql(const StringVector& values, MYSQL* mysql) const {
    if (nullptr == primary_key_field_ || values.empty()) {
        return "";
    }
    return "select " + ColumnListCsv() + " from " + table_name_ +
           " where " + primary_key_field_->name() + " IN (" + BuildInList(values, mysql) + ")";
}

std::string MessageTable::SelectAllByKvInSql(const std::string& key, const StringVector& values, MYSQL* mysql) const {
    if (values.empty()) {
        return "";
    }
    return "select " + ColumnListCsv() + " from " + table_name_ +
           " where " + key + " IN (" + BuildInList(values, mysql) + ")";
}

std::string MessageTable::SelectAllWithOptionsSql(const std::string& where_clause, const QueryOptions& opts) const {
    std::string where = where_clause.empty() ? std::string("1=1") : where_clause;
    return "select " + ColumnListCsv() + " from " + table_name_ + " where " + where + BuildOptionsSuffix(opts);
}

std::string MessageTable::SelectOneWithOptionsSql(const std::string& where_clause, QueryOptions opts) const {
    opts.limit = 1;
    opts.offset = 0;
    return SelectAllWithOptionsSql(where_clause, opts);
}

std::string MessageTable::CountSql() const {
    return "select count(*) from " + table_name_;
}

std::string MessageTable::CountByWhereSql(const std::string& where_clause) const {
    return "select count(*) from " + table_name_ + " where " + where_clause;
}

std::string MessageTable::ExistsByWhereSql(const std::string& where_clause) const {
    return "select 1 from " + table_name_ + " where " + where_clause + " limit 1";
}

std::string MessageTable::ExistsByPrimaryKeySql(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    return "select 1 from " + table_name_ + " where " + PrimaryKeyWhereClause(message, mysql) + " limit 1";
}

std::string MessageTable::CacheKey(const ::google::protobuf::Message& message, MYSQL* mysql) const {
    std::string key = "pb:" + registry_key_;
    for (const auto& pk : primary_key_) {
        const auto* field = descriptor_->FindFieldByName(pk);
        if (nullptr == field) {
            continue;
        }
        key += ":" + SerializeFieldAsString(message, field, mysql);
    }
    return key;
}

}  // namespace pbmysql
