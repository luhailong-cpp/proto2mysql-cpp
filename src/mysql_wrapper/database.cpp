#include "src/mysql_wrapper/database.h"
#include "src/mysql_wrapper/database_limits.h"
#include <mysqld_error.h>

#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/cache_codec.h"
#include <algorithm>
#include <exception>
#include <fstream>
#include <limits>
#include <set>
#include <unordered_set>

namespace pbmysql {
namespace {
using Message = google::protobuf::Message;
using Field = google::protobuf::FieldDescriptor;
inline constexpr std::int64_t kVersionIncrement = 1;

StringVector NonPKColumns(const MessageTable &table) {
    StringVector result;
    for (int i = 0; i < table.Descriptor()->field_count(); ++i) {
        const std::string name(table.Descriptor()->field(i)->name());
        if (std::find(table.PrimaryKeys().begin(), table.PrimaryKeys().end(), name) ==
            table.PrimaryKeys().end())
            result.push_back(name);
    }
    return result;
}
bool IsPK(const MessageTable &table, const std::string &name) {
    return std::find(table.PrimaryKeys().begin(), table.PrimaryKeys().end(), name) !=
           table.PrimaryKeys().end();
}
Statement MatchRow(const MessageTable &table, const Message &message, const StringVector &columns,
                   const std::vector<SqlValue> &written) {
    auto where = SQLBuilder(table).PrimaryKeyWhere(message);
    std::string sql = "SELECT 1 FROM " + QuoteIdentifier(table.TableName()) + " WHERE " + where.sql;
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto &name = columns[i];
        const auto *field = table.Descriptor()->FindFieldByName(name);
        // Compare exactly the bytes bound to UPDATE; reserializing a protobuf
        // map can otherwise produce a different field ordering. FLOAT still
        // needs its stored float32 value promoted for SQL comparison.
        auto value = field->type() == Field::TYPE_FLOAT && !field->is_repeated()
                         ? FieldComparisonValue(message, field)
                         : written.at(i);
        sql += " AND ";
        // Bytewise comparison avoids case-insensitive/PAD SPACE equality when
        // checking whether a full-row save really reached its target values.
        if (field->is_repeated() || field->cpp_type() == Field::CPPTYPE_STRING ||
            (field->cpp_type() == Field::CPPTYPE_MESSAGE &&
             field->message_type()->full_name() != "google.protobuf.Timestamp"))
            sql += "BINARY " + QuoteIdentifier(name) + " <=> BINARY ?";
        else
            sql += QuoteIdentifier(name) + " <=> ?";
        where.args.push_back(std::move(value));
    }
    return {sql + " LIMIT 1 FOR UPDATE", std::move(where.args)};
}
std::string RowCell(const ResultRow &row, size_t index) {
    if (index >= row.size() || !row[index])
        throw std::runtime_error("unexpected NULL or missing result column");
    return std::string(row[index], row.length(index));
}
} // namespace

