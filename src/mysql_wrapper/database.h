#pragma once

#include "src/mysql_wrapper/cache.h"
#include "src/mysql_wrapper/descriptor_registry.h"
#include "src/mysql_wrapper/message_table.h"
#include "src/mysql_wrapper/mysql_client.h"
#include "src/mysql_wrapper/sql_builder.h"
#include <cstdint>
#include <functional>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbmysql {
struct SaveOptions {
    StringVector fence_fields;
    bool allow_equal = false;
    StringVector preserve_fields;
};
enum class SaveOutcome { Unknown, Updated, Inserted, Unchanged, Rejected };
struct ConditionalSaveResult {
    SaveOutcome outcome = SaveOutcome::Unknown;
    std::vector<uint64_t> stored;
    bool Applied() const {
        return outcome == SaveOutcome::Updated || outcome == SaveOutcome::Inserted ||
               outcome == SaveOutcome::Unchanged;
    }
};
class ConditionalSaveError : public std::runtime_error {
  public:
    enum class Kind { Race, MatchMismatch };
    ConditionalSaveError(Kind reason, std::vector<uint64_t> fence)
        : std::runtime_error(reason == Kind::Race ? "conditional save race"
                                                  : "save match mismatch"),
          kind(reason), stored(std::move(fence)) {}
    Kind kind;
    std::vector<uint64_t> stored;
};
class MultipleRowsError : public std::runtime_error {
  public:
    MultipleRowsError() : std::runtime_error("single-row query returned multiple rows") {}
};
struct ExecutionResult {
    uint64_t affected_rows = 0;
    uint64_t last_insert_id = 0;
};

// A Database serializes access to its connection, transaction and cache state.
// Separate instances permit concurrent SQL; a transaction holds this instance's
// connection for its entire callback, including commit/rollback cleanup.
// PascalCase methods use PK/KV consistently. Original long-form names remain
// source-compatible wrappers so applications can migrate independently.
class Database : public MysqlClient {
  public:
    using Message = google::protobuf::Message;
    using TableMap = std::unordered_map<std::string, MessageTable>;
    using MysqlClient::Connect;
    void Disconnect();
    bool Connected() const {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
        return MysqlClient::Connected();
    }
    void Execute(const std::string &sql) override;
    void Execute(const Statement &statement) override;
    ExecutionResult ExecuteStatement(const Statement &statement);
    ResultRowPtr QueryOne(const std::string &sql) override;
    ResultRowPtr QueryOne(const Statement &statement) override;
    void Query(const std::string &sql, const RowProcessor &processor) override;
    void Query(const Statement &statement, const RowProcessor &processor) override;
    void QueryResultRowProcessor(const std::string &sql,
                                 const ResultRowProcessor &processor) override;
    void QueryResultRowProcessor(const Statement &statement,
                                 const ResultRowProcessor &processor) override;
    uint64_t LastInsertId() override;
    uint64_t AffectedRows() override;
    void Connect(const std::string &host, const std::string &user, const std::string &password,
                 const std::string &database, unsigned int port = 3306);
    void Connect(const std::string &host, const std::string &user, const std::string &password,
                 const std::string &database, unsigned int port, const ConnectionOptions &options);
    template <class ConnectionParameters> void Connect(const ConnectionParameters &parameters) {
        Connect(parameters.db_host(), parameters.db_user(), parameters.db_password(),
                parameters.db_dbname(), static_cast<unsigned int>(parameters.db_port()));
    }
    void Close();
    void SetDbName(const std::string &name);
    std::string DbName() const {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
        return db_name_;
    }
    void SetExpandOnly(bool enabled);
    bool ExpandOnly() const {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
        return expand_only_;
    }
    void RegisterTable(const Message &prototype, const TableOptions &options = {});
    StringVector RegisterAllTables();
    StringVector RegisterAllTables(const google::protobuf::FileDescriptor *file);
    StringVector
    RegisterAllTables(const std::vector<const google::protobuf::FileDescriptor *> &files);
    StringVector RegisterAllTables(const std::shared_ptr<DescriptorSet> &descriptors);
    StringVector RegisterDescriptorSet(const std::string &path);
    std::unique_ptr<Message> NewMessage(const std::string &full_name) const;
    void CreateOrUpdateTable(const Message &message);
    void UpdateTableField(const Message &message);
    void SyncAllTables();
    void Init() { SyncAllTables(); }
    bool IsTableExists(const std::string &table_name);
    std::string GetCreateTableSql(const Message &message);
    std::string GenerateMigrationSql(const Message &message);
    void WriteCreateTableSql(std::ostream &output) const;
    void DumpCreateTableSqlFile(const std::string &path) const;
    void WriteMigrationSql(std::ostream &output, const std::vector<const Message *> &messages);
    void DumpMigrationSqlFile(const std::string &path,
                              const std::vector<const Message *> &messages);
    SQLBuilder Builder(const Message &message);

