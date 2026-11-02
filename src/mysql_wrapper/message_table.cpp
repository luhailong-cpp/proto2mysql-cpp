#include "src/mysql_wrapper/message_table.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/value_codec.h"

namespace pbmysql {
namespace {
using Field = ::google::protobuf::FieldDescriptor;
using Message = ::google::protobuf::Message;
std::string Trim(std::string s) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && space(static_cast<unsigned char>(s.back()))) s.pop_back();
    auto first = std::find_if_not(s.begin(), s.end(), space);
    s.erase(s.begin(), first);
    return s;
}
std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
StringVector Split(const std::string& text, char delim) {
    StringVector out;
    std::size_t start = 0;
    do {
        auto end = text.find(delim, start);
        out.push_back(Trim(text.substr(start, end - start)));
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    return out;
}
std::string Join(const StringVector& values, const std::string& sep) {
    std::string out;
    for (const auto& v : values) {
        if (!out.empty()) out += sep;
        out += v;
    }
    return out;
}
bool Contains(const StringVector& values, const std::string& name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}
bool IsTimestamp(const Field* f) {
    return !f->is_repeated() && f->message_type() &&
           f->message_type()->full_name() == "google.protobuf.Timestamp";
}
bool Integral(const Field* f) {
    return !f->is_repeated() &&
           (f->cpp_type() == Field::CPPTYPE_INT32 || f->cpp_type() == Field::CPPTYPE_UINT32 ||
            f->cpp_type() == Field::CPPTYPE_INT64 || f->cpp_type() == Field::CPPTYPE_UINT64);
}
bool Numeric(const Field* f) {
    return f && !f->is_repeated() &&
           (Integral(f) || f->cpp_type() == Field::CPPTYPE_FLOAT || f->cpp_type() == Field::CPPTYPE_DOUBLE);
}
std::string StringLiteral(const std::string& text) {
    // Hex literals are independent of NO_BACKSLASH_ESCAPES and connection state.
    static constexpr char digits[] = "0123456789abcdef";
    std::string s = "X'";
    for (unsigned char c : text) {
        s += digits[c >> 4];
        s += digits[c & 15];
    }
    return s + "'";
}
std::string Literal(const SqlValue& v) {
    if (std::holds_alternative<std::nullptr_t>(v)) return "NULL";
    if (const auto* s = std::get_if<std::string>(&v)) return StringLiteral(*s);
    return ValueText(v);
}
std::string FieldLiteral(const Message& m, const Field* f) {
    return Literal(FieldValue(m, f));
}
std::string EscapeCache(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%')
            out += "%25";
        else if (c == ':')
            out += "%3A";
        else
            out += c;
    }
    return out;
}
std::size_t Utf8Length(const std::string& value) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size(); ++count) {
        auto c = static_cast<unsigned char>(value[i]);
        unsigned n = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            n = 1;
            cp = c;
        } else if (c >= 0xc2 && c <= 0xdf) {
            n = 2;
            cp = c & 31;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 3;
            cp = c & 15;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 4;
            cp = c & 7;
        } else
            throw std::invalid_argument("invalid UTF-8 key value");
        if (i + n > value.size()) throw std::invalid_argument("invalid UTF-8 key value");
        for (unsigned k = 1; k < n; ++k) {
            auto d = static_cast<unsigned char>(value[i + k]);
            if ((d & 0xc0) != 0x80) throw std::invalid_argument("invalid UTF-8 key value");
            cp = (cp << 6) | (d & 63);
        }
        if ((n == 2 && cp < 128) || (n == 3 && cp < 2048) || (n == 4 && cp < 65536) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff))
            throw std::invalid_argument("invalid UTF-8 key value");
        i += n;
    }
    return count;
}
struct TypeInfo {
    std::string base;
    int length = 0;
    bool unsigned_ = false;
};
TypeInfo ParseType(const std::string& s) {
    std::string text = Lower(Trim(s));
    TypeInfo info;
    auto end = text.find_first_of(" (\t");
    info.base = text.substr(0, end);
    if (info.base == "integer") info.base = "int";
    if (info.base == "bool" || info.base == "boolean") info.base = "tinyint";
    info.unsigned_ = text.find("unsigned") != std::string::npos;
    auto open = text.find('(');
    if (open != std::string::npos) {
        std::size_t used = 0;
        try {
            info.length = std::stoi(text.substr(open + 1), &used);
        } catch (...) {
            throw std::invalid_argument("invalid MySQL column type: " + s);
        }
    }
    return info;
}
int IntRank(const std::string& s) {
    if (s == "tinyint") return 1;
    if (s == "smallint") return 2;
    if (s == "mediumint") return 3;
    if (s == "int") return 4;
    if (s == "bigint") return 5;
    return 0;
}
int BlobRank(const std::string& s) {
    if (s == "tinyblob" || s == "tinytext") return 1;
    if (s == "blob" || s == "text") return 2;
    if (s == "mediumblob" || s == "mediumtext") return 3;
    if (s == "longblob" || s == "longtext") return 4;
    return 0;
}
// Returns -1 when unsafe, 0 when existing is sufficient, 1 for safe expansion.
int Conversion(const std::string& live, const std::string& target) {
    const auto a = ParseType(live), b = ParseType(target);
    int ar = IntRank(a.base), br = IntRank(b.base);
    if (ar || br) {
        if (!ar || !br || a.unsigned_ != b.unsigned_) return -1;
        return br > ar ? 1 : 0;
    }
    if (a.base == b.base) {
        if (a.base == "varchar" || a.base == "varbinary" || a.base == "char" || a.base == "binary" ||
            a.base == "datetime")
            return b.length > a.length ? 1 : 0;
        return 0;
    }
    if (a.base == "float" && b.base == "double") return 1;
    if (a.base == "double" && b.base == "float") return 0;
    int ab = BlobRank(a.base), bb = BlobRank(b.base);
    if (ab && bb &&
        ((a.base.find("blob") != std::string::npos) == (b.base.find("blob") != std::string::npos)))
        return bb > ab ? 1 : 0;
    return -1;
}
int FieldNumber(const std::string& comment) {
    if (comment.rfind("pb:", 0) != 0) return 0;
    std::string s = comment.substr(3);
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return 0;
    try {
        auto n = std::stoll(s);
        return n > 0 && n <= 536870911 ? static_cast<int>(n) : 0;
    } catch (...) {
        return 0;
    }
}
std::optional<std::string> DefaultFor(const std::string& type) {
    auto pos = type.find(" DEFAULT ");
    if (pos == std::string::npos) return std::nullopt;
    auto tail = type.substr(pos + 9);
    if (tail.rfind("''", 0) == 0) return std::string();
    return tail.substr(0, tail.find(' '));
}
std::string IndexName(const std::string& prefix, const std::string& table, const std::string& suffix = "") {
    auto name = prefix + table + suffix;
    if (Utf8Length(name) <= 64) return name;
    uint32_t hash = 2166136261u;
    for (unsigned char c : name) {
        hash ^= c;
        hash *= 16777619u;
    }
    std::ostringstream hex;
    hex << '_' << std::hex << std::setw(8) << std::setfill('0') << hash;
    std::size_t end = 0;
    for (int i = 0; i < 55; ++i) {
        ++end;
        while (end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) ++end;
    }
    return name.substr(0, end) + hex.str();
}
}  // namespace

