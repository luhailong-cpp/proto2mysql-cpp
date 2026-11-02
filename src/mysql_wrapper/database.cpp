#include "src/mysql_wrapper/database.h"

#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/cache_codec.h"
#include <algorithm>
#include <exception>
#include <fstream>
#include <set>
#include <unordered_set>

namespace pbmysql {
namespace {
using Message = google::protobuf::Message;
using Field = google::protobuf::FieldDescriptor;

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
void RegisterDescriptor(Database &db, const google::protobuf::Descriptor *desc, bool modern) {
    bool declared = modern ? desc->options().HasExtension(proto2mysql::table_name)
                           : desc->options().HasExtension(OptionTableName);
    if (declared) {
        auto *prototype = google::protobuf::MessageFactory::generated_factory()->GetPrototype(desc);
        if (!prototype)
            throw std::invalid_argument("no generated prototype for " +
                                        std::string(desc->full_name()));
        db.RegisterTable(*prototype);
    }
    for (int i = 0; i < desc->nested_type_count(); ++i)
        RegisterDescriptor(db, desc->nested_type(i), modern);
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
std::string SchemaLockName(const std::string &database, const std::string &table) {
    // MySQL limits lock names to 64 bytes. Hash a length-delimited identity so
    // valid long table/database names remain synchronizable across processes.
    uint64_t hash = 14695981039346656037ULL;
    const std::string identity = std::to_string(database.size()) + ":" + database + table;
    for (unsigned char byte : identity) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return "p2m:" + std::to_string(hash);
}
} // namespace

void Database::SetDbName(const std::string &name) {
    if (in_transaction_)
        throw std::logic_error("cannot change database during transaction");
    db_name_ = name;
    for (auto &entry : tables_)
        entry.second.SetDbName(name);
}
void Database::RegisterTable(const Message &message, const TableOptions &options) {
    if (in_transaction_)
        throw std::logic_error("cannot register tables during transaction");
    std::string key(message.GetDescriptor()->full_name());
    MessageTable table(message, options);
    table.SetDbName(db_name_);
    // Erasing then replacing would lose the valid registration on validation
    // failure. Fully validate the replacement before changing the registry.
    table.CreateTableSql();
    tables_.insert_or_assign(key, std::move(table));
}
void Database::RegisterAllTables(const google::protobuf::FileDescriptor *file) {
    if (!file)
        throw std::invalid_argument("null file descriptor");
    const bool modern = file->options().HasExtension(proto2mysql::db);
    if (modern && !file->options().GetExtension(proto2mysql::db))
        return;
    for (int i = 0; i < file->message_type_count(); ++i)
        RegisterDescriptor(*this, file->message_type(i), modern);
}
MessageTable *Database::TableFor(const Message &message) {
    auto it = tables_.find(std::string(message.GetDescriptor()->full_name()));
    if (it == tables_.end())
        throw std::invalid_argument("table not registered: " +
                                    std::string(message.GetDescriptor()->full_name()));
    it->second.ValidateMessage(message);
    return &it->second;
}
SQLBuilder Database::Builder(const Message &message) { return SQLBuilder(*TableFor(message)); }
std::string Database::GetCreateTableSql(const Message &message) {
    return TableFor(message)->CreateTableSql();
}
void Database::WriteCreateTableSql(std::ostream &out) const {
    std::vector<std::string> keys;
    for (const auto &entry : tables_)
        keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    std::vector<std::string> statements;
    for (const auto &key : keys)
        statements.push_back(tables_.at(key).CreateTableSql());
    for (const auto &statement : statements)
        out << statement << (statement.back() == ';' ? "" : ";") << "\n\n";
    if (!out)
        throw std::runtime_error("failed writing schema SQL");
}
void Database::DumpCreateTableSqlFile(const std::string &path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot open schema file: " + path);
    WriteCreateTableSql(out);
}
bool Database::IsTableExists(const std::string &name) {
    return QueryScalar({"SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = ? AND "
                        "TABLE_NAME = ?",
                        {db_name_, name}},
                       0) > 0;
}
std::string Database::GenerateMigrationSql(const Message &message) {
    auto *table = TableFor(message);
    if (!IsTableExists(table->TableName()))
        return table->CreateTableSql();
    table->ClearExistingColumns();
    Query(table->SelectColumnsSql(),
          [table](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table->OnSelectColumn(row, lengths, size);
          });
    table->ClearExistingIndexes();
    Query(table->SelectIndexesSql(),
          [table](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table->OnSelectIndex(row, lengths, size);
          });
    return table->AlterTableSql();
}
void Database::WriteMigrationSql(std::ostream &out, const std::vector<const Message *> &messages) {
    std::vector<std::string> statements;
    for (auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null migration message");
        auto sql = GenerateMigrationSql(*message);
        if (!sql.empty())
            statements.push_back(std::move(sql));
    }
    for (const auto &sql : statements)
        out << sql << (sql.back() == ';' ? "" : ";") << "\n\n";
    if (!out)
        throw std::runtime_error("failed writing migration SQL");
}
void Database::DumpMigrationSqlFile(const std::string &path,
                                    const std::vector<const Message *> &messages) {
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot open migration file: " + path);
    WriteMigrationSql(out, messages);
}
void Database::CreateOrUpdateTable(const Message &message) {
    if (in_transaction_)
        throw std::logic_error("schema DDL implicitly commits transactions");
    auto *table = TableFor(message);
    // Serializes cooperating schema writers. GET_LOCK is session-scoped and
    // survives MySQL's implicit DDL commit.
    const std::string lock = SchemaLockName(db_name_, table->TableName());
    int64_t acquired;
    try {
        acquired = QueryScalar({"SELECT GET_LOCK(?, 30)", {lock}}, 0);
    } catch (...) {
        Disconnect();
        throw;
    }
    if (acquired != 0 && acquired != 1)
        Disconnect();
    if (acquired != 1)
        throw std::runtime_error("schema lock unavailable");
    auto release = [&] {
        try {
            if (QueryScalar({"SELECT RELEASE_LOCK(?)", {lock}}, 0) != 1)
                throw std::runtime_error("schema lock release was not confirmed");
        } catch (...) {
            // Closing the session also releases advisory locks. A live session
            // must never retain a lock after an ambiguous release failure.
            Disconnect();
            throw;
        }
    };
    try {
        const auto sql = GenerateMigrationSql(message);
        if (!sql.empty())
            Execute(sql);
    } catch (...) {
        try {
            release();
        } catch (...) {
        }
        throw;
    }
    release();
}
void Database::SyncAllTables() {
    std::vector<std::string> keys;
    for (const auto &entry : tables_)
        keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    for (const auto &key : keys)
        CreateOrUpdateTable(tables_.at(key).DefaultInstance());
}
int64_t Database::QueryScalar(const Statement &sql, int64_t fallback) {
    auto row = QueryOne(sql);
    if (!row)
        return fallback;
    auto cell = RowCell(*row, 0);
    size_t pos;
    auto value = std::stoll(cell, &pos);
    if (pos != cell.size())
        throw std::runtime_error("invalid integer query result");
    return value;
}

void Database::Insert(const Message &message) {
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).Insert(message));
    InvalidateMessage(table, message);
}
bool Database::InsertIgnore(const Message &message) {
    try {
        Insert(message);
        return true;
    } catch (const MysqlError &error) {
        if (error.value() == 1062)
            return false;
        throw;
    }
}
uint64_t Database::InsertReturningId(const Message &message) {
    Insert(message);
    return LastInsertId();
}
void Database::InsertOnDupUpdate(const Message &message) { Save(message); }
void Database::BatchInsert(const std::vector<const Message *> &messages) {
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
    for (size_t start = 0; start < messages.size(); start += 1000) {
        const auto end = std::min(start + 1000, messages.size());
        std::vector<const Message *> chunk(messages.begin() + start, messages.begin() + end);
        Execute(SQLBuilder(*table).BatchInsert(chunk));
        for (auto *message : chunk)
            InvalidateMessage(table, *message);
    }
}
void Database::BatchSave(const std::vector<const Message *> &messages) {
    for (auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null batch message");
        Save(*message);
    }
}
void Database::Save(const Message &message) { SaveConditional(message); }
ConditionalSaveResult Database::SaveConditional(const Message &message,
                                                const SaveOptions &options) {
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
    locked.limit = 1;
    auto state_sql = builder.SelectColumns(state_columns, pk.sql, pk.args, locked);
    auto match_sql = MatchRow(*table, message, columns, update.args);
    auto read_state = [&]() -> std::pair<bool, std::vector<uint64_t>> {
        auto row = QueryOne(state_sql);
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
        for (int round = 0; round < 2; ++round) {
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
                    if (error.value() != 1062)
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
            if (QueryOne(match_sql)) {
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
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).UpdateByPK(message));
    InvalidateMessage(table, message);
}
void Database::UpdateByWhere(const Message &message, const std::string &where) {
    UpdateByWhereWithArgs(message, where, {});
}
void Database::UpdateByWhereWithArgs(const Message &message, const std::string &where,
                                     const std::vector<SqlValue> &args) {
    Execute(Builder(message).UpdateWhere(message, where, args));
}
void Database::UpdateKvByPrimaryKey(const Message &message, const std::string &field,
                                    const std::string &value) {
    auto *table = TableFor(message);
    if (IsPK(*table, field))
        throw std::invalid_argument("cannot update primary key");
    Execute(SQLBuilder(*table).UpdateAssignsByPK(message, {SetCol(field, value)}));
    InvalidateMessage(table, message);
}
void Database::UpdateFieldsByPrimaryKey(const Message &message, const StringVector &fields) {
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).UpdateFieldsByPK(message, fields));
    InvalidateMessage(table, message);
}
bool Database::UpdateIfVersion(const Message &message, const std::string &version) {
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
    auto *table = TableFor(message);
    auto *field = table->Descriptor()->FindFieldByName(version);
    if (!field || field->is_repeated() || !IsNumericField(field) || IsPK(*table, version) ||
        field->cpp_type() == Field::CPPTYPE_FLOAT || field->cpp_type() == Field::CPPTYPE_DOUBLE ||
        field->cpp_type() == Field::CPPTYPE_BOOL || field->cpp_type() == Field::CPPTYPE_ENUM)
        throw std::invalid_argument("version must be a non-primary integer field");
    auto current = FieldValue(message, field);
    // Validate selected columns via builder before adding the version expression.
    if (!fields.empty())
        SQLBuilder(*table).UpdateFieldsByPK(message, fields);
    std::vector<Assign> assigns;
    for (const auto &name : fields) {
        if (name == version)
            throw std::invalid_argument("version cannot also be assigned explicitly");
        assigns.push_back(
            SetCol(name, FieldValue(message, table->Descriptor()->FindFieldByName(name))));
    }
    assigns.push_back(AddCol(version, int64_t{1}));
    auto pk = SQLBuilder(*table).PrimaryKeyWhere(message);
    auto args = pk.args;
    args.push_back(current);
    Execute(SQLBuilder(*table).UpdateAssignsWhere(
        assigns, "(" + pk.sql + ") AND " + QuoteIdentifier(version) + " = ?", args));
    bool changed = AffectedRows() > 0;
    if (changed)
        InvalidateMessage(table, message);
    return changed;
}
void Database::IncrByPrimaryKey(const Message &message, const std::string &field, int64_t delta) {
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).IncrByPK(message, field, delta));
    InvalidateMessage(table, message);
}
bool Database::DecrByPrimaryKeyIfEnough(const Message &message, const std::string &field,
                                        int64_t delta) {
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).DecrByPKIfEnough(message, field, delta));
    bool changed = AffectedRows() > 0;
    if (changed)
        InvalidateMessage(table, message);
    return changed;
}
void Database::Delete(const Message &message) {
    auto *table = TableFor(message);
    Execute(SQLBuilder(*table).DeleteByPK(message));
    InvalidateMessage(table, message);
}
void Database::DeleteByKv(const Message &message, const std::string &key,
                          const std::string &value) {
    auto *table = TableFor(message);
    if (!table->HasField(key))
        throw std::invalid_argument("unknown field: " + key);
    Execute(SQLBuilder(*table).DeleteWhere(QuoteIdentifier(key) + " = ?", {value}));
}
void Database::DeleteByWhere(const Message &message, const std::string &where) {
    DeleteByWhereWithArgs(message, where, {});
}
void Database::DeleteByWhereWithArgs(const Message &message, const std::string &where,
                                     const std::vector<SqlValue> &args) {
    Execute(Builder(message).DeleteWhere(where, args));
}
void Database::BatchDelete(const std::vector<const Message *> &messages) {
    for (auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null delete message");
        Delete(*message);
    }
}