    void Insert(const Message &message);
    bool InsertIgnore(const Message &message);
    uint64_t InsertReturningId(const Message &message);
    uint64_t InsertReturningID(const Message &message) { return InsertReturningId(message); }
    void InsertOnDupUpdate(const Message &message);
    void BatchInsert(const std::vector<const Message *> &messages);
    void Save(const Message &message);
    ConditionalSaveResult SaveConditional(const Message &message, const SaveOptions &options = {});
    void BatchSave(const std::vector<const Message *> &messages);
    template <class T> void BatchSave(const Message &list) {
        const auto *field = ListField(list, T::descriptor());
        std::vector<const Message *> rows;
        for (int i = 0; i < list.GetReflection()->FieldSize(list, field); ++i)
            rows.push_back(&list.GetReflection()->GetRepeatedMessage(list, field, i));
        BatchSave(rows);
    }

    void Update(const Message &message);
    void UpdateByWhere(const Message &message, const std::string &where);
    void UpdateByWhereWithArgs(const Message &message, const std::string &where,
                               const std::vector<SqlValue> &args);
    void UpdateFieldsByPrimaryKey(const Message &message, const StringVector &fields);
    void UpdateFieldsByPK(const Message &message, const StringVector &fields) {
        UpdateFieldsByPrimaryKey(message, fields);
    }
    void UpdateKVByPK(const Message &message, const std::string &field, const SqlValue &value);
    void UpdateKvByPrimaryKey(const Message &message, const std::string &field,
                              const std::string &value) {
        UpdateKVByPK(message, field, value);
    }
    bool UpdateIfVersion(const Message &message, const std::string &version_field);
    bool UpdateFieldsIfVersion(const Message &message, const std::string &version_field,
                               const StringVector &fields);
    void IncrByPrimaryKey(const Message &message, const std::string &field, int64_t delta);
    void IncrByPK(const Message &message, const std::string &field, int64_t delta) {
        IncrByPrimaryKey(message, field, delta);
    }
    bool DecrByPrimaryKeyIfEnough(const Message &message, const std::string &field, int64_t delta);
    bool DecrByPKIfEnough(const Message &message, const std::string &field, int64_t delta) {
        return DecrByPrimaryKeyIfEnough(message, field, delta);
    }
    void Delete(const Message &message);
    void DeleteByKV(const Message &message, const std::string &field, const SqlValue &value);
    void DeleteByKv(const Message &message, const std::string &field, const std::string &value) {
        DeleteByKV(message, field, value);
    }
    void DeleteByWhere(const Message &message, const std::string &where);
    void DeleteByWhereWithArgs(const Message &message, const std::string &where,
                               const std::vector<SqlValue> &args);
    void BatchDelete(const std::vector<const Message *> &messages);