std::string QuoteIdentifier(const std::string& name) {
    if (name.empty() || Utf8Length(name) > 64 || name.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid MySQL identifier");
    std::string out = "`";
    for (char c : name) {
        out += c;
        if (c == '`') out += '`';
    }
    return out + "`";
}
void EscapeString(std::string& value, MYSQL* mysql) {
    if (!mysql) throw std::invalid_argument("EscapeString requires a connected MySQL handle");
    if (value.size() > std::numeric_limits<unsigned long>::max() / 2)
        throw std::length_error("SQL value is too large");
    std::string out(value.size() * 2 + 1, '\0');
    auto n =
        mysql_real_escape_string(mysql, out.data(), value.data(), static_cast<unsigned long>(value.size()));
    out.resize(n);
    value.swap(out);
}
void ParseFromRow(Message& message, const ResultRow& row) {
    std::vector<SqlValue> values;
    values.reserve(row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (!row[i])
            values.emplace_back(nullptr);
        else
            values.emplace_back(std::string(row[i], row.length(i)));
    }
    ParseRow(message, values);
}
std::string SerializeFieldAsString(const Message& m, const Field* f, MYSQL* mysql) {
    auto s = ValueText(FieldValue(m, f));
    EscapeString(s, mysql);
    return s;
}
std::string GetMysqlFieldType(const Field* f, const std::string& auto_key) {
    if (f->is_repeated()) return "MEDIUMBLOB";
    if (IsTimestamp(f)) return "DATETIME(6)";
    std::string type;
    switch (f->cpp_type()) {
        case Field::CPPTYPE_INT32:
        case Field::CPPTYPE_ENUM:
            type = "int NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_INT64:
            type = "bigint NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_UINT32:
            type = "int unsigned NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_UINT64:
            type = "bigint unsigned NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_FLOAT:
            type = "float NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_DOUBLE:
            type = "double NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_BOOL:
            type = "tinyint(1) NOT NULL DEFAULT 0";
            break;
        case Field::CPPTYPE_STRING:
            type = f->type() == Field::TYPE_BYTES ? "MEDIUMBLOB" : "MEDIUMTEXT";
            break;
        default:
            type = "MEDIUMBLOB";
    }
    if (std::string(f->name()) == auto_key) {
        auto pos = type.find(" DEFAULT 0");
        if (pos != std::string::npos) type.erase(pos, 10);
        type += " AUTO_INCREMENT";
    }
    return type;
}
bool IsTypeMatch(const std::string& live, const std::string& target) {
    return Conversion(live, target) == 0;
}

MessageTable::MessageTable(const Message& source, const TableOptions& o)
    : default_instance_(source.New()), descriptor_(source.GetDescriptor()), options_(descriptor_->options()) {
    registry_key_ = descriptor_->full_name();
    table_name_ =
        options_.HasExtension(OptionTableName) ? options_.GetExtension(OptionTableName) : registry_key_;
    if (options_.HasExtension(OptionPrimaryKey))
        primary_key_ = Split(options_.GetExtension(OptionPrimaryKey), ',');
    if (options_.HasExtension(OptionIndex)) indexes_ = Split(options_.GetExtension(OptionIndex), ';');
    if (options_.HasExtension(OptionUniqueKey)) unique_keys_ = options_.GetExtension(OptionUniqueKey);
    if (options_.HasExtension(OptionAutoIncrementKey))
        auto_increment_key_ = options_.GetExtension(OptionAutoIncrementKey);
    if (options_.HasExtension(proto2mysql::table_name))
        table_name_ = options_.GetExtension(proto2mysql::table_name);
    if (options_.HasExtension(proto2mysql::primary_key))
        primary_key_ = Split(options_.GetExtension(proto2mysql::primary_key), ',');
    if (options_.HasExtension(proto2mysql::index))
        indexes_ = Split(options_.GetExtension(proto2mysql::index), ';');
    if (options_.HasExtension(proto2mysql::unique_key))
        unique_keys_ = Trim(options_.GetExtension(proto2mysql::unique_key));
    if (options_.HasExtension(proto2mysql::auto_increment_key))
        auto_increment_key_ = Trim(options_.GetExtension(proto2mysql::auto_increment_key));
    tidb_nonclustered_pk_ = options_.GetExtension(proto2mysql::tidb_nonclustered_pk);
    tidb_shard_row_id_bits_ = options_.GetExtension(proto2mysql::tidb_shard_row_id_bits);
    tidb_pre_split_regions_ = options_.GetExtension(proto2mysql::tidb_pre_split_regions);
    tidb_auto_id_cache_one_ = options_.GetExtension(proto2mysql::tidb_auto_id_cache_one);
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        if (f->options().GetExtension(proto2mysql::nullable))
            nullable_fields_.push_back(std::string(f->name()));
        if (f->options().HasExtension(proto2mysql::max_length))
            max_lengths_[std::string(f->name())] = f->options().GetExtension(proto2mysql::max_length);
    }
    // Preserve the historic C++ first-field default only for legacy messages.
    if (primary_key_.empty() && !options_.HasExtension(proto2mysql::table_name) && descriptor_->field_count())
        primary_key_.push_back(std::string(descriptor_->field(0)->name()));
    if (o.table_name) table_name_ = *o.table_name;
    if (o.primary_keys) primary_key_ = *o.primary_keys;
    if (o.indexes) indexes_ = *o.indexes;
    if (o.unique_key) unique_keys_ = *o.unique_key;
    if (o.auto_increment_key) auto_increment_key_ = *o.auto_increment_key;
    if (o.nullable_fields) nullable_fields_ = *o.nullable_fields;
    if (o.max_lengths) max_lengths_ = *o.max_lengths;
    if (o.tidb_nonclustered_pk) tidb_nonclustered_pk_ = *o.tidb_nonclustered_pk;
    if (o.tidb_shard_row_id_bits) tidb_shard_row_id_bits_ = *o.tidb_shard_row_id_bits;
    if (o.tidb_pre_split_regions) tidb_pre_split_regions_ = *o.tidb_pre_split_regions;
    if (o.tidb_auto_id_cache_one) tidb_auto_id_cache_one_ = *o.tidb_auto_id_cache_one;
    ValidateOptions();
    if (!primary_key_.empty()) primary_key_field_ = descriptor_->FindFieldByName(primary_key_.front());
}
void MessageTable::ValidateOptions() {
    QuoteIdentifier(table_name_);
    for (int i = 0; i < descriptor_->field_count(); ++i)
        if (descriptor_->field(i)->real_containing_oneof())
            throw std::invalid_argument("real oneof fields cannot be represented as independent columns");
    if (!descriptor_->field_count()) throw std::invalid_argument("cannot map an empty message to a table");
    auto field = [&](const std::string& name) {
        auto* f = descriptor_->FindFieldByName(name);
        if (!f) throw std::invalid_argument("unknown field in table option: " + name);
        QuoteIdentifier(name);
        return f;
    };
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        QuoteIdentifier(std::string(f->name()));
        switch (f->type()) {
            case Field::TYPE_SINT32:
            case Field::TYPE_SINT64:
            case Field::TYPE_FIXED32:
            case Field::TYPE_FIXED64:
            case Field::TYPE_SFIXED32:
            case Field::TYPE_SFIXED64:
                throw std::invalid_argument("unsupported protobuf field kind: " + std::string(f->name()));
            default:
                break;
        }
    }
    auto key = [&](const StringVector& cols, bool primary) {
        std::unordered_set<std::string> seen;
        for (const auto& name : cols) {
            auto* f = field(name);
            if (!seen.insert(name).second) throw std::invalid_argument("duplicate key column: " + name);
            if (f->is_repeated() || f->cpp_type() == Field::CPPTYPE_MESSAGE ||
                f->cpp_type() == Field::CPPTYPE_FLOAT || f->cpp_type() == Field::CPPTYPE_DOUBLE)
                throw std::invalid_argument("key requires a stable scalar field: " + name);
            if (Contains(nullable_fields_, name) && (primary || f->cpp_type() == Field::CPPTYPE_STRING))
                throw std::invalid_argument("key column cannot be nullable: " + name);
            if (f->cpp_type() == Field::CPPTYPE_STRING) identity_columns_.insert(name);
        }
    };
    key(primary_key_, true);
    if (!unique_keys_.empty()) key(Split(unique_keys_, ','), false);
    for (const auto& n : nullable_fields_) field(n);
    for (const auto& pair : max_lengths_) {
        auto* f = field(pair.first);
        if (!identity_columns_.count(pair.first) || f->cpp_type() != Field::CPPTYPE_STRING ||
            pair.second == 0 || pair.second > (f->type() == Field::TYPE_BYTES ? 3072u : 768u))
            throw std::invalid_argument(
                "max_length must describe a string/bytes key within the InnoDB limit: " + pair.first);
    }
    if (!auto_increment_key_.empty()) {
        auto* f = field(auto_increment_key_);
        bool first_key = !primary_key_.empty() && primary_key_.front() == std::string(f->name());
        for (const auto& index : indexes_) {
            const auto columns = Split(index, ',');
            if (!columns.empty() && columns.front() == std::string(f->name())) first_key = true;
        }
        if (!unique_keys_.empty() && Split(unique_keys_, ',').front() == std::string(f->name()))
            first_key = true;
        if (!Integral(f) || Contains(nullable_fields_, std::string(f->name())) || !first_key)
            throw std::invalid_argument("AUTO_INCREMENT must be a nonnullable integer first column of a key");
    }
    auto budget = [&](const StringVector& cols) {
        std::size_t total = 0;
        std::unordered_set<std::string> seen;
        for (const auto& n : cols) {
            auto* f = field(n);
            if (!seen.insert(n).second) throw std::invalid_argument("duplicate index column: " + n);
            if (IsTimestamp(f))
                total += 8;
            else if (f->is_repeated() || f->cpp_type() == Field::CPPTYPE_MESSAGE)
                total += 191;
            else if (f->cpp_type() == Field::CPPTYPE_STRING) {
                auto max = max_lengths_.count(n) ? max_lengths_.at(n) : 191;
                total += (identity_columns_.count(n) ? max : 191) * (f->type() == Field::TYPE_BYTES ? 1 : 4);
            } else if (f->cpp_type() == Field::CPPTYPE_BOOL)
                ++total;
            else if (f->cpp_type() == Field::CPPTYPE_INT64 || f->cpp_type() == Field::CPPTYPE_UINT64 ||
                     f->cpp_type() == Field::CPPTYPE_DOUBLE)
                total += 8;
            else
                total += 4;
        }
        if (total > 3072) throw std::invalid_argument("index exceeds InnoDB 3072-byte limit");
    };
    budget(primary_key_);
    if (!unique_keys_.empty()) budget(Split(unique_keys_, ','));
    for (auto& idx : indexes_) {
        idx = Trim(idx);
        budget(Split(idx, ','));
    }
    if (tidb_shard_row_id_bits_ > 15) throw std::invalid_argument("tidb_shard_row_id_bits exceeds 15");
    if (!primary_key_.empty() && !tidb_nonclustered_pk_) tidb_shard_row_id_bits_ = 0;
    tidb_pre_split_regions_ = std::min(tidb_pre_split_regions_, tidb_shard_row_id_bits_);
}
void MessageTable::ValidateMessage(const Message& m) const {
    if (m.GetDescriptor() != descriptor_)
        throw std::invalid_argument("message type does not match registered table");
    for (const auto& n : identity_columns_) {
        const auto* f = descriptor_->FindFieldByName(n);
        const auto& value = m.GetReflection()->GetString(m, f);
        const auto max = max_lengths_.count(n) ? max_lengths_.at(n) : 191;
        auto length = f->type() == Field::TYPE_BYTES ? value.size() : Utf8Length(value);
        if (length > max) throw std::invalid_argument("key exceeds max_length: " + n);
    }
}
std::string MessageTable::ColumnType(const Field* f) const {
    if (f->containing_type() != descriptor_) throw std::invalid_argument("field does not belong to table");
    if (identity_columns_.count(std::string(f->name()))) {
        auto n = max_lengths_.count(std::string(f->name())) ? max_lengths_.at(std::string(f->name())) : 191;
        return f->type() == Field::TYPE_BYTES
                   ? "VARBINARY(" + std::to_string(n) + ") NOT NULL DEFAULT ''"
                   : "VARCHAR(" + std::to_string(n) +
                         ") CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT ''";
    }
    auto type = GetMysqlFieldType(f, auto_increment_key_);
    if (Contains(nullable_fields_, std::string(f->name()))) {
        auto p = type.find(" NOT NULL");
        if (p != std::string::npos) type.erase(p, 9);
    }
    return type;
}
std::string MessageTable::ColumnListCsv() const {
    StringVector cols;
    for (int i = 0; i < descriptor_->field_count(); ++i)
        cols.push_back(QuoteIdentifier(std::string(descriptor_->field(i)->name())));
    return Join(cols, ", ");
}
std::string MessageTable::CreateTableSql() const {
    StringVector clauses;
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        clauses.push_back(QuoteIdentifier(std::string(f->name())) + " " + ColumnType(f) +
                          " COMMENT 'pb:" + std::to_string(f->number()) + "'");
    }
    auto index = [&](const StringVector& cols, bool prefix) {
        StringVector list;
        for (auto& n : cols) {
            auto* f = descriptor_->FindFieldByName(n);
            auto col = QuoteIdentifier(n);
            if (prefix && !identity_columns_.count(n) &&
                (f->is_repeated() || f->cpp_type() == Field::CPPTYPE_MESSAGE ||
                 f->cpp_type() == Field::CPPTYPE_STRING) &&
                !IsTimestamp(f))
                col += "(191)";
            list.push_back(col);
        }
        return Join(list, ", ");
    };
    if (!primary_key_.empty())
        clauses.push_back("PRIMARY KEY (" + index(primary_key_, false) + ")" +
                          (tidb_nonclustered_pk_ ? " /*T![clustered_index] NONCLUSTERED */" : ""));
    if (!unique_keys_.empty())
        clauses.push_back("UNIQUE KEY " + QuoteIdentifier(IndexName("uk_", table_name_)) + " (" +
                          index(Split(unique_keys_, ','), false) + ")");
    for (std::size_t i = 0; i < indexes_.size(); ++i)
        clauses.push_back("KEY " + QuoteIdentifier(IndexName("idx_", table_name_, "_" + std::to_string(i))) +
                          " (" + index(Split(indexes_[i], ','), true) + ")");
    auto sql = "CREATE TABLE IF NOT EXISTS " + QuoteIdentifier(table_name_) + " (" + Join(clauses, ", ") +
               ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
    if (!auto_increment_key_.empty())
        sql += " AUTO_INCREMENT=" + std::to_string(std::max<uint64_t>(1, auto_increment_));
    if (tidb_shard_row_id_bits_) {
        sql += " /*T! SHARD_ROW_ID_BITS=" + std::to_string(tidb_shard_row_id_bits_);
        if (tidb_pre_split_regions_) sql += " PRE_SPLIT_REGIONS=" + std::to_string(tidb_pre_split_regions_);
        sql += " */";
    }
    if (tidb_auto_id_cache_one_) sql += " /*T![auto_id_cache] AUTO_ID_CACHE=1 */";
    return sql + ";";
}
std::string MessageTable::SelectColumnsSql() const {
    return "SELECT COLUMN_NAME,COLUMN_TYPE,COLUMN_COMMENT,IS_NULLABLE,COLUMN_DEFAULT,EXTRA,COLLATION_NAME "
           "FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = " +
           StringLiteral(db_name_) + " AND TABLE_NAME = " + StringLiteral(table_name_);
}
bool MessageTable::OnSelectColumn(const MYSQL_ROW& row, const unsigned long* lengths, uint32_t size) {
    if (size < 2 || !row || !row[0] || !row[1] || !lengths)
        throw std::invalid_argument("incomplete column metadata");
    ColumnMeta meta;
    meta.type.assign(row[1], lengths[1]);
    if (size > 2 && row[2]) meta.field_num = FieldNumber(std::string(row[2], lengths[2]));
    if (size >= 7) {
        meta.metadata_complete = true;
        meta.nullable = row[3] && std::string(row[3], lengths[3]) == "YES";
        if (row[4]) meta.default_value = std::string(row[4], lengths[4]);
        if (row[5]) meta.extra.assign(row[5], lengths[5]);
        if (row[6]) meta.collation.assign(row[6], lengths[6]);
    }
    if (!existing_columns_.emplace(std::string(row[0], lengths[0]), meta).second)
        throw std::invalid_argument("duplicate column metadata");
    return true;
}
StringVector MessageTable::BuildAlterClauses() const {
    ColumnMap remaining = existing_columns_;
    std::unordered_map<int, std::string> by_number;
    for (const auto& e : remaining)
        if (e.second.field_num) {
            if (!by_number.emplace(e.second.field_num, e.first).second)
                throw std::runtime_error("schema drift: duplicate pb field-number comments");
        }
    StringVector clauses;
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        auto target = ColumnType(f);
        auto comment = " COMMENT 'pb:" + std::to_string(f->number()) + "'";
        auto found = remaining.find(std::string(f->name()));
        bool rename = false;
        if (found == remaining.end()) {
            auto old = by_number.find(f->number());
            if (old != by_number.end()) {
                found = remaining.find(old->second);
                rename = found != remaining.end();
            }
        }
        if (found == remaining.end()) {
            clauses.push_back("ADD COLUMN " + QuoteIdentifier(std::string(f->name())) + " " + target +
                              comment);
            continue;
        }
        const auto& meta = found->second;
        if (meta.field_num && meta.field_num != f->number())
            throw std::runtime_error("schema drift: conflicting field number for " + std::string(f->name()));
        const auto conversion = Conversion(meta.type, target);
        if (conversion < 0)
            throw std::runtime_error("unsafe schema conversion for column " + found->first + ": " +
                                     meta.type + " -> " + target);
        if (meta.metadata_complete) {
            if (meta.nullable != (target.find("NOT NULL") == std::string::npos) ||
                meta.default_value != DefaultFor(target) ||
                (Lower(meta.extra).find("auto_increment") != std::string::npos) !=
                    (std::string(f->name()) == auto_increment_key_) ||
                (!meta.extra.empty() && Lower(meta.extra) != "auto_increment"))
                throw std::runtime_error(
                    "schema drift: NULL/DEFAULT/AUTO_INCREMENT/generated column mismatch for " +
                    found->first);
            if (identity_columns_.count(std::string(f->name())) && f->type() != Field::TYPE_BYTES &&
                Lower(meta.collation) != "utf8mb4_0900_bin")
                throw std::runtime_error(
                    "schema drift: string key requires utf8mb4_0900_bin; migrate legacy key manually: " +
                    found->first);
        }
        // A wider online column remains wider even when its comment/name changes.
        if (!conversion) {
            auto current = ParseType(meta.type), desired = ParseType(target);
            if (current.base != desired.base || current.length > desired.length) {
                std::size_t end = std::string::npos;
                for (const auto* attr :
                     {" NOT NULL", " DEFAULT", " CHARACTER SET", " COLLATE", " AUTO_INCREMENT"})
                    end = std::min(end, target.find(attr));
                target = end == std::string::npos ? meta.type : meta.type + target.substr(end);
            }
        }
        if (rename || conversion || meta.field_num != f->number()) {
            if (expand_only_)
                throw std::runtime_error("expand-only schema mode rejects MODIFY/CHANGE COLUMN: " +
                                         std::string(f->name()));
            clauses.push_back(rename ? "CHANGE COLUMN " + QuoteIdentifier(found->first) + " " +
                                           QuoteIdentifier(std::string(f->name())) + " " + target + comment
                                     : "MODIFY COLUMN " + QuoteIdentifier(std::string(f->name())) + " " +
                                           target + comment);
        }
        remaining.erase(found);
    }
    auto index_clauses = BuildIndexAlterClauses();
    clauses.insert(clauses.end(), index_clauses.begin(), index_clauses.end());
    return clauses;
}
std::string MessageTable::IndexColumns(const StringVector& fields, bool prefix) const {
    StringVector cols;
    for (const auto& n : fields) {
        auto* f = descriptor_->FindFieldByName(n);
        auto col = QuoteIdentifier(n);
        if (prefix && !identity_columns_.count(n) &&
            (f->is_repeated() || f->cpp_type() == Field::CPPTYPE_MESSAGE ||
             f->cpp_type() == Field::CPPTYPE_STRING) &&
            !IsTimestamp(f))
            col += "(191)";
        cols.push_back(col);
    }
    return Join(cols, ", ");
}
std::string MessageTable::SelectIndexesSql() const {
    return "SELECT INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX,COLUMN_NAME,SUB_PART,INDEX_TYPE,COLLATION FROM "
           "INFORMATION_SCHEMA.STATISTICS WHERE TABLE_SCHEMA=" +
           StringLiteral(db_name_) + " AND TABLE_NAME=" + StringLiteral(table_name_) +
           " ORDER BY INDEX_NAME,SEQ_IN_INDEX";
}
bool MessageTable::OnSelectIndex(const MYSQL_ROW& row, const unsigned long* lengths, uint32_t size) {
    indexes_known_ = true;
    if (size < 7 || !row || !lengths || !row[0] || !row[1] || !row[2] || !row[3])
        throw std::runtime_error("schema drift: unsupported or incomplete index metadata");
    auto text = [&](int n) { return row[n] ? std::string(row[n], lengths[n]) : std::string(); };
    auto& idx = existing_indexes_[Lower(text(0))];
    idx.unique = text(1) == "0";
    idx.type = Lower(text(5));
    if (text(6) != "A" && text(6) != "")
        throw std::runtime_error("schema drift: descending index is unsupported");
    IndexColumn col;
    col.sequence = std::stoi(text(2));
    col.name = text(3);
    col.prefix = row[4] ? std::stoi(text(4)) : 0;
    if (col.sequence < 1 || col.prefix < 0) throw std::runtime_error("schema drift: invalid index metadata");
    idx.columns.push_back(std::move(col));
    return true;
}
StringVector MessageTable::BuildIndexAlterClauses() const {
    if (!indexes_known_) return {};
    StringVector clauses;
    auto check = [&](const std::string& name, const StringVector& cols, bool unique, bool prefix,
                     bool primary) {
        const auto found = existing_indexes_.find(Lower(name));
        if (found == existing_indexes_.end()) {
            if (primary) {
                if (!cols.empty())
                    throw std::runtime_error(
                        "schema drift: online primary key is missing; migrate explicitly");
                return;
            }
            clauses.push_back(std::string("ADD ") + (unique ? "UNIQUE KEY " : "KEY ") +
                              QuoteIdentifier(name) + " (" + IndexColumns(cols, prefix) + ")");
            return;
        }
        const auto& meta = found->second;
        auto actual = meta.columns;
        std::sort(actual.begin(), actual.end(),
                  [](const IndexColumn& a, const IndexColumn& b) { return a.sequence < b.sequence; });
        if (meta.unique != unique || meta.type != "btree" || actual.size() != cols.size())
            throw std::runtime_error("schema drift: index definition mismatch: " + name);
        for (std::size_t i = 0; i < cols.size(); ++i) {
            auto* f = descriptor_->FindFieldByName(cols[i]);
            int expected_prefix = prefix && !identity_columns_.count(cols[i]) &&
                                          (f->is_repeated() || f->cpp_type() == Field::CPPTYPE_MESSAGE ||
                                           f->cpp_type() == Field::CPPTYPE_STRING) &&
                                          !IsTimestamp(f)
                                      ? 191
                                      : 0;
            auto online_name = actual[i].name;
            auto old = existing_columns_.find(online_name);
            if (old != existing_columns_.end() && old->second.field_num) {
                auto* renamed = descriptor_->FindFieldByNumber(old->second.field_num);
                if (renamed) online_name = renamed->name();
            }
            if (actual[i].sequence != static_cast<int>(i + 1) || online_name != cols[i] ||
                actual[i].prefix != expected_prefix)
                throw std::runtime_error("schema drift: index columns or prefix mismatch: " + name);
        }
    };
    check("PRIMARY", primary_key_, true, false, true);
    if (!unique_keys_.empty())
        check(IndexName("uk_", table_name_), Split(unique_keys_, ','), true, false, false);
    for (std::size_t i = 0; i < indexes_.size(); ++i)
        check(IndexName("idx_", table_name_, "_" + std::to_string(i)), Split(indexes_[i], ','), false, true,
              false);
    return clauses;
}
std::string MessageTable::AlterTableSql() const {
    auto clauses = BuildAlterClauses();
    return clauses.empty() ? ""
                           : "ALTER TABLE " + QuoteIdentifier(table_name_) + " " + Join(clauses, ", ") + ";";
}

