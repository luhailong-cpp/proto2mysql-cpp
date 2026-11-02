#include "src/mysql_wrapper/message_table.h"

#include "src/mysql_wrapper/database_limits.h"
#include "src/mysql_wrapper/identifier_utils.h"
#include "src/mysql_wrapper/text_encoding.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace pbmysql {
namespace {
using Field = google::protobuf::FieldDescriptor;
constexpr std::size_t kMediumIntegerIndexBytes =
    3; // MySQL MEDIUMINT has a 24-bit index representation.
constexpr std::string_view kFieldNumberCommentPrefix = "pb:";
constexpr std::string_view kCaseInsensitiveCollationSuffix = "_ci";
constexpr const char *kMySql8FreshMetadataSql =
    "/*!80000 SET SESSION information_schema_stats_expiry = 0 */";
std::string Lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string NameKey(const std::string &name) { return Lower(FoldIdentifier(name)); }
std::string Trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? ""
                                      : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::vector<std::string> Split(const std::string &text, char delimiter = ',') {
    std::vector<std::string> result;
    size_t start = 0;
    for (;;) {
        auto end = text.find(delimiter, start);
        result.push_back(Trim(text.substr(start, end - start)));
        if (end == std::string::npos)
            return result;
        start = end + 1;
    }
}
std::string Join(const std::vector<std::string> &values, const std::string &delimiter = ", ") {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty())
            result += delimiter;
        result += value;
    }
    return result;
}
std::string HexLiteral(const std::string &text) { return "X'" + HexEncode(text) + "'"; }
std::string RowText(const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size, uint32_t n) {
    if (!row || !lengths || n >= size || !row[n])
        return {};
    return std::string(row[n], lengths[n]);
}
template <class T> T Integer(const std::string &text) {
    T value{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw SchemaError(SchemaErrorCode::IncompleteMetadata,
                          "invalid integer in schema metadata");
    return value;
}
std::string BaseType(const std::string &type) {
    auto s = Lower(Trim(type));
    s = s.substr(0, s.find_first_of(" (\t"));
    if (s == "integer")
        s = "int";
    if (s == "bool" || s == "boolean")
        s = "tinyint";
    if (s == "timestamp")
        s = "datetime";
    return s;
}
int TypeLength(const std::string &type) {
    auto a = type.find('('), b = type.find(')', a);
    if (a == std::string::npos)
        return 0;
    auto params = type.substr(a + 1, b - a - 1);
    params = params.substr(0, params.find(','));
    return Integer<int>(params);
}
bool Has(const std::vector<std::string> &names, const std::string &name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
bool HasIntegerType(const std::string &type) {
    auto t = BaseType(type);
    return t == "tinyint" || t == "smallint" || t == "mediumint" || t == "int" || t == "bigint";
}
bool SameFamily(const std::string &a, const std::string &b) {
    for (const auto &family : {std::string("tinyint smallint mediumint int bigint"),
                               std::string("char varchar tinytext text mediumtext longtext"),
                               std::string("binary varbinary tinyblob blob mediumblob longblob"),
                               std::string("float double"), std::string("datetime")}) {
        const auto types = Split(family, ' ');
        if (Has(types, BaseType(a)) && Has(types, BaseType(b)))
            return true;
    }
    return false;
}
bool NeedsPrefix(const std::string &type) {
    auto t = BaseType(type);
    return t.find("text") != std::string::npos || t.find("blob") != std::string::npos;
}
size_t IndexBytes(const std::string &type, int prefix = 0) {
    const auto base = BaseType(type);
    const int length = prefix ? prefix : TypeLength(type);
    if (base == "varchar" || base == "char" || base.find("text") != std::string::npos)
        return static_cast<size_t>(std::max(0, length)) * kUtf8MaxBytesPerCodePoint;
    if (base == "varbinary" || base == "binary" || base.find("blob") != std::string::npos)
        return static_cast<size_t>(std::max(0, length));
    if (base == "tinyint")
        return sizeof(std::uint8_t);
    if (base == "smallint")
        return sizeof(std::uint16_t);
    if (base == "mediumint")
        return kMediumIntegerIndexBytes;
    if (base == "int" || base == "float")
        return sizeof(std::uint32_t);
    if (base == "bigint" || base == "double" || base == "datetime")
        return sizeof(std::uint64_t);
    return 0;
}
std::string SafeGeneratedName(const std::string &name) {
    return TruncateIdentifier(name, kMaxIdentifierLength);
}
std::string ColumnAttributes(const std::string &type) {
    size_t begin = std::string::npos;
    for (const char *attr :
         {" NOT NULL", " DEFAULT", " CHARACTER SET", " COLLATE", " AUTO_INCREMENT"})
        begin = std::min(begin, type.find(attr));
    return begin == std::string::npos ? "" : type.substr(begin);
}
std::optional<std::string> ExpectedDefault(const std::string &type) {
    if (type.find(" DEFAULT ''") != std::string::npos)
        return std::string();
    if (type.find(" DEFAULT 0") != std::string::npos)
        return std::string("0");
    return std::nullopt;
}
std::string NormalizeDefault(std::string value) {
    value = Trim(value);
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
        value = value.substr(1, value.size() - 2);
    return Lower(value);
}
bool DefaultsEqual(const std::optional<std::string> &a, const std::optional<std::string> &b) {
    return a.has_value() == b.has_value() && (!a || NormalizeDefault(*a) == NormalizeDefault(*b));
}
void ReplaceOnce(std::string &target, const std::string &from, const std::string &to) {
    auto pos = target.find(from);
    if (pos == std::string::npos)
        throw std::logic_error("migration template mismatch");
    target.replace(pos, from.size(), to);
}
template <class Map> std::vector<std::string> SortedNames(const Map &values) {
    std::vector<std::string> names;
    for (const auto &entry : values)
        names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}
template <class Index> auto OrderedColumns(const Index &index) {
    auto columns = index.columns;
    std::sort(columns.begin(), columns.end(),
              [](const auto &a, const auto &b) { return a.sequence < b.sequence; });
    return columns;
}
const char *StrictModeSQL =
    "SET SESSION sql_mode = CONCAT_WS(',', NULLIF(@@SESSION.sql_mode, ''), 'STRICT_ALL_TABLES')";
} // namespace

std::string MigrationReport::ToString() const {
    std::ostringstream out;
    out << (shadow_rebuild ? "Manual shadow-table rebuild" : "Manual key-column migration")
        << "; automatic DDL was refused.";
    for (const auto &reason : reasons)
        out << "\n  - " << reason;
    for (const auto &warning : warnings)
        out << "\n  WARNING: " << warning;
    if (!checks.empty()) {
        out << "\nRead-only checks (review every result first):";
        for (const auto &sql : checks)
            out << "\n    " << sql << ";";
    }
    if (!statements.empty()) {
        out << "\nManual SQL (stop writes; execute in order in ONE session):";
        for (const auto &sql : statements)
            out << "\n    " << sql << ";";
    }
    return out.str();
}
SchemaError::SchemaError(SchemaErrorCode code, std::string message, MigrationReport report)
    : std::runtime_error(message + (report.reasons.empty() ? "" : "\n" + report.ToString())),
      code_(code), report_(std::move(report)) {}
bool SchemaError::Is(SchemaErrorCode code) const noexcept {
    return code_ == code ||
           (code == SchemaErrorCode::SchemaDrift && (code_ == SchemaErrorCode::LegacyKeyColumn ||
                                                     code_ == SchemaErrorCode::IncompleteMetadata));
}
std::vector<std::string> SchemaAlignmentPlan::Statements(const std::string &table) const {
    std::vector<std::string> result;
    if (!columns.empty())
        result.push_back("ALTER TABLE " + QuoteIdentifier(table) + " " + Join(columns));
    if (!primary_key.empty())
        result.push_back("ALTER TABLE " + QuoteIdentifier(table) + " " + Join(primary_key));
    return result;
}
void MessageTable::ClearTableMetadata() {
    table_engine_.clear();
    table_collation_.clear();
    online_auto_increment_.reset();
    table_metadata_known_ = false;
}
void MessageTable::ClearDependencies() {
    foreign_keys_.clear();
    triggers_.clear();
    dependencies_known_ = true;
}
std::string MessageTable::SelectColumnsSql() const {
    return "SELECT "
           "COLUMN_NAME,COLUMN_TYPE,COLUMN_COMMENT,IS_NULLABLE,COLUMN_DEFAULT,EXTRA,COLLATION_NAME,"
           "CHARACTER_SET_NAME FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA=" +
           (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_)) +
           " AND TABLE_NAME=" + HexLiteral(table_name_) + " ORDER BY ORDINAL_POSITION";
}
std::string MessageTable::SelectTableMetadataSql() const {
    return "SELECT ENGINE,TABLE_COLLATION,AUTO_INCREMENT FROM INFORMATION_SCHEMA.TABLES WHERE "
           "TABLE_SCHEMA=" +
           (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_)) +
           " AND TABLE_NAME=" + HexLiteral(table_name_);
}
bool MessageTable::OnSelectTableMetadata(const MYSQL_ROW &row, const unsigned long *lengths,
                                         uint32_t size) {
    enum Column : uint32_t { Engine, Collation, AutoIncrement, TableColumnCount };
    if (size < TableColumnCount || !row || !row[Engine] || !row[Collation] || table_metadata_known_)
        throw SchemaError(SchemaErrorCode::IncompleteMetadata,
                          "incomplete or duplicate table metadata");
    table_metadata_known_ = true;
    table_engine_ = RowText(row, lengths, size, Engine);
    table_collation_ = RowText(row, lengths, size, Collation);
    if (row[AutoIncrement])
        online_auto_increment_ = Integer<uint64_t>(RowText(row, lengths, size, AutoIncrement));
    return true;
}
std::string MessageTable::SelectForeignKeysSql() const {
    return "SELECT CONSTRAINT_SCHEMA,CONSTRAINT_NAME,TABLE_NAME,REFERENCED_TABLE_NAME FROM "
           "INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS WHERE (UNIQUE_CONSTRAINT_SCHEMA=" +
           (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_)) +
           " AND REFERENCED_TABLE_NAME=" + HexLiteral(table_name_) + ") OR (CONSTRAINT_SCHEMA=" +
           (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_)) +
           " AND TABLE_NAME=" + HexLiteral(table_name_) +
           ") ORDER BY CONSTRAINT_SCHEMA,CONSTRAINT_NAME";
}
bool MessageTable::OnSelectForeignKey(const MYSQL_ROW &row, const unsigned long *lengths,
                                      uint32_t size) {
    enum Column : uint32_t { Schema, Name, OwnerTable, ReferencedTable, ForeignKeyColumnCount };
    if (size < ForeignKeyColumnCount || !row || !row[Schema] || !row[Name] || !row[OwnerTable] ||
        !row[ReferencedTable])
        throw SchemaError(SchemaErrorCode::IncompleteMetadata, "incomplete foreign-key metadata");
    dependencies_known_ = true;
    foreign_keys_.push_back(RowText(row, lengths, size, Schema) + "." +
                            RowText(row, lengths, size, Name) + " (" +
                            RowText(row, lengths, size, OwnerTable) + " -> " +
                            RowText(row, lengths, size, ReferencedTable) + ")");
    return true;
}
std::string MessageTable::SelectTriggersSql() const {
    return "SELECT TRIGGER_NAME,ACTION_TIMING,EVENT_MANIPULATION FROM INFORMATION_SCHEMA.TRIGGERS "
           "WHERE TRIGGER_SCHEMA=" +
           (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_)) +
           " AND EVENT_OBJECT_TABLE=" + HexLiteral(table_name_) + " ORDER BY TRIGGER_NAME";
}
bool MessageTable::OnSelectTrigger(const MYSQL_ROW &row, const unsigned long *lengths,
                                   uint32_t size) {
    enum Column : uint32_t { Name, Timing, Event, TriggerColumnCount };
    if (size < TriggerColumnCount || !row || !row[Name] || !row[Timing] || !row[Event])
        throw SchemaError(SchemaErrorCode::IncompleteMetadata, "incomplete trigger metadata");
    dependencies_known_ = true;
    triggers_.push_back(RowText(row, lengths, size, Name) + " (" +
                        RowText(row, lengths, size, Timing) + " " +
                        RowText(row, lengths, size, Event) + ")");
    return true;
}
bool MessageTable::OnSelectColumn(const MYSQL_ROW &row, const unsigned long *lengths,
                                  uint32_t size) {
    enum Column : uint32_t {
        Name,
        Type,
        Comment,
        Nullable,
        Default,
        Extra,
        Collation,
        Charset,
        ColumnCount
    };
    if (size <= Type || !row || !row[Name] || !row[Type] || !lengths)
        throw SchemaError(SchemaErrorCode::IncompleteMetadata, "incomplete column metadata");
    ColumnMeta meta;
    meta.type = RowText(row, lengths, size, Type);
    const auto comment = RowText(row, lengths, size, Comment);
    if (comment.rfind(kFieldNumberCommentPrefix, 0) == 0) {
        auto num = comment.substr(kFieldNumberCommentPrefix.size());
        if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos) {
            auto value = Integer<uint64_t>(num);
            if (value > 0 && value <= Field::kMaxNumber)
                meta.field_num = static_cast<int>(value);
        }
    }
    if (size >= Charset) {
        if (!row[Nullable])
            throw SchemaError(SchemaErrorCode::IncompleteMetadata, "IS_NULLABLE metadata is NULL");
        meta.metadata_complete = true;
        meta.nullable = Lower(RowText(row, lengths, size, Nullable)) == "yes";
        if (row[Default])
            meta.default_value = RowText(row, lengths, size, Default);
        meta.extra = Lower(RowText(row, lengths, size, Extra));
        meta.collation = RowText(row, lengths, size, Collation);
        meta.charset = size >= ColumnCount ? RowText(row, lengths, size, Charset)
                                           : meta.collation.substr(0, meta.collation.find('_'));
    }
    const auto name = RowText(row, lengths, size, Name);
    if (!existing_columns_.emplace(name, std::move(meta)).second)
        throw SchemaError(SchemaErrorCode::SchemaDrift, "duplicate column metadata: " + name);
    return true;
}
std::string MessageTable::SelectIndexesSql() const {
    return "SELECT INDEX_NAME,NON_UNIQUE,SEQ_IN_INDEX,COLUMN_NAME,SUB_PART,INDEX_TYPE,COLLATION "
           "FROM INFORMATION_SCHEMA.STATISTICS WHERE TABLE_SCHEMA=" +
           (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_)) +
           " AND TABLE_NAME=" + HexLiteral(table_name_) + " ORDER BY INDEX_NAME,SEQ_IN_INDEX";
}
bool MessageTable::OnSelectIndex(const MYSQL_ROW &row, const unsigned long *lengths,
                                 uint32_t size) {
    enum Column : uint32_t {
        Name,
        NonUnique,
        Sequence,
        ColumnName,
        Prefix,
        Type,
        Direction,
        IndexColumnCount
    };
    indexes_known_ = true;
    if (size <= Type || !row || !lengths || !row[Name] || !row[NonUnique] || !row[Sequence])
        throw SchemaError(SchemaErrorCode::IncompleteMetadata, "incomplete index metadata");
    const auto name = RowText(row, lengths, size, Name), key = NameKey(name);
    auto found = existing_indexes_.find(key);
    bool unique = RowText(row, lengths, size, NonUnique) == "0";
    auto type = Lower(RowText(row, lengths, size, Type));
    if (found != existing_indexes_.end() &&
        (found->second.name != name || found->second.unique != unique ||
         found->second.type != type))
        throw SchemaError(SchemaErrorCode::SchemaDrift,
                          "inconsistent or case-ambiguous index metadata: " + name);
    auto &index = existing_indexes_[key];
    index.name = name;
    index.unique = unique;
    index.type = type;
    IndexColumn column;
    column.sequence = Integer<int>(RowText(row, lengths, size, Sequence));
    column.name = RowText(row, lengths, size, ColumnName);
    column.prefix = row[Prefix] ? Integer<int>(RowText(row, lengths, size, Prefix)) : 0;
    column.descending =
        size >= IndexColumnCount && Lower(RowText(row, lengths, size, Direction)) == "d";
    if (column.sequence < 1 || column.prefix < 0 ||
        std::any_of(
            index.columns.begin(), index.columns.end(),
            [&](const auto &c) { return c.sequence == column.sequence; }))
        throw SchemaError(SchemaErrorCode::SchemaDrift,
                          "invalid or duplicate index ordinal: " + name);
    index.columns.push_back(std::move(column));
    return true;
}

