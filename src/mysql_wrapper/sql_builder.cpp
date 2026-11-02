#include "src/mysql_wrapper/sql_builder.h"
#include "src/mysql_wrapper/database_limits.h"
#include <string_view>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace pbmysql {
namespace {
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
std::string Q(const std::string &name) {
    std::string result = "`";
    for (char c : name) {
        result += c;
        if (c == '`')
            result += '`';
    }
    return result + '`';
}
std::string Join(const StringVector &parts, const std::string &delimiter) {
    std::string result;
    for (const auto &part : parts) {
        if (!result.empty())
            result += delimiter;
        result += part;
    }
    return result;
}
std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Where(std::string value, bool required = false) {
    value = Trim(value);
    constexpr std::string_view kWhereKeyword = "where";
    if (value.size() >= kWhereKeyword.size()) {
        std::string prefix = value.substr(0, kWhereKeyword.size());
        std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::string_view(prefix) == kWhereKeyword &&
            (value.size() == kWhereKeyword.size() ||
             std::isspace(static_cast<unsigned char>(value[kWhereKeyword.size()]))))
            value = Trim(value.substr(kWhereKeyword.size()));
    }
    if (required && value.empty())
        throw std::invalid_argument("empty WHERE clause");
    return value;
}
std::string Guard(const std::string &where, const std::string &guard) {
    const std::string normalized = Where(guard);
    return normalized.empty() ? where : "(" + where + ") AND (" + normalized + ")";
}
void Append(std::vector<SqlValue> &values, const std::vector<SqlValue> &tail) {
    values.insert(values.end(), tail.begin(), tail.end());
}
std::string Suffix(const QueryOptions &options) {
    std::string suffix;
    if (!options.order_by.empty())
        suffix += " ORDER BY " + options.order_by;
    if (options.limit > 0) {
        suffix += " LIMIT " + std::to_string(options.limit);
        if (options.offset > 0)
            suffix += " OFFSET " + std::to_string(options.offset);
    }
    if (options.for_update)
        suffix += " FOR UPDATE";
    return suffix;
}
bool Has(const StringVector &columns, const std::string &column) {
    return std::find(columns.begin(), columns.end(), column) != columns.end();
}
std::vector<Assign> NewAssigns(const StringVector &columns, bool add) {
    std::vector<Assign> assigns;
    for (const auto &column : columns)
        assigns.push_back(add ? AddNew(column) : SetNew(column));
    return assigns;
}
void ValidateNumericValue(const SqlValue &value) {
    if (std::holds_alternative<std::nullptr_t>(value))
        throw std::invalid_argument("NULL numeric operand");
    if (const auto *v = std::get_if<double>(&value)) {
        if (!std::isfinite(*v))
            throw std::invalid_argument("non-finite numeric operand");
    } else if (const auto *text = std::get_if<std::string>(&value)) {
        double parsed{};
        const auto result = std::from_chars(text->data(), text->data() + text->size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text->data() + text->size() ||
            !std::isfinite(parsed))
            throw std::invalid_argument("invalid numeric operand");
    }
}
} // namespace

std::string NormalizeWhereClause(const std::string &where) {
    const auto clause = Where(where);
    return clause.empty() ? "1=1" : clause;
}

Assign SetCol(std::string c, SqlValue v) {
    return {c, Q(c) + " = ?", {std::move(v)}, false, false};
}
Assign AddCol(std::string c, SqlValue v) {
    return {c, Q(c) + " = " + Q(c) + " + ?", {std::move(v)}, true, false};
}
Assign SubCol(std::string c, SqlValue v) {
    return {c, Q(c) + " = " + Q(c) + " - ?", {std::move(v)}, true, false};
}
Assign SetColExpr(std::string c, std::string expr, std::vector<SqlValue> args) {
    if (Trim(expr).empty())
        throw std::invalid_argument("empty assignment expression");
    return {c, Q(c) + " = " + expr, std::move(args), false, false};
}
Assign SetNew(std::string c) { return {c, Q(c) + " = VALUES(" + Q(c) + ")", {}, false, true}; }
Assign AddNew(std::string c) {
    return {c, Q(c) + " = " + Q(c) + " + VALUES(" + Q(c) + ")", {}, true, true};
}
Assign MinNew(std::string c) {
    return {c, Q(c) + " = LEAST(" + Q(c) + ", VALUES(" + Q(c) + "))", {}, true, true};
}
Assign MaxNew(std::string c) {
    return {c, Q(c) + " = GREATEST(" + Q(c) + ", VALUES(" + Q(c) + "))", {}, true, true};
}
Assign SetNewIfZero(std::string c) {
    return {
        c, Q(c) + " = IF(" + Q(c) + " = 0, VALUES(" + Q(c) + "), " + Q(c) + ")", {}, true, true};
}
Assign KeepOld(std::string c) { return {c, Q(c) + " = " + Q(c), {}, false, false}; }