bool Database::FindOneByPrimaryKey(Message &message) {
    auto *table = TableFor(message);
    if (cache_ && !in_transaction_) {
        try {
            std::string value;
            if (cache_->Get(CacheKey(message), &value) && DecodeCacheEntry(value, message))
                return true;
        } catch (...) {
        } // Cache failures never change DB results.
    }
    auto row = QueryOne(SQLBuilder(*table).SelectByPK(message));
    if (!row)
        return false;
    ParseFromRow(message, *row);
    if (cache_ && !in_transaction_) {
        try {
            cache_->Set(CacheKey(message), EncodeCacheEntry(message), cache_ttl_seconds_);
        } catch (...) {
        }
    }
    return true;
}
bool Database::FindOneByPrimaryKeyForUpdate(Message &message) {
    if (!in_transaction_)
        throw std::logic_error("FOR UPDATE requires Transaction()");
    auto row = QueryOne(Builder(message).SelectByPKForUpdate(message));
    if (!row)
        return false;
    ParseFromRow(message, *row);
    return true;
}
bool Database::FindOrCreate(Message &message, bool *created) {
    if (created)
        *created = false;
    if (FindOneByPrimaryKey(message))
        return true;
    try {
        Insert(message);
        if (created)
            *created = true;
        return true;
    } catch (const MysqlError &error) {
        if (error.value() != 1062)
            throw;
        // A duplicate on a different unique row must remain an error. Bypass
        // cache and use a current read to classify a concurrent PK insertion.
        auto row = QueryOne(Builder(message).SelectByPKForUpdate(message));
        if (!row)
            throw;
        ParseFromRow(message, *row);
        return true;
    }
}
bool Database::FindOneByKv(Message &message, const std::string &key, const std::string &value) {
    auto *table = TableFor(message);
    if (!table->HasField(key))
        throw std::invalid_argument("unknown field: " + key);
    return FindOneByWhereWithArgs(message, QuoteIdentifier(key) + " = ?", {value});
}
bool Database::FindOneByWhere(Message &message, const std::string &where) {
    return FindOneByWhereWithArgs(message, where, {});
}
bool Database::FindOneByWhereWithArgs(Message &message, const std::string &where,
                                      const std::vector<SqlValue> &args) {
    QueryOptions options;
    options.limit = 1;
    auto row = QueryOne(Builder(message).SelectWhere(where, args, options));
    if (!row)
        return false;
    ParseFromRow(message, *row);
    return true;
}
bool Database::FindOneWithOptions(Message &message, const std::string &where,
                                  const QueryOptions &options) {
    auto one = options;
    one.limit = 1;
    if (one.for_update && !in_transaction_)
        throw std::logic_error("FOR UPDATE requires Transaction()");
    auto row = QueryOne(Builder(message).SelectWhere(where, {}, one));
    if (!row)
        return false;
    ParseFromRow(message, *row);
    return true;
}
void Database::FindMultiByWhereClauses(const std::vector<MultiQuery> &queries) {
    for (const auto &query : queries) {
        if (!query.message)
            throw std::invalid_argument("null query message");
        FindOneByWhere(*query.message, query.where_clause);
    }
}
int64_t Database::Count(const Message &message) { return QueryScalar(Builder(message).Count(), 0); }
int64_t Database::CountByWhere(const Message &message, const std::string &where) {
    return QueryScalar(Builder(message).Count(where), 0);
}
bool Database::Exists(const Message &message, const std::string &where) {
    return QueryOne(Builder(message).Exists(where)) != nullptr;
}
bool Database::ExistsByPrimaryKey(const Message &message) {
    auto builder = Builder(message);
    auto pk = builder.PrimaryKeyWhere(message);
    return QueryOne(builder.Exists(pk.sql, pk.args)) != nullptr;
}

bool Database::Transaction(const std::function<bool(Database &)> &fn) {
    if (in_transaction_)
        throw std::logic_error("nested transactions are unsupported");
    Execute(std::string("START TRANSACTION"));
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
    if (in_transaction_)
        throw std::logic_error("cannot replace cache during transaction");
    cache_ = cache;
    cache_ttl_seconds_ = ttl_seconds;
}
std::string Database::CacheKey(const Message &message) {
    auto *table = TableFor(message);
    auto pk = SQLBuilder(*table).PrimaryKeyWhere(message);
    std::string key = "pb:" + EscapeCacheKeyPart(table->TableName());
    for (const auto &value : pk.args)
        key += ":" + EscapeCacheKeyPart(ValueText(value));
    return key;
}
void Database::InvalidateCache(const Message &message) {
    InvalidateMessage(TableFor(message), message);
}
void Database::InvalidateMessage(MessageTable *, const Message &message) {
    if (!cache_)
        return;
    std::string key;
    try {
        key = CacheKey(message);
    } catch (...) {
        return;
    }
    if (in_transaction_)
        pending_cache_deletes_.push_back(key);
    else {
        try {
            cache_->Del(key);
        } catch (...) {
        }
    }
}
void Database::FlushPendingCacheDeletes() {
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