std::vector<MessageTable::ColumnMatch> MessageTable::MatchExistingColumns() const {
    std::map<std::string, std::string> folded;
    std::map<int, std::string> numbered;
    std::unordered_set<std::string> used;
    for (const auto &name : SortedNames(existing_columns_)) {
        const auto &meta = existing_columns_.at(name);
        if (!folded.emplace(NameKey(name), name).second)
            throw SchemaError(SchemaErrorCode::SchemaDrift,
                              "case-ambiguous online column: " + name);
        if (meta.field_num && !numbered.emplace(meta.field_num, name).second)
            throw SchemaError(SchemaErrorCode::SchemaDrift,
                              "duplicate pb:" + std::to_string(meta.field_num) +
                                  " column identities");
    }
    std::vector<ColumnMatch> matches;
    for (int i = 0; i < descriptor_->field_count(); ++i) {
        ColumnMatch match;
        match.field = descriptor_->field(i);
        auto found = folded.find(NameKey(std::string(match.field->name())));
        if (found != folded.end()) {
            match.online_name = found->second;
            match.meta = &existing_columns_.at(match.online_name);
            used.insert(match.online_name);
        }
        matches.push_back(match);
    }
    for (auto &match : matches)
        if (!match.meta) {
            auto found = numbered.find(match.field->number());
            if (found != numbered.end() && !used.count(found->second)) {
                match.online_name = found->second;
                match.meta = &existing_columns_.at(match.online_name);
                used.insert(match.online_name);
            }
        }
    return matches;
}
std::string MessageTable::LegacyKeyReason(const ColumnMatch &column) const {
    if (!column.meta || !identity_columns_.count(std::string(column.field->name())))
        return {};
    auto base = BaseType(column.meta->type);
    if (column.field->type() == Field::TYPE_BYTES) {
        if (base == "varbinary")
            return {};
        if (base == "binary")
            return "BINARY pads with zero bytes; use VARBINARY for exact byte identity";
        return "bytes key is not VARBINARY; BLOB prefix indexes do not enforce full-value "
               "uniqueness";
    }
    if (base != "varchar")
        return base == "char"
                   ? "CHAR ignores trailing spaces; use VARCHAR with NO PAD binary collation"
                   : "string key is not VARCHAR; TEXT prefix indexes do not enforce full-value "
                     "uniqueness";
    auto collation = Lower(column.meta->collation);
    if (collation == "utf8mb4_0900_bin")
        return {};
    if (collation.empty())
        return "string key collation metadata is missing";
    if (collation.size() >= kCaseInsensitiveCollationSuffix.size() &&
        collation.substr(collation.size() - kCaseInsensitiveCollationSuffix.size()) ==
            kCaseInsensitiveCollationSuffix)
        return "case-insensitive key collation " + column.meta->collation;
    return "key collation " + column.meta->collation +
           " does not guarantee codepoint and trailing-space identity";
}
std::string MessageTable::AlignedColumnType(const ColumnMatch &column) const {
    auto target = ColumnType(column.field);
    if (!column.meta || !IsTypeMatch(column.meta->type, target))
        return target;
    std::string aligned = column.meta->type + ColumnAttributes(target);
    if (!column.meta->collation.empty() && aligned.find("COLLATE") == std::string::npos) {
        std::string charset = column.meta->charset;
        if (charset.empty())
            charset = column.meta->collation.substr(0, column.meta->collation.find('_'));
        for (char c : charset + column.meta->collation)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                throw SchemaError(SchemaErrorCode::IncompleteMetadata,
                                  "invalid charset/collation metadata");
        aligned = column.meta->type + " CHARACTER SET " + charset + " COLLATE " +
                  column.meta->collation + ColumnAttributes(target);
    }
    return aligned;
}
std::string MessageTable::IndexColumns(const StringVector &fields, bool prefix) const {
    StringVector columns;
    for (const auto &name : fields) {
        auto field = descriptor_->FindFieldByName(name);
        auto sql = QuoteIdentifier(name);
        if (prefix && NeedsPrefix(ColumnType(field)))
            sql += "(" + std::to_string(kDefaultKeyLength) + ")";
        columns.push_back(sql);
    }
    return Join(columns);
}
SchemaAlignmentPlan MessageTable::PlanSchemaAlignment() const {
    const auto matches = MatchExistingColumns();
    auto report = LegacyMigrationReport();
    if (!report.reasons.empty())
        throw SchemaError(SchemaErrorCode::LegacyKeyColumn,
                          "legacy key columns require a reviewed manual migration",
                          std::move(report));
    if (table_metadata_known_ && Lower(table_engine_) != "innodb")
        throw SchemaError(SchemaErrorCode::SchemaDrift,
                          "online table engine is not InnoDB: " + table_engine_);
    SchemaAlignmentPlan plan;
    StringVector drifts;
    struct Clause {
        std::string sql;
        std::string column;
        bool auto_increment = false;
    };
    std::vector<Clause> clauses;
    for (const auto &column : matches) {
        auto *field = column.field;
        auto name = std::string(field->name()), target = ColumnType(field);
        const auto comment = " COMMENT 'pb:" + std::to_string(field->number()) + "'";
        if (!column.meta) {
            clauses.push_back({"ADD COLUMN " + QuoteIdentifier(name) + " " + target + comment, name,
                               target.find("AUTO_INCREMENT") != std::string::npos});
            continue;
        }
        const auto &meta = *column.meta;
        bool rename = NameKey(column.online_name) != NameKey(name);
        const bool sign_change = HasIntegerType(meta.type) &&
                                 ((Lower(meta.type).find("unsigned") != std::string::npos) !=
                                  (Lower(target).find("unsigned") != std::string::npos));
        if (!SameFamily(meta.type, target) || sign_change)
            throw SchemaError(rename && !sign_change ? SchemaErrorCode::FieldNumberReused
                                                     : SchemaErrorCode::UnsafeSchemaConversion,
                              "unsafe schema conversion " + column.online_name + " -> " + name +
                                  ": " + meta.type + " -> " + target);
        if (meta.metadata_complete) {
            if (meta.nullable != (target.find("NOT NULL") == std::string::npos))
                drifts.push_back(name + " nullable mismatch");
            if (!DefaultsEqual(meta.default_value, ExpectedDefault(target)))
                drifts.push_back(name + " default mismatch");
            if ((meta.extra.find("auto_increment") != std::string::npos) !=
                (name == auto_increment_key_))
                drifts.push_back(name + " auto_increment mismatch");
            if (!meta.extra.empty() && meta.extra != "auto_increment")
                drifts.push_back(name + " unsupported EXTRA: " + meta.extra);
            if (!meta.charset.empty() && Lower(meta.charset) != "utf8mb4" &&
                field->type() == Field::TYPE_STRING && !field->is_repeated())
                drifts.push_back(name + " character set mismatch: " + meta.charset);
        }
        if (rename || !IsTypeMatch(meta.type, target) || meta.field_num != field->number()) {
            auto aligned = AlignedColumnType(column);
            if (expand_only_)
                throw SchemaError(SchemaErrorCode::ExpandOnlyViolation,
                                  "expand-only mode rejects MODIFY/CHANGE COLUMN: " + name);
            clauses.push_back(
                {(rename ? "CHANGE COLUMN " + QuoteIdentifier(column.online_name) + " "
                         : "MODIFY COLUMN ") +
                     QuoteIdentifier(name) + " " + aligned + comment,
                 name, aligned.find("AUTO_INCREMENT") != std::string::npos});
        }
    }
    auto index_clauses = BuildIndexAlterClauses();
    if (!drifts.empty())
        throw SchemaError(SchemaErrorCode::SchemaDrift,
                          "table " + table_name_ + ": " + Join(drifts, "; "));
    const bool missing_pk =
        indexes_known_ && !primary_key_.empty() && !existing_indexes_.count("primary");
    std::unordered_set<std::string> moved;
    for (const auto &clause : clauses) {
        if (missing_pk && clause.auto_increment && Has(primary_key_, clause.column)) {
            plan.primary_key.push_back(clause.sql);
            moved.insert(clause.column);
        } else
            plan.columns.push_back(clause.sql);
    }
    if (missing_pk)
        plan.primary_key.push_back(
            "ADD PRIMARY KEY (" + IndexColumns(primary_key_, true) + ")" +
            (tidb_nonclustered_pk_ ? " /*T![clustered_index] NONCLUSTERED */" : ""));
    for (const auto &sql : index_clauses) {
        bool depends = false;
        for (const auto &column : moved)
            if (sql.find(QuoteIdentifier(column)) != std::string::npos)
                depends = true;
        (depends ? plan.primary_key : plan.columns).push_back(sql);
    }
    return plan;
}
StringVector MessageTable::BuildIndexAlterClauses() const {
    if (!indexes_known_)
        return {};
    StringVector clauses;
    const auto matches = MatchExistingColumns();
    std::unordered_map<std::string, std::string> renamed;
    for (const auto &column : matches)
        if (column.meta)
            renamed[NameKey(column.online_name)] = std::string(column.field->name());
    auto check = [&](const std::string &name, const StringVector &fields, bool unique,
                     bool primary) {
        auto found = existing_indexes_.find(NameKey(name));
        size_t total = 0;
        for (const auto &field_name : fields) {
            auto *field = descriptor_->FindFieldByName(field_name);
            auto match = std::find_if(matches.begin(), matches.end(),
                                      [&](const auto &column) { return column.field == field; });
            total += IndexBytes(AlignedColumnType(*match), NeedsPrefix(ColumnType(field))
                                                               ? static_cast<int>(kDefaultKeyLength)
                                                               : 0);
        }
        if (total > kMaxIndexBytes)
            throw SchemaError(SchemaErrorCode::SchemaDrift, "preserved online widths exceed " +
                                                                std::to_string(kMaxIndexBytes) +
                                                                "-byte index budget: " + name);
        if (found == existing_indexes_.end()) {
            if (!primary)
                clauses.push_back(std::string("ADD ") + (unique ? "UNIQUE KEY " : "KEY ") +
                                  QuoteIdentifier(name) + " (" + IndexColumns(fields, true) + ")");
            return;
        }
        const auto &index = found->second;
        auto columns = OrderedColumns(index);
        if (index.unique != unique || columns.size() != fields.size() ||
            (index.type != "btree" && index.type != "hash" && index.type != ""))
            throw SchemaError(SchemaErrorCode::SchemaDrift, "index definition mismatch: " + name);
        for (size_t i = 0; i < fields.size(); ++i) {
            auto actual = columns[i].name;
            auto map = renamed.find(NameKey(actual));
            if (map != renamed.end())
                actual = map->second;
            auto field = descriptor_->FindFieldByName(fields[i]);
            int prefix = NeedsPrefix(ColumnType(field)) ? static_cast<int>(kDefaultKeyLength) : 0;
            if (columns[i].sequence != static_cast<int>(i + 1) ||
                NameKey(actual) != NameKey(fields[i]) || columns[i].prefix != prefix ||
                columns[i].descending)
                throw SchemaError(SchemaErrorCode::SchemaDrift,
                                  "index columns, prefix, or direction mismatch: " + name);
        }
    };
    if (!primary_key_.empty())
        check("PRIMARY", primary_key_, true, true);
    if (!unique_keys_.empty())
        check(SafeGeneratedName("uk_" + table_name_), Split(unique_keys_), true, false);
    for (size_t i = 0; i < indexes_.size(); ++i)
        check(SafeGeneratedName("idx_" + table_name_ + "_" + std::to_string(i)), Split(indexes_[i]),
              false, false);
    return clauses;
}
StringVector MessageTable::BuildMigrationStatements() const {
    return PlanSchemaAlignment().Statements(table_name_);
}
StringVector MessageTable::BuildAlterClauses() const {
    auto plan = PlanSchemaAlignment();
    plan.columns.insert(plan.columns.end(), plan.primary_key.begin(), plan.primary_key.end());
    return plan.columns;
}
std::string MessageTable::AlterTableSql() const {
    auto statements = BuildMigrationStatements();
    return statements.empty() ? "" : Join(statements, ";\n") + ";";
}