const FieldDescriptor *SQLBuilder::Field(const std::string &column) const {
    const auto *field = table_.Descriptor()->FindFieldByName(column);
    if (!field)
        throw std::invalid_argument("unknown field: " + column);
    return field;
}
StringVector SQLBuilder::Columns(bool exclude_primary) const {
    StringVector columns;
    for (int i = 0; i < table_.Descriptor()->field_count(); ++i) {
        const std::string name(table_.Descriptor()->field(i)->name());
        if (!exclude_primary || !Has(table_.PrimaryKeys(), name))
            columns.push_back(name);
    }
    return columns;
}
StringVector SQLBuilder::SetFields(const Message &message, bool exclude_primary) const {
    if (message.GetDescriptor() != table_.Descriptor())
        throw std::invalid_argument("message descriptor mismatch");
    std::vector<const FieldDescriptor *> fields;
    message.GetReflection()->ListFields(message, &fields);
    std::unordered_set<const FieldDescriptor *> set(fields.begin(), fields.end());
    StringVector columns;
    for (const auto &column : Columns(exclude_primary))
        if (set.count(Field(column)))
            columns.push_back(column);
    if (columns.empty())
        throw std::invalid_argument("no fields set in message");
    return columns;
}
Statement SQLBuilder::PrimaryKeyWhere(const Message &message) const {
    table_.ValidatePrimaryKeyMessage(message);
    if (table_.PrimaryKeys().empty())
        throw std::invalid_argument("primary key not found");
    Statement statement;
    for (const auto &key : table_.PrimaryKeys()) {
        if (!statement.sql.empty())
            statement.sql += " AND ";
        statement.sql += Q(key) + " = ?";
        statement.args.push_back(FieldValue(message, Field(key)));
    }
    return statement;
}
Statement SQLBuilder::InsertColumns(const std::vector<const Message *> &messages,
                                    const StringVector &columns, bool replace) const {
    if (messages.empty())
        throw std::invalid_argument("empty message batch");
    if (messages.size() > kBatchSize)
        throw std::invalid_argument("batch size exceeds maximum " + std::to_string(kBatchSize));
    StringVector quoted;
    for (const auto &column : columns) {
        Field(column);
        quoted.push_back(Q(column));
    }
    Statement statement{std::string(replace ? "REPLACE INTO " : "INSERT INTO ") + Q(TableName()) +
                            " (" + Join(quoted, ", ") + ") VALUES ",
                        {}};
    StringVector placeholders(columns.size(), "?");
    const std::string group = "(" + Join(placeholders, ", ") + ")";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (!messages[i])
            throw std::invalid_argument("null message in batch");
        table_.ValidateMessage(*messages[i]);
        if (i)
            statement.sql += ", ";
        statement.sql += group;
        for (const auto &column : columns)
            statement.args.push_back(FieldValue(*messages[i], Field(column)));
    }
    return statement;
}
Statement SQLBuilder::Insert(const Message &message) const {
    return InsertColumns({&message}, Columns(), false);
}
Statement SQLBuilder::InsertSetFields(const Message &message) const {
    return InsertColumns({&message}, SetFields(message, false), false);
}
Statement SQLBuilder::Replace(const Message &message) const {
    return InsertColumns({&message}, Columns(), true);
}
Statement SQLBuilder::BatchInsert(const std::vector<const Message *> &messages) const {
    return InsertColumns(messages, Columns(), false);
}
Statement SQLBuilder::BatchReplace(const std::vector<const Message *> &messages) const {
    return InsertColumns(messages, Columns(), true);
}
Statement SQLBuilder::DuplicateNoop(Statement insert) const {
    auto columns = table_.PrimaryKeys();
    if (columns.empty())
        columns = Columns();
    if (columns.empty())
        throw std::invalid_argument("table has no columns");
    insert.sql += " ON DUPLICATE KEY UPDATE " + Q(columns.front()) + " = " + Q(columns.front());
    return insert;
}
Statement SQLBuilder::InsertIgnore(const Message &message) const {
    return DuplicateNoop(Insert(message));
}
Statement SQLBuilder::InsertIgnoreSetFields(const Message &message) const {
    return DuplicateNoop(InsertSetFields(message));
}
Statement SQLBuilder::BatchInsertIgnore(const std::vector<const Message *> &messages) const {
    return DuplicateNoop(BatchInsert(messages));
}
Statement SQLBuilder::BuildAssigns(const std::vector<Assign> &assigns, bool upsert) const {
    if (assigns.empty())
        throw std::invalid_argument("empty assignments");
    Statement result;
    for (const auto &assign : assigns) {
        const auto *field = Field(assign.column);
        if (assign.expression.empty())
            throw std::invalid_argument("invalid assignment: " + assign.column);
        if (assign.numeric && !IsNumericField(field))
            throw std::invalid_argument("numeric assignment requires numeric scalar: " +
                                        assign.column);
        if (assign.numeric)
            for (const auto &arg : assign.args)
                ValidateNumericValue(arg);
        if (assign.insert_only && !upsert)
            throw std::invalid_argument("VALUES assignment requires UPSERT");
        if (!result.sql.empty())
            result.sql += ", ";
        result.sql += assign.expression;
        if (!assign.numeric && !assign.insert_only &&
            assign.expression == Q(assign.column) + " = ?" && assign.args.size() == 1)
            result.args.push_back(ComparisonValue(assign.column, assign.args.front()));
        else {
            for (const auto &arg : assign.args)
                if (const auto *number = std::get_if<double>(&arg);
                    number && !std::isfinite(*number))
                    throw std::invalid_argument("non-finite assignment parameter");
            Append(result.args, assign.args);
        }
    }
    return result;
}
Statement SQLBuilder::AppendDuplicate(Statement insert, const std::vector<Assign> &assigns) const {
    auto set = BuildAssigns(assigns, true);
    insert.sql += " ON DUPLICATE KEY UPDATE " + set.sql;
    Append(insert.args, set.args);
    return insert;
}
Statement SQLBuilder::Upsert(const Message &message, const StringVector &columns) const {
    return UpsertWith(message, NewAssigns(columns.empty() ? Columns(true) : columns, false));
}
Statement SQLBuilder::UpsertAdd(const Message &message, const StringVector &columns) const {
    if (columns.empty())
        throw std::invalid_argument("UpsertAdd requires explicit numeric columns");
    return UpsertWith(message, NewAssigns(columns, true));
}
Statement SQLBuilder::UpsertKeepOld(const Message &message) const {
    if (table_.PrimaryKeys().empty())
        throw std::invalid_argument("primary key not found");
    return UpsertWith(message, {KeepOld(table_.PrimaryKeys().front())});
}
std::vector<Assign> SQLBuilder::IdentityAssigns(const Message *partial) const {
    if (table_.PrimaryKeys().empty())
        throw std::invalid_argument("primary key not found");
    StringVector predicates;
    for (const auto &key : table_.PrimaryKeys())
        predicates.push_back(Q(key) + " <=> VALUES(" + Q(key) + ")");
    const auto identity = Join(predicates, " AND ");
    std::unordered_set<const FieldDescriptor *> populated;
    if (partial) {
        table_.ValidateMessage(*partial);
        std::vector<const FieldDescriptor *> fields;
        partial->GetReflection()->ListFields(*partial, &fields);
        populated.insert(fields.begin(), fields.end());
    }
    std::vector<Assign> assignments;
    for (const auto &column : Columns(true)) {
        if (partial && !populated.count(Field(column)))
            continue;
        if (partial)
            assignments.push_back(SetColExpr(column, "IF(" + identity + ", ?, " + Q(column) + ")",
                                             {FieldValue(*partial, Field(column))}));
        else
            assignments.push_back(SetColExpr(column, "IF(" + identity + ", VALUES(" + Q(column) +
                                                         "), " + Q(column) + ")"));
    }
    if (assignments.empty())
        assignments.push_back(KeepOld(table_.PrimaryKeys().front()));
    return assignments;
}
Statement SQLBuilder::GetSaveSQLWithArgs(const Message &message) const {
    return UpsertWith(message, IdentityAssigns(nullptr));
}
Statement SQLBuilder::GetBatchSaveSQLWithArgs(const std::vector<const Message *> &messages) const {
    return BatchUpsertWith(messages, IdentityAssigns(nullptr));
}
Statement SQLBuilder::GetInsertOnDupUpdateSQLWithArgs(const Message &message) const {
    return UpsertWith(message, IdentityAssigns(&message));
}
Statement SQLBuilder::GetInsertOnDupKeyForPrimaryKeyWithArgs(const Message &message) const {
    return UpsertKeepOld(message);
}
Statement SQLBuilder::UpsertWith(const Message &message, const std::vector<Assign> &assigns) const {
    return AppendDuplicate(Insert(message), assigns);
}
Statement SQLBuilder::BatchUpsert(const std::vector<const Message *> &messages,
                                  const StringVector &columns) const {
    return BatchUpsertWith(messages, NewAssigns(columns.empty() ? Columns(true) : columns, false));
}
Statement SQLBuilder::BatchUpsertWith(const std::vector<const Message *> &messages,
                                      const std::vector<Assign> &assigns) const {
    return AppendDuplicate(BatchInsert(messages), assigns);
}