    bool FindOneByPrimaryKey(Message &message);
    bool FindOneByPK(Message &message) { return FindOneByPrimaryKey(message); }
    bool FindOneByPrimaryKeyForUpdate(Message &message);
    bool FindOneByPKForUpdate(Message &message) { return FindOneByPrimaryKeyForUpdate(message); }
    bool FindOrCreate(Message &message, bool *created);
    bool FindOrCreate(Message &message) {
        bool created = false;
        FindOrCreate(message, &created);
        return created;
    }
    bool FindOneByKV(Message &message, const std::string &field, const SqlValue &value);
    bool FindOneByKv(Message &message, const std::string &field, const std::string &value) {
        return FindOneByKV(message, field, value);
    }
    bool FindOneByWhere(Message &message, const std::string &where);
    bool FindOneByWhereWithArgs(Message &message, const std::string &where,
                                const std::vector<SqlValue> &args);
    bool FindOneWithOptions(Message &message, const std::string &where,
                            const QueryOptions &options) {
        return FindOneWithOptions(message, where, {}, options);
    }
    bool FindOneWithOptions(Message &message, const std::string &where,
                            const std::vector<SqlValue> &args, const QueryOptions &options);
    struct MultiQuery {
        Message *message = nullptr;
        std::string where_clause;
        std::vector<SqlValue> where_args;
    };
    void FindMultiByWhereClauses(const std::vector<MultiQuery> &queries);

    void FindAll(Message &list);
    void FindAllByWhere(Message &list, const std::string &where) {
        FindAllByWhereWithArgs(list, where, {});
    }
    void FindAllByWhereWithArgs(Message &list, const std::string &where,
                                const std::vector<SqlValue> &args);
    void FindMultiByWhereWithArgs(Message &list, const std::string &where,
                                  const std::vector<SqlValue> &args) {
        FindAllByWhereWithArgs(list, where, args);
    }
    void FindAllByPKIn(Message &list, const std::vector<SqlValue> &values);
    void FindAllByKVIn(Message &list, const std::string &field,
                       const std::vector<SqlValue> &values);
    void FindMultiByKV(Message &list, const std::string &field, const SqlValue &value);
    void FindAllWithOptions(Message &list, const std::string &where,
                            const std::vector<SqlValue> &args, const QueryOptions &options);
    void FindAllWithOptions(Message &list, const std::string &where, const QueryOptions &options) {
        FindAllWithOptions(list, where, {}, options);
    }
    void FindPage(Message &list, const std::string &where, const std::vector<SqlValue> &args,
                  int page_index, int page_size);
    void FindPage(Message &list, const std::string &where, int page_index, int page_size) {
        FindPage(list, where, {}, page_index, page_size);
    }
    void FindPageByCursor(Message &list, const std::string &where,
                          const std::vector<SqlValue> &args, const std::string &cursor_field,
                          const std::optional<SqlValue> &cursor, int page_size);
    template <class T> void FindAll(Message &list) {
        ListField(list, T::descriptor());
        FindAll(list);
    }
    template <class T> void FindAllByWhere(Message &list, const std::string &where) {
        ListField(list, T::descriptor());
        FindAllByWhere(list, where);
    }
    template <class T>
    void FindAllByWhereWithArgs(Message &list, const std::string &where,
                                const std::vector<SqlValue> &args) {
        ListField(list, T::descriptor());
        FindAllByWhereWithArgs(list, where, args);
    }
    template <class T> void FindAllByPrimaryKeyIn(Message &list, const StringVector &values) {
        ListField(list, T::descriptor());
        FindAllByPKIn(list, {values.begin(), values.end()});
    }
    template <class T>
    void FindAllByKvIn(Message &list, const std::string &field, const StringVector &values) {
        ListField(list, T::descriptor());
        FindAllByKVIn(list, field, {values.begin(), values.end()});
    }
    template <class T>
    void FindMultiByKv(Message &list, const std::string &field, const std::string &value) {
        ListField(list, T::descriptor());
        FindMultiByKV(list, field, value);
    }
    template <class T>
    void FindAllWithOptions(Message &list, const std::string &where, const QueryOptions &options) {
        ListField(list, T::descriptor());
        FindAllWithOptions(list, where, {}, options);
    }
    template <class T>
    void FindAllWithOptions(Message &list, const std::string &where,
                            const std::vector<SqlValue> &args, const QueryOptions &options) {
        ListField(list, T::descriptor());
        FindAllWithOptions(list, where, args, options);
    }
    template <class T>
    void FindPage(Message &list, const std::string &where, int page_index, int page_size) {
        ListField(list, T::descriptor());
        FindPage(list, where, {}, page_index, page_size);
    }
    template <class T>
    void FindPageByCursor(Message &list, const std::string &where, const std::string &field,
                          const std::string &cursor, bool has_cursor, int size) {
        ListField(list, T::descriptor());
        FindPageByCursor(list, where, {}, field,
                         has_cursor ? std::optional<SqlValue>{cursor} : std::nullopt, size);
    }