MigrationReport MessageTable::LegacyMigrationReport() const {
    MigrationReport report;
    const auto matches = MatchExistingColumns();
    std::unordered_set<std::string> legacy_names;
    std::map<std::string, std::string> types;
    for (const auto &column : matches) {
        auto name = std::string(column.field->name());
        const auto reason = LegacyKeyReason(column);
        if (!reason.empty()) {
            legacy_names.insert(name);
            report.reasons.push_back("column " + column.online_name + " (" + column.meta->type +
                                     "): " + reason + "; target " + ColumnType(column.field));
            if (Has(primary_key_, name))
                report.shadow_rebuild = true;
        }
        types[name] = reason.empty() ? AlignedColumnType(column) : ColumnType(column.field);
    }
    if (report.reasons.empty())
        return report;
    for (const auto &column : matches) {
        const auto name = std::string(column.field->name());
        if (!column.meta || legacy_names.count(name))
            continue;
        const auto target = ColumnType(column.field);
        const bool sign_change =
            HasIntegerType(column.meta->type) &&
            ((Lower(column.meta->type).find("unsigned") != std::string::npos) !=
             (Lower(target).find("unsigned") != std::string::npos));
        if (!SameFamily(column.meta->type, target) || sign_change)
            throw SchemaError(SchemaErrorCode::UnsafeSchemaConversion,
                              "manual key migration cannot safely convert unrelated column " +
                                  name + ": " + column.meta->type + " -> " + target);
        if (column.meta->metadata_complete &&
            (column.meta->nullable != (target.find("NOT NULL") == std::string::npos) ||
             !DefaultsEqual(column.meta->default_value, ExpectedDefault(target)) ||
             (column.meta->extra.find("auto_increment") != std::string::npos) !=
                 (name == auto_increment_key_)))
            report.warnings.push_back(
                "Unrelated column attributes also drift; resolve before copying: " + name +
                " target " + target);
    }
    const auto table = QuoteIdentifier(table_name_);
    const auto qualified = [&](const std::string &name) {
        return db_name_.empty() ? QuoteIdentifier(name)
                                : QuoteIdentifier(db_name_) + "." + QuoteIdentifier(name);
    };
    auto check_values = [&](const ColumnMatch &column) {
        auto field = column.field;
        auto name = std::string(field->name());
        auto online = QuoteIdentifier(column.online_name);
        auto type = ColumnType(field);
        auto limit = TypeLength(type);
        const bool bytes = field->type() == Field::TYPE_BYTES;
        report.checks.push_back(
            "SELECT MAX(" + std::string(bytes ? "LENGTH" : "CHAR_LENGTH") + "(" + online +
            ")) AS " + (bytes ? "max_bytes" : "max_chars") + ", SUM(" + online +
            " IS NULL) AS null_rows, COUNT(*) AS rows_total FROM " + qualified(table_name_) +
            " /* target " + QuoteIdentifier(name) + " limit " + std::to_string(limit) + " */");
    };
    for (const auto &column : matches)
        if (legacy_names.count(std::string(column.field->name())))
            check_values(column);
    report.warnings.push_back(
        "Stop writes for the entire migration. Resolve overlong values and NULL/empty-key "
        "collisions before executing. STRICT_ALL_TABLES prevents silent truncation.");
    report.warnings.push_back(
        "Review SHOW CREATE TABLE for CHECK constraints, partitions, descending/invisible index "
        "attributes and foreign keys; this report never executes SQL automatically.");
    report.checks.push_back(SelectForeignKeysSql());
    report.checks.push_back(SelectTriggersSql());
    report.checks.push_back("SHOW CREATE TABLE " + qualified(table_name_));
    for (const auto &fk : foreign_keys_)
        report.warnings.push_back("Foreign key requires manual recreation/repointing: " + fk);
    for (const auto &trigger : triggers_)
        report.warnings.push_back(
            "Trigger stays on the old table and requires recreation after RENAME: " + trigger);
    if (!dependencies_known_)
        report.warnings.push_back("Foreign-key and trigger names have not been loaded; run the "
                                  "listed metadata checks before proceeding.");
    std::unordered_set<std::string> declared;
    declared.insert("primary");
    if (!unique_keys_.empty())
        declared.insert(NameKey(SafeGeneratedName("uk_" + table_name_)));
    for (size_t i = 0; i < indexes_.size(); ++i)
        declared.insert(NameKey(SafeGeneratedName("idx_" + table_name_ + "_" + std::to_string(i))));
    report.statements.push_back(StrictModeSQL);
    if (!report.shadow_rebuild) {
        StringVector rebuilds;
        auto declared_index = [&](const std::string &name, const StringVector &fields,
                                  bool unique) {
            bool touches = false;
            for (const auto &field : fields)
                if (legacy_names.count(field))
                    touches = true;
            if (!touches)
                return;
            if (indexes_known_ && existing_indexes_.count(NameKey(name)))
                report.statements.push_back("ALTER TABLE " + qualified(table_name_) +
                                            " DROP INDEX " + QuoteIdentifier(name));
            rebuilds.push_back("ALTER TABLE " + qualified(table_name_) +
                               std::string(unique ? " ADD UNIQUE KEY " : " ADD KEY ") +
                               QuoteIdentifier(name) + " (" + IndexColumns(fields, true) + ")");
        };
        if (!unique_keys_.empty())
            declared_index(SafeGeneratedName("uk_" + table_name_), Split(unique_keys_), true);
        for (size_t i = 0; i < indexes_.size(); ++i)
            declared_index(SafeGeneratedName("idx_" + table_name_ + "_" + std::to_string(i)),
                           Split(indexes_[i]), false);
        for (const auto &key : SortedNames(existing_indexes_)) {
            if (declared.count(key))
                continue;
            const auto &index = existing_indexes_.at(key);
            for (const auto &part : index.columns)
                for (const auto &column : matches)
                    if (column.meta && NameKey(part.name) == NameKey(column.online_name) &&
                        legacy_names.count(std::string(column.field->name())))
                        report.warnings.push_back("Extra online index must be manually dropped "
                                                  "before MODIFY and rebuilt afterward: " +
                                                  index.name);
        }
        for (const auto &column : matches)
            if (legacy_names.count(std::string(column.field->name())) && column.meta->nullable)
                report.statements.push_back("UPDATE " + qualified(table_name_) + " SET " +
                                            QuoteIdentifier(column.online_name) + " = '' WHERE " +
                                            QuoteIdentifier(column.online_name) + " IS NULL");
        for (const auto &column : matches)
            if (legacy_names.count(std::string(column.field->name()))) {
                auto name = std::string(column.field->name());
                const bool rename = column.online_name != name;
                report.statements.push_back(
                    "ALTER TABLE " + qualified(table_name_) +
                    (rename ? " CHANGE COLUMN " + QuoteIdentifier(column.online_name) + " "
                            : " MODIFY COLUMN ") +
                    QuoteIdentifier(name) + " " + ColumnType(column.field) +
                    " COMMENT 'pb:" + std::to_string(column.field->number()) + "'");
            }
        report.statements.insert(report.statements.end(), rebuilds.begin(), rebuilds.end());
        if (!indexes_known_)
            report.warnings.push_back("Secondary-index metadata is unavailable: manually DROP "
                                      "every affected index and rebuild it after MODIFY.");
        return report;
    }
    const auto shadow_name = SafeGeneratedName(table_name_ + "__p2m_new"),
               backup_name = SafeGeneratedName(table_name_ + "__p2m_old");
    report.warnings.push_back("RENAME redirects incoming foreign keys and retains triggers on " +
                              backup_name +
                              ". Recreate these objects explicitly. Retain the backup until all "
                              "data is verified; no DROP TABLE is generated.");
    auto budget = [&](const StringVector &fields) {
        size_t total = 0;
        for (const auto &name : fields) {
            auto *field = descriptor_->FindFieldByName(name);
            total += IndexBytes(types.at(name), NeedsPrefix(ColumnType(field))
                                                    ? static_cast<int>(kDefaultKeyLength)
                                                    : 0);
        }
        return total;
    };
    std::vector<StringVector> keys;
    if (!primary_key_.empty())
        keys.push_back(primary_key_);
    if (!unique_keys_.empty())
        keys.push_back(Split(unique_keys_));
    for (const auto &index : indexes_)
        keys.push_back(Split(index));
    std::set<std::string> shrunk;
    for (const auto &key : keys)
        for (const auto &name : key) {
            if (budget(key) <= kMaxIndexBytes)
                break;
            auto *field = descriptor_->FindFieldByName(name);
            if (identity_columns_.count(name) && types[name] != ColumnType(field)) {
                types[name] = ColumnType(field);
                shrunk.insert(name);
            }
        }
    for (const auto &name : shrunk) {
        report.warnings.push_back("Online key width is reduced to the proto declaration to satisfy "
                                  "the " +
                                  std::to_string(kMaxIndexBytes) + "-byte index budget: " + name +
                                  " " + types[name]);
        for (const auto &column : matches)
            if (std::string(column.field->name()) == name && column.meta &&
                !legacy_names.count(name))
                check_values(column);
    }
    std::string create = CreateTableSql();
    ReplaceOnce(create, "CREATE TABLE IF NOT EXISTS " + table,
                "CREATE TABLE " + qualified(shadow_name));
    std::unordered_map<std::string, std::string> field_by_online;
    StringVector targets, sources;
    std::unordered_set<std::string> claimed;
    for (const auto &column : matches) {
        auto name = std::string(column.field->name());
        const auto comment = " COMMENT 'pb:" + std::to_string(column.field->number()) + "'";
        if (types[name] != ColumnType(column.field)) {
            ReplaceOnce(create, QuoteIdentifier(name) + " " + ColumnType(column.field) + comment,
                        QuoteIdentifier(name) + " " + types[name] + comment);
            report.warnings.push_back("Preserved online column width/collation: " + name + " " +
                                      types[name]);
        }
        if (!column.meta)
            continue;
        claimed.insert(column.online_name);
        field_by_online[NameKey(column.online_name)] = name;
        targets.push_back(QuoteIdentifier(name));
        auto source = QuoteIdentifier(column.online_name);
        if (legacy_names.count(name) && column.meta->nullable)
            source = "COALESCE(" + source + ", '')";
        else if (column.meta->nullable && types[name].find("NOT NULL") != std::string::npos) {
            report.warnings.push_back(
                "Backfill NULL values manually before copying into NOT NULL column: " +
                column.online_name + " " + types[name]);
            report.checks.push_back("SELECT SUM(" + source + " IS NULL) AS null_rows FROM " +
                                    qualified(table_name_));
        }
        sources.push_back(source);
    }
    for (const auto &name : SortedNames(existing_columns_))
        if (!claimed.count(name))
            report.warnings.push_back(
                "Orphan online column is absent from the shadow table; add it to CREATE and INSERT "
                "lists before execution or its data remains only in the backup: " +
                name);
    StringVector extra_indexes;
    for (const auto &key : SortedNames(existing_indexes_)) {
        if (declared.count(key))
            continue;
        const auto &index = existing_indexes_.at(key);
        std::string reason;
        if (index.type != "btree" && index.type != "hash" && index.type != "")
            reason = "unsupported type " + index.type;
        StringVector parts;
        size_t total = 0;
        for (const auto &part : OrderedColumns(index)) {
            auto match = field_by_online.find(NameKey(part.name));
            if (match == field_by_online.end()) {
                reason = "expression or column absent from shadow: " + part.name;
                break;
            }
            const auto &name = match->second;
            int prefix = part.prefix;
            if (legacy_names.count(name))
                prefix = 0;
            else if (!prefix && NeedsPrefix(types[name]))
                prefix = static_cast<int>(kDefaultKeyLength);
            if (prefix && TypeLength(types[name]) > 0 && prefix >= TypeLength(types[name]) &&
                !NeedsPrefix(types[name]))
                prefix = 0;
            auto sql = QuoteIdentifier(name);
            if (prefix)
                sql += "(" + std::to_string(prefix) + ")";
            if (part.descending)
                sql += " DESC";
            parts.push_back(sql);
            total += IndexBytes(types[name], prefix);
        }
        if (total > kMaxIndexBytes)
            reason = "preserved definition exceeds " + std::to_string(kMaxIndexBytes) +
                     "-byte index budget";
        if (!reason.empty()) {
            report.warnings.push_back("Cannot automatically reproduce extra index " + index.name +
                                      ": " + reason + "; recreate it manually before RENAME");
            continue;
        }
        extra_indexes.push_back(std::string(index.unique ? "UNIQUE KEY " : "KEY ") +
                                QuoteIdentifier(index.name) + " (" + Join(parts) + ")");
        report.warnings.push_back("Preserved extra online index: " + index.name);
    }
    if (!extra_indexes.empty()) {
        auto pos = create.find(") ENGINE=");
        if (pos == std::string::npos)
            throw std::logic_error("shadow CREATE template mismatch");
        create.insert(pos, ", " + Join(extra_indexes));
    }
    if (!create.empty() && create.back() == ';')
        create.pop_back();
    report.statements.push_back(create);
    report.statements.push_back("INSERT INTO " + qualified(shadow_name) + " (" + Join(targets) +
                                ") SELECT " + Join(sources) + " FROM " + qualified(table_name_));
    if (!auto_increment_key_.empty()) {
        report.statements.push_back(kMySql8FreshMetadataSql);
        report.statements.push_back(
            "SET @p2m_auto_increment = COALESCE((SELECT AUTO_INCREMENT FROM "
            "INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA=" +
            (db_name_.empty()
                 ? std::string("DATABASE()")
                 : (db_name_.empty() ? std::string("DATABASE()") : HexLiteral(db_name_))) +
            " AND TABLE_NAME=" + HexLiteral(table_name_) + "), 1)");
        if (online_auto_increment_)
            report.warnings.push_back("Observed AUTO_INCREMENT high-water mark: " +
                                      std::to_string(*online_auto_increment_) +
                                      "; SQL rereads it immediately before rebasing the shadow.");
        report.statements.push_back(
            "SET @p2m_rebase_sql = CONCAT(" +
            HexLiteral("ALTER TABLE " + qualified(shadow_name) + " AUTO_INCREMENT = ") +
            ", @p2m_auto_increment)");
        report.statements.push_back("PREPARE p2m_rebase_auto_increment FROM @p2m_rebase_sql");
        report.statements.push_back("EXECUTE p2m_rebase_auto_increment");
        report.statements.push_back("DEALLOCATE PREPARE p2m_rebase_auto_increment");
    }
    report.statements.push_back("RENAME TABLE " + qualified(table_name_) + " TO " +
                                qualified(backup_name) + ", " + qualified(shadow_name) + " TO " +
                                qualified(table_name_));
    if (!indexes_known_)
        report.warnings.push_back("Online secondary indexes were not loaded; add every required "
                                  "extra index to the shadow CREATE before execution.");
    return report;
}
} // namespace pbmysql