void Database::Insert(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).Insert(message));
    InvalidateMessage(table, message);
}
bool Database::InsertIgnore(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    try {
        Insert(message);
        return true;
    } catch (const MysqlError &error) {
        if (error.value() == ER_DUP_ENTRY)
            return false;
        throw;
    }
}
uint64_t Database::InsertReturningId(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    Insert(message);
    return LastInsertId();
}
void Database::InsertOnDupUpdate(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    Save(message);
}
void Database::BatchInsert(const std::vector<const Message *> &messages) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (messages.empty())
        return;
    // Validate the entire batch before the first write, including mixed types.
    auto *table = messages.front() ? TableFor(*messages.front()) : nullptr;
    if (!table)
        throw std::invalid_argument("null batch message");
    for (auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null batch message");
        table->ValidateMessage(*message);
        SQLBuilder(*table).Insert(*message);
    }
    for (size_t start = 0; start < messages.size(); start += kBatchSize) {
        const auto end = std::min(start + kBatchSize, messages.size());
        std::vector<const Message *> chunk(messages.begin() + start, messages.begin() + end);
        Execute(SQLBuilder(*table).BatchInsert(chunk));
        for (auto *message : chunk)
            InvalidateMessage(table, *message);
    }
}
void Database::BatchSave(const std::vector<const Message *> &messages) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (messages.empty())
        return;
    if (!messages.front())
        throw std::invalid_argument("null batch message");
    auto *table = TableFor(*messages.front());
    for (const auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null batch message");
        table->ValidateMessage(*message);
        SQLBuilder(*table).PrimaryKeyWhere(*message);
        SQLBuilder(*table).Insert(*message);
    }
    for (auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null batch message");
        Save(*message);
    }
}
void Database::Save(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    const SQLBuilder builder(*table);
    const auto columns = NonPKColumns(*table);
    auto pk = builder.PrimaryKeyWhere(message);
    const auto update =
        columns.empty()
            ? Statement{"UPDATE " + QuoteIdentifier(table->TableName()) + " SET " +
                            QuoteIdentifier(table->PrimaryKeys().front()) + " = " +
                            QuoteIdentifier(table->PrimaryKeys().front()) + " WHERE " + pk.sql,
                        pk.args}
            : builder.UpdateFieldsByPK(message, columns);
    const auto insert = builder.Insert(message);
    Execute(update);
    if (AffectedRows() > 0) {
        InvalidateMessage(table, message);
        return;
    }
    try {
        Execute(insert);
        InvalidateMessage(table, message);
        return;
    } catch (const MysqlError &error) {
        if (error.value() != ER_DUP_ENTRY)
            throw;
        const auto duplicate = std::current_exception();
        Execute(update);
        if (AffectedRows() > 0 || QueryAtMostOne(MatchRow(*table, message, columns, update.args))) {
            InvalidateMessage(table, message);
            return;
        }
        std::rethrow_exception(duplicate);
    }
}
ConditionalSaveResult Database::SaveConditional(const Message &message,
                                                const SaveOptions &options) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    SQLBuilder builder(*table);
    auto pk = builder.PrimaryKeyWhere(message);
    std::unordered_set<std::string> fences, preserved;
    std::vector<uint64_t> target;
    for (const auto &name : options.fence_fields) {
        const auto *field = table->Descriptor()->FindFieldByName(name);
        if (!field || field->is_repeated() ||
            (field->cpp_type() != Field::CPPTYPE_UINT32 &&
             field->cpp_type() != Field::CPPTYPE_UINT64) ||
            IsPK(*table, name) || name == table->AutoIncrementKey() ||
            std::find(table->NullableFields().begin(), table->NullableFields().end(), name) !=
                table->NullableFields().end() ||
            !fences.insert(name).second)
            throw std::invalid_argument("invalid save fence: " + name);
        auto value = FieldValue(message, field);
        target.push_back(std::get<uint64_t>(value));
    }
    if (!target.empty() && !options.allow_equal &&
        std::all_of(target.begin(), target.end(), [](uint64_t v) { return v == 0; }))
        throw std::invalid_argument("strict save fence cannot be all zero");
    for (const auto &name : options.preserve_fields) {
        if (!table->HasField(name) || IsPK(*table, name) || fences.count(name) ||
            !preserved.insert(name).second)
            throw std::invalid_argument("invalid preserve field: " + name);
    }
    auto columns = NonPKColumns(*table);
    columns.erase(std::remove_if(columns.begin(), columns.end(),
                                 [&](const std::string &name) { return preserved.count(name); }),
                  columns.end());
    if (columns.empty() && !preserved.empty())
        throw std::invalid_argument("preserve fields leave no writable column");
    auto insert = builder.Insert(message);
    Statement update;
    if (!columns.empty())
        update = builder.UpdateFieldsByPK(message, columns);
    else { // A PK-only table still needs an existence-safe UPDATE.
        auto column = QuoteIdentifier(table->PrimaryKeys().front());
        update = {"UPDATE " + QuoteIdentifier(table->TableName()) + " SET " + column + " = " +
                      column + " WHERE " + pk.sql,
                  pk.args};
    }
    if (!target.empty()) {
        update.sql += " AND (";
        for (size_t i = 0; i < target.size(); ++i) {
            if (i)
                update.sql += " OR ";
            update.sql += "(";
            for (size_t j = 0; j < i; ++j) {
                update.sql += QuoteIdentifier(options.fence_fields[j]) + " = ? AND ";
                update.args.push_back(target[j]);
            }
            update.sql += QuoteIdentifier(options.fence_fields[i]) +
                          (options.allow_equal && i + 1 == target.size() ? " <= ?" : " < ?") + ")";
            update.args.push_back(target[i]);
        }
        update.sql += ")";
    }
    StringVector state_columns =
        options.fence_fields.empty() ? table->PrimaryKeys() : options.fence_fields;
    QueryOptions locked;
    locked.for_update = true;
    auto state_sql = builder.SelectColumns(state_columns, pk.sql, pk.args, locked);
    auto match_sql = MatchRow(*table, message, columns, update.args);
    auto read_state = [&]() -> std::pair<bool, std::vector<uint64_t>> {
        auto row = QueryAtMostOne(state_sql);
        if (!row)
            return {false, {}};
        std::vector<uint64_t> stored;
        for (size_t i = 0; i < target.size(); ++i) {
            auto text = RowCell(*row, i);
            if (text.empty() || text[0] == '-')
                throw std::runtime_error("invalid stored fence");
            size_t pos;
            auto value = std::stoull(text, &pos);
            if (pos != text.size())
                throw std::runtime_error("invalid stored fence");
            stored.push_back(value);
        }
        return {true, std::move(stored)};
    };
    std::exception_ptr duplicate;
    std::optional<std::vector<uint64_t>> compared;
    std::vector<uint64_t> last;
    try {
        for (int round = 0; round < kConditionalSaveMaxRounds; ++round) {
            Execute(update);
            if (AffectedRows() > 0) {
                InvalidateMessage(table, message);
                return {SaveOutcome::Updated, {}};
            }
            auto state = read_state();
            if (!state.first) {
                if (duplicate)
                    std::rethrow_exception(duplicate);
                try {
                    Execute(insert);
                    InvalidateMessage(table, message);
                    return {SaveOutcome::Inserted, {}};
                } catch (const MysqlError &error) {
                    if (error.value() != ER_DUP_ENTRY)
                        throw;
                    duplicate = std::current_exception();
                    state = read_state();
                    if (!state.first)
                        std::rethrow_exception(duplicate);
                }
            }
            last = state.second;
            const bool older = std::lexicographical_compare(last.begin(), last.end(),
                                                            target.begin(), target.end());
            if (!target.empty() && !older && !(options.allow_equal && last == target))
                return {SaveOutcome::Rejected, last};
            if (!target.empty() && last != target) {
                compared.reset();
                continue;
            }
            if (QueryAtMostOne(match_sql)) {
                InvalidateMessage(table, message);
                return {SaveOutcome::Unchanged, last};
            }
            if (compared && *compared == last)
                throw ConditionalSaveError(ConditionalSaveError::Kind::MatchMismatch, last);
            compared = last;
        }
        throw ConditionalSaveError(ConditionalSaveError::Kind::Race, last);
    } catch (...) {
        InvalidateMessage(table, message);
        throw;
    }
}