// Legacy literal builders remain available. Runtime CRUD uses SQLBuilder's
// parameterized statements; these helpers use hex literals to avoid SQL-mode
// dependent escaping and must still receive trusted raw SQL conditions.
std::string MessageTable::InsertSql(const Message& m, MYSQL*) const {
    ValidateMessage(m);
    StringVector values;
    for (int i = 0; i < descriptor_->field_count(); ++i)
        values.push_back(FieldLiteral(m, descriptor_->field(i)));
    return "INSERT INTO " + QuoteIdentifier(table_name_) + " (" + ColumnListCsv() + ") VALUES (" +
           Join(values, ", ") + ")";
}
std::string MessageTable::InsertIgnoreSql(const Message& m, MYSQL* c) const {
    if (primary_key_.empty()) throw std::invalid_argument("InsertIgnore needs a key for no-op assignment");
    auto pk = QuoteIdentifier(primary_key_[0]);
    return InsertSql(m, c) + " ON DUPLICATE KEY UPDATE " + pk + "=" + pk;
}
std::string MessageTable::InsertOnDupUpdateSql(const Message& m, MYSQL* c) const {
    StringVector assigns;
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        if (!Contains(primary_key_, std::string(f->name()))) {
            auto name = QuoteIdentifier(std::string(f->name()));
            assigns.push_back(name + "=VALUES(" + name + ")");
        }
    }
    if (assigns.empty()) {
        auto pk = QuoteIdentifier(primary_key_.at(0));
        assigns.push_back(pk + "=" + pk);
    }
    return InsertSql(m, c) + " ON DUPLICATE KEY UPDATE " + Join(assigns, ", ");
}
std::string MessageTable::InsertOnDupKeyForPrimaryKeySql(const Message& m, MYSQL* c) const {
    return InsertIgnoreSql(m, c);
}
std::string MessageTable::ReplaceSql(const Message& m, MYSQL* c) const {
    auto s = InsertSql(m, c);
    s.replace(0, 6, "REPLACE");
    return s;
}
std::string MessageTable::BatchInsertSql(const std::vector<const Message*>& messages, MYSQL* c) const {
    if (messages.empty()) throw std::invalid_argument("empty insert batch");
    StringVector rows;
    for (auto* m : messages) {
        if (!m) throw std::invalid_argument("null message in batch");
        auto s = InsertSql(*m, c);
        rows.push_back(s.substr(s.find("VALUES (") + 7));
    }
    return "INSERT INTO " + QuoteIdentifier(table_name_) + " (" + ColumnListCsv() + ") VALUES " +
           Join(rows, ", ");
}
std::string MessageTable::PrimaryKeyWhereClause(const Message& m, MYSQL*) const {
    ValidateMessage(m);
    if (primary_key_.empty()) throw std::invalid_argument("table has no primary key");
    StringVector keys;
    for (auto& n : primary_key_)
        keys.push_back(QuoteIdentifier(n) + "=" + FieldLiteral(m, descriptor_->FindFieldByName(n)));
    return Join(keys, " AND ");
}
std::string MessageTable::UpdateSetClause(const Message& m, MYSQL*) const {
    ValidateMessage(m);
    StringVector assigns;
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        if (Contains(primary_key_, std::string(f->name()))) continue;
        if (f->is_repeated() ? m.GetReflection()->FieldSize(m, f) > 0 : m.GetReflection()->HasField(m, f))
            assigns.push_back(QuoteIdentifier(std::string(f->name())) + "=" + FieldLiteral(m, f));
    }
    if (assigns.empty()) throw std::invalid_argument("no fields to update");
    return Join(assigns, ", ");
}
std::string MessageTable::UpdateSql(const Message& m, MYSQL* c) const {
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + UpdateSetClause(m, c) + " WHERE " +
           PrimaryKeyWhereClause(m, c);
}
std::string MessageTable::UpdateByWhereSql(const Message& m, MYSQL* c, const std::string& where) const {
    if (Trim(where).empty()) throw std::invalid_argument("empty UPDATE condition");
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + UpdateSetClause(m, c) + " WHERE " + where;
}
std::string MessageTable::UpdateKvByPrimaryKeySql(const Message& m, MYSQL* c, const std::string& n,
                                                  const std::string& value) const {
    if (!HasField(n) || Contains(primary_key_, n)) throw std::invalid_argument("invalid update column: " + n);
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + QuoteIdentifier(n) + "=" +
           StringLiteral(value) + " WHERE " + PrimaryKeyWhereClause(m, c);
}
std::string MessageTable::UpdateFieldsByPrimaryKeySql(const Message& m, MYSQL* c,
                                                      const StringVector& fields) const {
    ValidateMessage(m);
    if (fields.empty()) throw std::invalid_argument("empty update fields");
    StringVector assigns;
    for (auto& n : fields) {
        auto* f = descriptor_->FindFieldByName(n);
        if (!f || Contains(primary_key_, n)) throw std::invalid_argument("invalid update column: " + n);
        assigns.push_back(QuoteIdentifier(n) + "=" + FieldLiteral(m, f));
    }
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + Join(assigns, ", ") + " WHERE " +
           PrimaryKeyWhereClause(m, c);
}
std::string MessageTable::UpdateIfVersionSql(const Message& m, MYSQL* c, const std::string& version) const {
    StringVector fields;
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        auto* f = descriptor_->field(i);
        if (std::string(f->name()) != version && !Contains(primary_key_, std::string(f->name())) &&
            (f->is_repeated() ? m.GetReflection()->FieldSize(m, f) > 0 : m.GetReflection()->HasField(m, f)))
            fields.push_back(std::string(f->name()));
    }
    return UpdateFieldsIfVersionSql(m, c, version, fields);
}
std::string MessageTable::UpdateFieldsIfVersionSql(const Message& m, MYSQL* c, const std::string& version,
                                                   const StringVector& fields) const {
    ValidateMessage(m);
    auto* v = descriptor_->FindFieldByName(version);
    if (!v || !Integral(v) || Contains(primary_key_, version))
        throw std::invalid_argument("invalid version column");
    StringVector assigns;
    for (auto& n : fields) {
        if (n == version) continue;
        auto* f = descriptor_->FindFieldByName(n);
        if (!f || Contains(primary_key_, n)) throw std::invalid_argument("invalid update column: " + n);
        assigns.push_back(QuoteIdentifier(n) + "=" + FieldLiteral(m, f));
    }
    auto name = QuoteIdentifier(version);
    assigns.push_back(name + "=" + name + "+1");
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + Join(assigns, ", ") + " WHERE " +
           PrimaryKeyWhereClause(m, c) + " AND " + name + "=" + FieldLiteral(m, v);
}
std::string MessageTable::IncrByPrimaryKeySql(const Message& m, MYSQL* c, const std::string& n,
                                              int64_t delta) const {
    if (!Numeric(descriptor_->FindFieldByName(n)) || Contains(primary_key_, n))
        throw std::invalid_argument("increment needs a non-key numeric column");
    auto name = QuoteIdentifier(n);
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + name + "=" + name + "+(" +
           std::to_string(delta) + ") WHERE " + PrimaryKeyWhereClause(m, c);
}
std::string MessageTable::DecrByPrimaryKeyIfEnoughSql(const Message& m, MYSQL* c, const std::string& n,
                                                      int64_t delta) const {
    if (delta < 0 || !Numeric(descriptor_->FindFieldByName(n)) || Contains(primary_key_, n))
        throw std::invalid_argument("decrement needs a numeric column and nonnegative delta");
    auto name = QuoteIdentifier(n), d = std::to_string(delta);
    return "UPDATE " + QuoteIdentifier(table_name_) + " SET " + name + "=" + name + "-" + d + " WHERE " +
           PrimaryKeyWhereClause(m, c) + " AND " + name + ">=" + d;
}
std::string MessageTable::DeleteByPrimaryKeySql(const Message& m, MYSQL* c) const {
    return "DELETE FROM " + QuoteIdentifier(table_name_) + " WHERE " + PrimaryKeyWhereClause(m, c);
}
std::string MessageTable::DeleteByKvSql(const std::string& n, const std::string& v, MYSQL*) const {
    if (!HasField(n)) throw std::invalid_argument("unknown field: " + n);
    return "DELETE FROM " + QuoteIdentifier(table_name_) + " WHERE " + QuoteIdentifier(n) + "=" +
           StringLiteral(v);
}
std::string MessageTable::DeleteByWhereSql(const std::string& where) const {
    if (Trim(where).empty()) throw std::invalid_argument("empty DELETE condition");
    return "DELETE FROM " + QuoteIdentifier(table_name_) + " WHERE " + where;
}
std::string MessageTable::TruncateSql() const {
    return "TRUNCATE TABLE " + QuoteIdentifier(table_name_);
}
std::string MessageTable::SelectByPrimaryKeySql(const Message& m, MYSQL* c) const {
    return "SELECT " + ColumnListCsv() + " FROM " + QuoteIdentifier(table_name_) + " WHERE " +
           PrimaryKeyWhereClause(m, c);
}
std::string MessageTable::SelectByPrimaryKeyForUpdateSql(const Message& m, MYSQL* c) const {
    return SelectByPrimaryKeySql(m, c) + " FOR UPDATE";
}
std::string MessageTable::SelectByKvSql(const std::string& n, const std::string& v) const {
    if (!HasField(n)) throw std::invalid_argument("unknown field: " + n);
    return SelectAllByWhereSql(QuoteIdentifier(n) + "=" + StringLiteral(v));
}
std::string MessageTable::SelectByWhereSql(const std::string& where) const {
    return SelectAllByWhereSql(where);
}
std::string MessageTable::SelectAllSql() const {
    return "SELECT " + ColumnListCsv() + " FROM " + QuoteIdentifier(table_name_);
}
std::string MessageTable::SelectAllByWhereSql(const std::string& where) const {
    return SelectAllSql() + (Trim(where).empty() ? "" : " WHERE " + where);
}
std::string MessageTable::SelectAllByPrimaryKeyInSql(const StringVector& values, MYSQL* c) const {
    if (primary_key_.size() != 1)
        throw std::invalid_argument("PK IN requires exactly one primary-key column");
    return SelectAllByKvInSql(primary_key_[0], values, c);
}
std::string MessageTable::SelectAllByKvInSql(const std::string& n, const StringVector& values, MYSQL*) const {
    if (!HasField(n)) throw std::invalid_argument("unknown field: " + n);
    if (values.empty()) return SelectAllByWhereSql("1=0");
    StringVector items;
    for (auto& v : values) items.push_back(StringLiteral(v));
    return SelectAllByWhereSql(QuoteIdentifier(n) + " IN (" + Join(items, ", ") + ")");
}
std::string MessageTable::SelectAllWithOptionsSql(const std::string& where, const QueryOptions& opts) const {
    if (opts.limit < 0 || opts.offset < 0) throw std::invalid_argument("negative limit/offset");
    auto sql = SelectAllByWhereSql(where);
    if (!opts.order_by.empty()) sql += " ORDER BY " + opts.order_by;
    if (opts.limit > 0)
        sql += " LIMIT " + std::to_string(opts.limit);
    else if (opts.offset > 0)
        sql += " LIMIT 18446744073709551615";
    if (opts.offset > 0) sql += " OFFSET " + std::to_string(opts.offset);
    if (opts.for_update) sql += " FOR UPDATE";
    return sql;
}
std::string MessageTable::SelectOneWithOptionsSql(const std::string& where, QueryOptions opts) const {
    opts.limit = 1;
    return SelectAllWithOptionsSql(where, opts);
}
std::string MessageTable::CountSql() const {
    return CountByWhereSql("");
}
std::string MessageTable::CountByWhereSql(const std::string& where) const {
    return "SELECT COUNT(*) FROM " + QuoteIdentifier(table_name_) +
           (Trim(where).empty() ? "" : " WHERE " + where);
}
std::string MessageTable::ExistsByWhereSql(const std::string& where) const {
    return "SELECT 1 FROM " + QuoteIdentifier(table_name_) + (Trim(where).empty() ? "" : " WHERE " + where) +
           " LIMIT 1";
}
std::string MessageTable::ExistsByPrimaryKeySql(const Message& m, MYSQL* c) const {
    return ExistsByWhereSql(PrimaryKeyWhereClause(m, c));
}
std::string MessageTable::CacheKey(const Message& m, MYSQL*) const {
    ValidateMessage(m);
    if (primary_key_.empty()) throw std::invalid_argument("cache requires a primary key");
    auto key = "pb:" + EscapeCache(table_name_);
    for (auto& n : primary_key_)
        key += ":" + EscapeCache(ValueText(FieldValue(m, descriptor_->FindFieldByName(n))));
    return key;
}

}  // namespace pbmysql