    int64_t Count(const Message &message);
    int64_t CountByWhere(const Message &message, const std::string &where) {
        return CountByWhereWithArgs(message, where, {});
    }
    int64_t CountByWhereWithArgs(const Message &message, const std::string &where,
                                 const std::vector<SqlValue> &args);
    bool Exists(const Message &message, const std::string &where = {},
                const std::vector<SqlValue> &args = {});
    bool ExistsByPrimaryKey(const Message &message);
    bool ExistsByPK(const Message &message) { return ExistsByPrimaryKey(message); }
    bool Transaction(const std::function<bool(Database &)> &callback);
    bool RunInTransaction(const std::function<bool(Database &)> &callback) {
        return Transaction(callback);
    }
    bool InTransaction() const {
        std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
        return in_transaction_;
    }
    void EnableCache(Cache *cache, int ttl_seconds);
    std::string CacheKey(const Message &message);
    void InvalidateCache(const Message &message);
    void InvalidateCache(const std::vector<const Message *> &messages);
    TableMap &Tables() { return tables_; }
    const TableMap &Tables() const { return tables_; }
    TableMap &tables() { return tables_; }

  private:
    StringVector RegisterFiles(const std::vector<const google::protobuf::FileDescriptor *> &files,
                               DescriptorSet *source);
    StringVector SortedTableKeys() const;
    MessageTable *TableFor(const Message &message);
    MessageTable *ResolveListTable(const Message &list);
    MessageTable *ResolveAnyTable(const Message &message);
    static const google::protobuf::FieldDescriptor *
    ListField(const Message &list, const google::protobuf::Descriptor *expected = nullptr);
    ResultRowPtr QueryAtMostOne(const Statement &statement);
    bool LoadOne(Message &message, const Statement &statement);
    void LoadList(Message &list, const Statement &statement);
    int64_t QueryScalar(const Statement &statement, int64_t fallback);
    StringVector MigrationStatements(const Message &message);
    void RefreshSchemaMetadata(MessageTable &table);
    void SynchronizeTable(const Message &message, bool create_if_missing);
    void InvalidateMessage(MessageTable *table, const Message &message);
    void InvalidateKey(const std::string &key);
    void InvalidateShiftedPrimaryKey(MessageTable &table, const Message &message,
                                     const std::string &field, int64_t delta);
    void FlushPendingCacheDeletes();
    std::vector<std::shared_ptr<DescriptorSet>> descriptor_sets_;
    TableMap tables_;
    std::string db_name_;
    bool expand_only_ = false;
    Cache *cache_ = nullptr;
    int cache_ttl_seconds_ = 0;
    bool in_transaction_ = false;
    std::vector<std::string> pending_cache_deletes_;
    mutable std::recursive_mutex operation_mutex_;
};
using DatabasePtr = std::shared_ptr<Database>;
} // namespace pbmysql