void Database::Update(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).UpdateByPK(message));
    InvalidateMessage(table, message);
}
void Database::UpdateByWhere(const Message &message, const std::string &where) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    UpdateByWhereWithArgs(message, where, {});
}
void Database::UpdateByWhereWithArgs(const Message &message, const std::string &where,
                                     const std::vector<SqlValue> &args) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    Execute(Builder(message).UpdateWhere(message, where, args));
}
void Database::UpdateKVByPK(const Message &message, const std::string &field,
                            const SqlValue &value) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    const SQLBuilder builder(*table);
    const auto normalized = builder.NormalizeComparisonValue(field, value);
    auto keys = builder.PrimaryKeyWhere(message).args;
    Execute(builder.UpdateAssignsByPK(message, {SetCol(field, normalized)}));
    InvalidateMessage(table, message);
    if (IsPK(*table, field)) {
        for (size_t i = 0; i < table->PrimaryKeys().size(); ++i)
            if (table->PrimaryKeys()[i] == field)
                keys[i] = normalized;
        std::string key = "pb:" + EscapeCacheKeyPart(table->TableName());
        for (const auto &part : keys)
            key += ":" + EscapeCacheKeyPart(ValueText(part));
        InvalidateKey(key);
    }
}
void Database::UpdateFieldsByPrimaryKey(const Message &message, const StringVector &fields) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).UpdateFieldsByPK(message, fields));
    InvalidateMessage(table, message);
}
bool Database::UpdateIfVersion(const Message &message, const std::string &version) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    StringVector fields;
    for (const auto &name : NonPKColumns(*table)) {
        auto *field = table->Descriptor()->FindFieldByName(name);
        const bool populated = field->is_repeated()
                                   ? message.GetReflection()->FieldSize(message, field) > 0
                                   : message.GetReflection()->HasField(message, field);
        if (name != version && populated)
            fields.push_back(name);
    }
    return UpdateFieldsIfVersion(message, version, fields);
}
bool Database::UpdateFieldsIfVersion(const Message &message, const std::string &version,
                                     const StringVector &fields) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (fields.empty())
        throw std::invalid_argument("no fields to update");
    auto *table = TableFor(message);
    auto *field = table->Descriptor()->FindFieldByName(version);
    if (!field || !IsNumericField(field))
        throw std::invalid_argument("version must be a numeric scalar field");
    auto current = FieldComparisonValue(message, field);
    // Validate selected columns via builder before adding the version expression.
    if (!fields.empty())
        SQLBuilder(*table).UpdateFieldsByPK(message, fields);
    std::vector<Assign> assigns;
    for (const auto &name : fields) {
        if (name == version)
            continue;
        assigns.push_back(
            SetCol(name, FieldValue(message, table->Descriptor()->FindFieldByName(name))));
    }
    assigns.push_back(AddCol(version, kVersionIncrement));
    auto pk = SQLBuilder(*table).PrimaryKeyWhere(message);
    auto args = pk.args;
    args.push_back(current);
    Execute(SQLBuilder(*table).UpdateAssignsWhere(
        assigns, "(" + pk.sql + ") AND " + QuoteIdentifier(version) + " = ?", args));
    bool changed = AffectedRows() > 0;
    if (changed)
        InvalidateMessage(table, message);
    if (changed)
        InvalidateShiftedPrimaryKey(*table, message, version, kVersionIncrement);
    return changed;
}
void Database::IncrByPrimaryKey(const Message &message, const std::string &field, int64_t delta) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).IncrByPK(message, field, delta));
    InvalidateMessage(table, message);
    if (AffectedRows() > 0)
        InvalidateShiftedPrimaryKey(*table, message, field, delta);
}
bool Database::DecrByPrimaryKeyIfEnough(const Message &message, const std::string &field,
                                        int64_t delta) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).DecrByPKIfEnough(message, field, delta));
    bool changed = AffectedRows() > 0;
    if (changed)
        InvalidateMessage(table, message);
    if (changed)
        InvalidateShiftedPrimaryKey(*table, message, field, -delta);
    return changed;
}
void Database::Delete(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).DeleteByPK(message));
    InvalidateMessage(table, message);
}
void Database::DeleteByKV(const Message &message, const std::string &key, const SqlValue &value) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    const SQLBuilder builder(*table);
    Execute(builder.DeleteWhere(QuoteIdentifier(key) + " = ?",
                                {builder.NormalizeComparisonValue(key, value)}));
}
void Database::DeleteByWhere(const Message &message, const std::string &where) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    DeleteByWhereWithArgs(message, where, {});
}
void Database::DeleteByWhereWithArgs(const Message &message, const std::string &where,
                                     const std::vector<SqlValue> &args) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    Execute(Builder(message).DeleteWhere(where, args));
}
void Database::BatchDelete(const std::vector<const Message *> &messages) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (messages.empty())
        return;
    if (!messages.front())
        throw std::invalid_argument("null delete message");
    auto *table = TableFor(*messages.front());
    const SQLBuilder builder(*table);
    std::vector<std::vector<SqlValue>> keys;
    for (const auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null delete message");
        keys.push_back(builder.PrimaryKeyWhere(*message).args);
    }
    std::string columns;
    std::string placeholders;
    for (const auto &name : table->PrimaryKeys()) {
        if (!columns.empty()) {
            columns += ", ";
            placeholders += ", ";
        }
        columns += QuoteIdentifier(name);
        placeholders += '?';
    }
    for (size_t start = 0; start < messages.size(); start += kBatchSize) {
        const auto end = std::min(start + kBatchSize, messages.size());
        std::string where = "(" + columns + ") IN (";
        std::vector<SqlValue> args;
        for (size_t i = start; i < end; ++i) {
            if (i != start)
                where += ", ";
            where += "(" + placeholders + ")";
            args.insert(args.end(), keys[i].begin(), keys[i].end());
        }
        Execute(builder.DeleteWhere(where + ")", args));
        for (size_t i = start; i < end; ++i)
            InvalidateMessage(table, *messages[i]);
    }
}

