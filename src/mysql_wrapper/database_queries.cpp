#include "src/mysql_wrapper/cache_codec.h"
#include "src/mysql_wrapper/database.h"
#include <limits>

namespace pbmysql {
const google::protobuf::FieldDescriptor *
Database::ListField(const Message &list, const google::protobuf::Descriptor *expected) {
    const google::protobuf::FieldDescriptor *result = nullptr;
    const auto *descriptor = list.GetDescriptor();
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto *field = descriptor->field(i);
        if (!field->is_repeated() || field->is_map())
            continue;
        if (result)
            throw std::invalid_argument("list message contains multiple repeated fields");
        result = field;
    }
    if (!result || result->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE ||
        (expected && result->message_type() != expected))
        throw std::invalid_argument(
            "list message requires one repeated message field of the requested type");
    return result;
}
MessageTable *Database::ResolveListTable(const Message &list) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto *field = ListField(list);
    auto found = tables_.find(std::string(field->message_type()->full_name()));
    if (found == tables_.end() || found->second.Descriptor() != field->message_type())
        throw std::invalid_argument("list element table is not registered");
    return &found->second;
}
MessageTable *Database::ResolveAnyTable(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto found = tables_.find(std::string(message.GetDescriptor()->full_name()));
    return found == tables_.end() ? ResolveListTable(message) : TableFor(message);
}
MysqlClient::ResultRowPtr Database::QueryAtMostOne(const Statement &statement) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    ResultRowPtr result;
    QueryResultRowProcessor(statement, [&](const ResultRowPtr &row) {
        if (result)
            throw MultipleRowsError();
        result = std::make_unique<ResultRow>(row->values());
        return true;
    });
    return result;
}
bool Database::LoadOne(Message &message, const Statement &statement) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto row = QueryAtMostOne(statement);
    if (!row)
        return false;
    ParseFromRow(message, *row);
    return true;
}
void Database::LoadList(Message &list, const Statement &statement) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto *field = ListField(list);
    const auto *reflection = list.GetReflection();
    std::unique_ptr<Message> staged(list.New());
    staged->CopyFrom(list);
    staged->GetReflection()->ClearField(staged.get(), field);
    QueryResultRowProcessor(statement, [&](const ResultRowPtr &row) {
        auto *appended = staged->GetReflection()->AddMessage(staged.get(), field);
        ParseFromRow(*appended, *row);
        return true;
    });
    reflection->SwapFields(&list, staged.get(), {field});
}
int64_t Database::QueryScalar(const Statement &statement, int64_t fallback) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto row = QueryAtMostOne(statement);
    if (!row)
        return fallback;
    if (!row->size() || !(*row)[0])
        throw std::runtime_error("unexpected NULL scalar result");
    const std::string text((*row)[0], row->length(0));
    size_t position = 0;
    const auto value = std::stoll(text, &position);
    if (position != text.size())
        throw std::runtime_error("invalid scalar result");
    return value;
}
bool Database::FindOneByPrimaryKey(Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = TableFor(message);
    if (cache_ && !in_transaction_) {
        try {
            std::string data;
            if (cache_->Get(CacheKey(message), &data) && DecodeCacheEntry(data, message))
                return true;
        } catch (...) {
        }
    }
    if (!LoadOne(message, SQLBuilder(*table).SelectByPK(message)))
        return false;
    if (cache_ && !in_transaction_) {
        try {
            cache_->Set(CacheKey(message), EncodeCacheEntry(message), cache_ttl_seconds_);
        } catch (...) {
        }
    }
    return true;
}
bool Database::FindOneByPrimaryKeyForUpdate(Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (!in_transaction_)
        throw std::logic_error("FOR UPDATE requires Transaction()");
    return LoadOne(message, Builder(message).SelectByPKForUpdate(message));
}
bool Database::FindOrCreate(Message &message, bool *created) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (created)
        *created = false;
    if (FindOneByPrimaryKey(message))
        return true;
    Insert(message);
    if (created)
        *created = true;
    return true;
}
bool Database::FindOneByKV(Message &message, const std::string &field, const SqlValue &value) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto builder = Builder(message);
    return FindOneByWhereWithArgs(message, QuoteIdentifier(field) + " = ?",
                                  {builder.NormalizeComparisonValue(field, value)});
}
bool Database::FindOneByWhere(Message &message, const std::string &where) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return FindOneByWhereWithArgs(message, where, {});
}
bool Database::FindOneByWhereWithArgs(Message &message, const std::string &where,
                                      const std::vector<SqlValue> &args) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return LoadOne(message, Builder(message).SelectWhere(where, args));
}
bool Database::FindOneWithOptions(Message &message, const std::string &where,
                                  const std::vector<SqlValue> &args, const QueryOptions &options) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto single = options;
    single.limit = 1;
    single.offset = 0;
    return LoadOne(message, Builder(message).SelectWhere(where, args, single));
}
void Database::FindMultiByWhereClauses(const std::vector<MultiQuery> &queries) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (queries.empty())
        throw std::invalid_argument("no queries provided");
    std::vector<Statement> statements;
    for (const auto &query : queries) {
        if (!query.message)
            throw std::invalid_argument("null query message");
        statements.push_back(
            Builder(*query.message).SelectWhere(query.where_clause, query.where_args));
    }
    for (size_t i = 0; i < queries.size(); ++i)
        if (!LoadOne(*queries[i].message, statements[i]))
            throw std::runtime_error("multi-query returned no row");
}
void Database::FindAll(Message &list) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    FindAllByWhereWithArgs(list, "", {});
}
void Database::FindAllByWhereWithArgs(Message &list, const std::string &where,
                                      const std::vector<SqlValue> &args) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    LoadList(list, SQLBuilder(*ResolveListTable(list)).SelectWhere(where, args));
}
void Database::FindAllByPKIn(Message &list, const std::vector<SqlValue> &values) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = ResolveListTable(list);
    if (values.empty()) {
        list.GetReflection()->ClearField(&list, ListField(list));
        return;
    }
    LoadList(list, SQLBuilder(*table).SelectByPKIn(values));
}
void Database::FindAllByKVIn(Message &list, const std::string &field,
                             const std::vector<SqlValue> &values) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto *table = ResolveListTable(list);
    if (values.empty()) {
        list.GetReflection()->ClearField(&list, ListField(list));
        return;
    }
    LoadList(list, SQLBuilder(*table).SelectByKVIn(field, values));
}
void Database::FindMultiByKV(Message &list, const std::string &field, const SqlValue &value) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto builder = SQLBuilder(*ResolveListTable(list));
    LoadList(list, builder.SelectWhere(QuoteIdentifier(field) + " = ?",
                                       {builder.NormalizeComparisonValue(field, value)}));
}
void Database::FindAllWithOptions(Message &list, const std::string &where,
                                  const std::vector<SqlValue> &args, const QueryOptions &options) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    LoadList(list, SQLBuilder(*ResolveListTable(list)).SelectWhere(where, args, options));
}
void Database::FindPage(Message &list, const std::string &where, const std::vector<SqlValue> &args,
                        int page_index, int page_size) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (page_index < 1 || page_size < 1)
        throw std::invalid_argument("page index and size must be positive");
    const auto offset = static_cast<int64_t>(page_index - 1) * page_size;
    if (offset > std::numeric_limits<int>::max())
        throw std::invalid_argument("page offset overflow");
    QueryOptions options;
    options.limit = page_size;
    options.offset = static_cast<int>(offset);
    FindAllWithOptions(list, where, args, options);
}
void Database::FindPageByCursor(Message &list, const std::string &where,
                                const std::vector<SqlValue> &where_args, const std::string &field,
                                const std::optional<SqlValue> &cursor, int page_size) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (page_size < 1)
        throw std::invalid_argument("page size must be positive");
    const auto builder = SQLBuilder(*ResolveListTable(list));
    if (!builder.Table().HasField(field))
        throw std::invalid_argument("unknown cursor field: " + field);
    std::string clause = NormalizeWhereClause(where);
    auto args = where_args;
    if (cursor && !std::holds_alternative<std::nullptr_t>(*cursor)) {
        clause = "(" + clause + ") AND " + QuoteIdentifier(field) + " > ?";
        args.push_back(builder.NormalizeComparisonValue(field, *cursor));
    }
    QueryOptions options;
    options.limit = page_size;
    options.order_by = QuoteIdentifier(field) + " ASC";
    LoadList(list, builder.SelectWhere(clause, args, options));
}
int64_t Database::Count(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return CountByWhereWithArgs(message, "", {});
}
int64_t Database::CountByWhereWithArgs(const Message &message, const std::string &where,
                                       const std::vector<SqlValue> &args) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return QueryScalar(SQLBuilder(*ResolveAnyTable(message)).Count(where, args), 0);
}
bool Database::Exists(const Message &message, const std::string &where,
                      const std::vector<SqlValue> &args) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return QueryAtMostOne(SQLBuilder(*ResolveAnyTable(message)).Exists(where, args)) != nullptr;
}
bool Database::ExistsByPrimaryKey(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto builder = Builder(message);
    const auto pk = builder.PrimaryKeyWhere(message);
    return QueryAtMostOne(builder.Exists(pk.sql, pk.args)) != nullptr;
}
} // namespace pbmysql