Statement SQLBuilder::SelectColumns(const StringVector &columns, const std::string &where,
                                    const std::vector<SqlValue> &args,
                                    const QueryOptions &options) const {
    if (columns.empty())
        return SelectWhere(where, args, options);
    StringVector quoted;
    for (const auto &column : columns) {
        Field(column);
        quoted.push_back(Q(column));
    }
    Statement statement{"SELECT " + Join(quoted, ", ") + " FROM " + Q(TableName()), args};
    const auto clause = Where(where);
    statement.sql += " WHERE " + (clause.empty() ? "1=1" : clause);
    statement.sql += Suffix(options);
    return statement;
}
Statement SQLBuilder::SelectWhere(const std::string &where, const std::vector<SqlValue> &args,
                                  const QueryOptions &options) const {
    if (table_.Descriptor()->field_count() == 0)
        throw std::invalid_argument("table has no fields");
    return SelectColumns(Columns(), where, args, options);
}
Statement SQLBuilder::SelectByPK(const Message &message) const {
    const auto pk = PrimaryKeyWhere(message);
    return SelectWhere(pk.sql, pk.args);
}
Statement SQLBuilder::SelectByPKForUpdate(const Message &message) const {
    auto statement = SelectByPK(message);
    statement.sql += " FOR UPDATE";
    return statement;
}
SqlValue SQLBuilder::ComparisonValue(const std::string &column, const SqlValue &value) const {
    const auto *field = Field(column);
    if (std::holds_alternative<std::nullptr_t>(value))
        return value;
    auto normalized = pbmysql::NormalizeComparisonValue(field, value);
    if (!field->is_repeated() && field->cpp_type() == FieldDescriptor::CPPTYPE_STRING) {
        if (const auto *text = std::get_if<std::string>(&value)) {
            std::unique_ptr<Message> temporary(table_.DefaultInstance().New());
            temporary->GetReflection()->SetString(temporary.get(), field, *text);
            table_.ValidateFieldValue(*temporary, field);
        }
    }
    return normalized;
}
Statement SQLBuilder::InWhere(const std::string &column,
                              const std::vector<SqlValue> &values) const {
    Field(column);
    if (values.empty())
        throw std::invalid_argument("empty IN value list");
    Statement result{Q(column) + " IN (" + Join(StringVector(values.size(), "?"), ", ") + ")", {}};
    for (const auto &value : values)
        result.args.push_back(ComparisonValue(column, value));
    return result;
}
std::string SQLBuilder::SinglePK() const {
    if (table_.PrimaryKeys().size() != 1)
        throw std::invalid_argument("IN requires a single-column primary key");
    return table_.PrimaryKeys().front();
}
Statement SQLBuilder::SelectByKVIn(const std::string &column, const std::vector<SqlValue> &values,
                                   const QueryOptions &options) const {
    const auto where = InWhere(column, values);
    return SelectWhere(where.sql, where.args, options);
}
Statement SQLBuilder::SelectByPKIn(const std::vector<SqlValue> &values,
                                   const QueryOptions &options) const {
    return SelectByKVIn(SinglePK(), values, options);
}
Statement SQLBuilder::Count(const std::string &where, const std::vector<SqlValue> &args) const {
    Statement statement{"SELECT COUNT(*) FROM " + Q(TableName()), args};
    const auto clause = Where(where);
    statement.sql += " WHERE " + (clause.empty() ? "1=1" : clause);
    return statement;
}
Statement SQLBuilder::Exists(const std::string &where, const std::vector<SqlValue> &args) const {
    Statement statement{"SELECT 1 FROM " + Q(TableName()), args};
    const auto clause = Where(where);
    statement.sql += " WHERE " + (clause.empty() ? "1=1" : clause);
    statement.sql += " LIMIT 1";
    return statement;
}
Statement SQLBuilder::ExistsByPKForUpdate(const Message &message) const {
    const auto pk = PrimaryKeyWhere(message);
    auto statement = Exists(pk.sql, pk.args);
    statement.sql += " FOR UPDATE";
    return statement;
}