bool Database::Transaction(const std::function<bool(Database &)> &fn) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("nested transactions are unsupported");
    try {
        Execute(std::string("START TRANSACTION"));
    } catch (...) {
        Disconnect();
        throw;
    }
    in_transaction_ = true;
    pending_cache_deletes_.clear();
    bool committed = false;
    bool commit_attempted = false;
    try {
        if (fn(*this)) {
            commit_attempted = true;
            Execute(std::string("COMMIT"));
            committed = true;
        } else
            Execute(std::string("ROLLBACK"));
    } catch (...) {
        bool rolled_back = false;
        try {
            Execute(std::string("ROLLBACK"));
            rolled_back = true;
        } catch (...) {
        }
        if (!rolled_back)
            Disconnect();
        in_transaction_ = false;
        // COMMIT failures can have an unknown outcome. Invalidating cached rows
        // is safe even when the server rolled back.
        if (commit_attempted || !rolled_back)
            FlushPendingCacheDeletes();
        else
            pending_cache_deletes_.clear();
        throw;
    }
    in_transaction_ = false;
    if (committed)
        FlushPendingCacheDeletes();
    else
        pending_cache_deletes_.clear();
    return committed;
}
void Database::EnableCache(Cache *cache, int ttl_seconds) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot replace cache during transaction");
    cache_ = cache;
    cache_ttl_seconds_ = ttl_seconds;
}
std::string Database::CacheKey(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    auto pk = SQLBuilder(*table).PrimaryKeyWhere(message);
    std::string key = "pb:" + EscapeCacheKeyPart(table->TableName());
    for (const auto &value : pk.args)
        key += ":" + EscapeCacheKeyPart(ValueText(value));
    return key;
}
void Database::InvalidateCache(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    InvalidateMessage(TableFor(message), message);
}
void Database::InvalidateCache(const std::vector<const Message *> &messages) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    std::vector<std::string> keys;
    for (const auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null cache message");
        keys.push_back(CacheKey(*message));
    }
    for (const auto &key : keys)
        InvalidateKey(key);
}
void Database::InvalidateMessage(MessageTable *, const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (!cache_)
        return;
    std::string key;
    try {
        key = CacheKey(message);
    } catch (...) {
        return;
    }
    InvalidateKey(key);
}
void Database::InvalidateKey(const std::string &key) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (!cache_)
        return;
    if (in_transaction_)
        pending_cache_deletes_.push_back(key);
    else {
        try {
            cache_->Del(key);
        } catch (...) {
        }
    }
}
void Database::InvalidateShiftedPrimaryKey(MessageTable &table, const Message &message,
                                           const std::string &field, int64_t delta) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (!cache_ || !IsPK(table, field) || delta == 0)
        return;
    auto keys = SQLBuilder(table).PrimaryKeyWhere(message).args;
    for (size_t i = 0; i < table.PrimaryKeys().size(); ++i) {
        if (table.PrimaryKeys()[i] != field)
            continue;
        if (auto *value = std::get_if<uint64_t>(&keys[i])) {
            const auto amount = delta >= 0 ? static_cast<uint64_t>(delta)
                                           : uint64_t{0} - static_cast<uint64_t>(delta);
            if ((delta >= 0 && *value > std::numeric_limits<uint64_t>::max() - amount) ||
                (delta < 0 && *value < amount))
                return;
            *value = delta >= 0 ? *value + amount : *value - amount;
        } else if (auto *value = std::get_if<int64_t>(&keys[i])) {
            if ((delta > 0 && *value > std::numeric_limits<int64_t>::max() - delta) ||
                (delta < 0 && *value < std::numeric_limits<int64_t>::min() - delta))
                return;
            *value += delta;
        } else
            return;
    }
    std::string key = "pb:" + EscapeCacheKeyPart(table.TableName());
    for (const auto &part : keys)
        key += ":" + EscapeCacheKeyPart(ValueText(part));
    InvalidateKey(key);
}
void Database::FlushPendingCacheDeletes() {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (cache_) {
        std::set<std::string> unique(pending_cache_deletes_.begin(), pending_cache_deletes_.end());
        for (const auto &key : unique) {
            try {
                cache_->Del(key);
            } catch (...) {
            }
        }
    }
    pending_cache_deletes_.clear();
}
} // namespace pbmysql