Statement SQLBuilder::UpdateAssignsWhere(const std::vector<Assign> &assigns,
                                         const std::string &where,
                                         const std::vector<SqlValue> &args) const {
    const auto clause = Where(where, true);
    auto set = BuildAssigns(assigns, false);
    set.sql = "UPDATE " + Q(TableName()) + " SET " + set.sql + " WHERE " + clause;
    Append(set.args, args);
    return set;
}
Statement SQLBuilder::UpdateAssignsByPK(const Message &message,
                                        const std::vector<Assign> &assigns) const {
    const auto pk = PrimaryKeyWhere(message);
    return UpdateAssignsWhere(assigns, pk.sql, pk.args);
}
Statement SQLBuilder::UpdateFieldsByPK(const Message &message, const StringVector &columns) const {
    if (message.GetDescriptor() != table_.Descriptor())
        throw std::invalid_argument("message descriptor mismatch");
    std::vector<Assign> assigns;
    for (const auto &column : columns) {
        table_.ValidateFieldValue(message, Field(column));
        assigns.push_back(SetCol(column, FieldValue(message, Field(column))));
    }
    return UpdateAssignsByPK(message, assigns);
}
Statement SQLBuilder::UpdateByPK(const Message &message) const {
    return UpdateFieldsByPK(message, SetFields(message, false));
}
Statement SQLBuilder::UpdateByPKIf(const Message &message, const std::string &guard,
                                   const std::vector<SqlValue> &args) const {
    const auto pk = PrimaryKeyWhere(message);
    auto where_args = pk.args;
    Append(where_args, args);
    std::vector<Assign> assigns;
    for (const auto &column : SetFields(message, false))
        assigns.push_back(SetCol(column, FieldValue(message, Field(column))));
    return UpdateAssignsWhere(assigns, Guard(pk.sql, guard), where_args);
}
Statement SQLBuilder::UpdateWhere(const Message &message, const std::string &where,
                                  const std::vector<SqlValue> &args) const {
    std::vector<Assign> assigns;
    for (const auto &column : SetFields(message, false))
        assigns.push_back(SetCol(column, FieldValue(message, Field(column))));
    return UpdateAssignsWhere(assigns, where, args);
}
Statement SQLBuilder::IncrByPK(const Message &message, const std::string &column,
                               int64_t delta) const {
    return UpdateAssignsByPK(message, {AddCol(column, delta)});
}
Statement SQLBuilder::DecrByPKIfEnough(const Message &message, const std::string &column,
                                       int64_t delta) const {
    if (delta <= 0)
        throw std::invalid_argument("decrement delta must be positive");
    auto pk = PrimaryKeyWhere(message);
    pk.sql += " AND " + Q(column) + " >= ?";
    pk.args.push_back(delta);
    return UpdateAssignsWhere({SubCol(column, delta)}, pk.sql, pk.args);
}
Statement SQLBuilder::DeleteWhere(const std::string &where,
                                  const std::vector<SqlValue> &args) const {
    return {"DELETE FROM " + Q(TableName()) + " WHERE " + Where(where, true), args};
}
Statement SQLBuilder::DeleteByPK(const Message &message) const {
    const auto pk = PrimaryKeyWhere(message);
    return DeleteWhere(pk.sql, pk.args);
}
Statement SQLBuilder::DeleteByPKIf(const Message &message, const std::string &guard,
                                   const std::vector<SqlValue> &args) const {
    auto pk = PrimaryKeyWhere(message);
    Append(pk.args, args);
    return DeleteWhere(Guard(pk.sql, guard), pk.args);
}
Statement SQLBuilder::DeleteWhereLimit(const std::string &where, const std::vector<SqlValue> &args,
                                       const std::string &order_by, int limit) const {
    if (limit <= 0)
        throw std::invalid_argument("delete limit must be positive");
    auto statement = DeleteWhere(where, args);
    if (!order_by.empty())
        statement.sql += " ORDER BY " + order_by;
    statement.sql += " LIMIT " + std::to_string(limit);
    return statement;
}
Statement SQLBuilder::DeleteByKVIn(const std::string &column,
                                   const std::vector<SqlValue> &values) const {
    const auto where = InWhere(column, values);
    return DeleteWhere(where.sql, where.args);
}
Statement SQLBuilder::DeleteByPKIn(const std::vector<SqlValue> &values) const {
    return DeleteByKVIn(SinglePK(), values);
}
} // namespace pbmysql
