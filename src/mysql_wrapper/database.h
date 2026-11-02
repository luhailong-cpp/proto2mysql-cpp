#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/reflection.h"

#include "src/mysql_wrapper/cache.h"
#include "src/mysql_wrapper/message_table.h"
#include "src/mysql_wrapper/mysql_client.h"
#include "src/mysql_wrapper/sql_builder.h"

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

// Database is the primary entry point of the library. It maps protobuf messages
// to MySQL tables and exposes CRUD, schema-management, transaction and
// cache-aside helpers. The method surface mirrors the *DB type of the Go
// proto2mysql library so both codebases stay in sync.
class Database : public MysqlClient {
  public:
    using TableMap = std::unordered_map<std::string, MessageTable>;

    // Schema / registration --------------------------------------------------
    void SetDbName(const std::string &name);
    const std::string &DbName() const { return db_name_; }

    // Registers the mapping between a protobuf message and its table. The
    // registry key is always the protobuf full name.
    void RegisterTable(const ::google::protobuf::Message &default_instance,
                       const TableOptions &options = {});

    // Registers every message in a .proto file (recursing into nested types)
    // that declares the (OptionTableName) option. Mirrors Go RegisterAllTables.
    void RegisterAllTables(const ::google::protobuf::FileDescriptor *file);

    // Creates the table if missing, then adds any newly declared columns.
    void CreateOrUpdateTable(const ::google::protobuf::Message &message);

    // Creates / aligns every registered table. Replaces the old Init().
    void SyncAllTables();

    // Kept for backwards compatibility; equivalent to SyncAllTables().
    void Init() { SyncAllTables(); }

    bool IsTableExists(const std::string &table_name);

    std::string GetCreateTableSql(const ::google::protobuf::Message &message);

    // Offline SQL generation -------------------------------------------------
    // Writes CREATE TABLE statements for every registered table, ordered by
    // table name, to out. No DB connection required. Mirrors Go
    // WriteCreateTableSQL.
    void WriteCreateTableSql(std::ostream &out) const;
    // Same as WriteCreateTableSql but writes to a file path (overwrites).
    void DumpCreateTableSqlFile(const std::string &file_path) const;
    // Builds the migration SQL that aligns the live table to message's proto
    // definition: CREATE TABLE when the table is missing, ALTER TABLE when it
    // differs, or "" when already aligned. Requires a live DB connection (reads
    // information_schema). Mirrors Go GenerateMigrationSQL.
    std::string GenerateMigrationSql(const ::google::protobuf::Message &message);
    // Emits GenerateMigrationSql for each message (skipping empty results).
    void WriteMigrationSql(std::ostream &out,
                           const std::vector<const ::google::protobuf::Message *> &messages);
    void DumpMigrationSqlFile(const std::string &file_path,
                              const std::vector<const ::google::protobuf::Message *> &messages);

    // Inserts ----------------------------------------------------------------
    void Insert(const ::google::protobuf::Message &message);
    // Returns true when a new row was inserted, false when ignored (duplicate).
    bool InsertIgnore(const ::google::protobuf::Message &message);
    // Returns the AUTO_INCREMENT id produced by the insert.
    uint64_t InsertReturningId(const ::google::protobuf::Message &message);
    void InsertOnDupUpdate(const ::google::protobuf::Message &message);
    void BatchInsert(const std::vector<const ::google::protobuf::Message *> &messages);

    // Insert-or-update on the primary key.
    void Save(const ::google::protobuf::Message &message);
    ConditionalSaveResult SaveConditional(const ::google::protobuf::Message &message,
                                          const SaveOptions &options = {});
    void BatchSave(const std::vector<const ::google::protobuf::Message *> &messages);
    SQLBuilder Builder(const ::google::protobuf::Message &message);
    template <typename T> void BatchSave(const ::google::protobuf::Message &list_message) {
        ForEachRepeated<T>(list_message, [this](const T &item) { Save(item); });
    }

    // Updates ----------------------------------------------------------------
    void Update(const ::google::protobuf::Message &message);
    void UpdateByWhere(const ::google::protobuf::Message &message, const std::string &where_clause);
    void UpdateByWhereWithArgs(const ::google::protobuf::Message &message,
                               const std::string &where_clause, const std::vector<SqlValue> &args);
    void UpdateKvByPrimaryKey(const ::google::protobuf::Message &message, const std::string &field,
                              const std::string &value);
    // Updates only the named fields, matched by primary key.
    void UpdateFieldsByPrimaryKey(const ::google::protobuf::Message &message,
                                  const StringVector &fields);
    // Optimistic-lock update: sets the message's populated fields and bumps the
    // version column only when the stored version matches. Returns true on a
    // successful CAS (a row was updated), false on a version conflict.
    bool UpdateIfVersion(const ::google::protobuf::Message &message,
                         const std::string &version_field);
    // Optimistic-lock update restricted to an explicit field list (avoids
    // proto3 implicit-presence skipping of zero-value fields).
    bool UpdateFieldsIfVersion(const ::google::protobuf::Message &message,
                               const std::string &version_field, const StringVector &fields);
    // Atomically adds delta to a numeric field, matched by primary key.
    void IncrByPrimaryKey(const ::google::protobuf::Message &message, const std::string &field,
                          int64_t delta);
    // Atomically subtracts delta from a numeric field only when field >= delta.
    // delta must be non-negative. Returns true when a row was updated.
    bool DecrByPrimaryKeyIfEnough(const ::google::protobuf::Message &message,
                                  const std::string &field, int64_t delta);

    // Deletes ----------------------------------------------------------------
    void Delete(const ::google::protobuf::Message &message);
    void DeleteByKv(const ::google::protobuf::Message &message, const std::string &key,
                    const std::string &value);
    void DeleteByWhere(const ::google::protobuf::Message &message, const std::string &where_clause);
    void DeleteByWhereWithArgs(const ::google::protobuf::Message &message,
                               const std::string &where_clause, const std::vector<SqlValue> &args);
    void BatchDelete(const std::vector<const ::google::protobuf::Message *> &messages);

    // Single-row queries -----------------------------------------------------
    // Fills message by its primary key value(s). Uses the cache when enabled.
    bool FindOneByPrimaryKey(::google::protobuf::Message &message);
    // Like FindOneByPrimaryKey but takes a row lock (SELECT ... FOR UPDATE).
    // Only valid inside Transaction(); throws otherwise.
    bool FindOneByPrimaryKeyForUpdate(::google::protobuf::Message &message);
    // Finds by primary key; inserts message when absent. Returns whether a new
    // row was created (out param), and true when the row now exists.
    bool FindOrCreate(::google::protobuf::Message &message, bool *created);
    bool FindOneByKv(::google::protobuf::Message &message, const std::string &key,
                     const std::string &value);
    bool FindOneByWhere(::google::protobuf::Message &message, const std::string &where_clause);
    bool FindOneByWhereWithArgs(::google::protobuf::Message &message,
                                const std::string &where_clause, const std::vector<SqlValue> &args);
    // Finds a single row honouring ORDER BY/LIMIT (LIMIT forced to 1).
    bool FindOneWithOptions(::google::protobuf::Message &message, const std::string &where_clause,
                            const QueryOptions &opts);

    // One entry of a FindMultiByWhereClauses batch: fills message using
    // where_clause. message is an out parameter.
    struct MultiQuery {
        ::google::protobuf::Message *message = nullptr;
        std::string where_clause;
    };
    // Fills several messages, each from its own table and WHERE clause, in one
    // call. Mirrors Go FindMultiByWhereClauses (executed sequentially here).
    void FindMultiByWhereClauses(const std::vector<MultiQuery> &queries);

    // Multi-row queries. The list message must contain exactly one repeated
    // field whose element type is T.
    template <typename T> void FindAll(::google::protobuf::Message &list_message) {
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        LoadRepeatedBySql<T>(list_message, SQLBuilder(*table).SelectWhere("", {}));
    }
    template <typename T>
    void FindAllByWhere(::google::protobuf::Message &list_message,
                        const std::string &where_clause) {
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        LoadRepeatedBySql<T>(list_message, SQLBuilder(*table).SelectWhere(where_clause, {}));
    }
    template <typename T>
    void FindAllByWhereWithArgs(::google::protobuf::Message &list_message,
                                const std::string &where_clause,
                                const std::vector<SqlValue> &args) {
        T scratch;
        LoadRepeatedBySql<T>(list_message, Builder(scratch).SelectWhere(where_clause, args));
    }
    // Batch primary-key lookup (WHERE pk IN (...)). Missing keys are skipped.
    template <typename T>
    void FindAllByPrimaryKeyIn(::google::protobuf::Message &list_message,
                               const StringVector &pk_values) {
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        if (pk_values.empty()) {
            ClearRepeated<T>(list_message);
            return;
        }
        std::vector<SqlValue> values(pk_values.begin(), pk_values.end());
        LoadRepeatedBySql<T>(list_message, SQLBuilder(*table).SelectByPKIn(values));
    }
    // Batch lookup by an arbitrary key (WHERE key IN (...)).
    template <typename T>
    void FindAllByKvIn(::google::protobuf::Message &list_message, const std::string &key,
                       const StringVector &values) {
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        if (values.empty()) {
            ClearRepeated<T>(list_message);
            return;
        }
        std::vector<SqlValue> args(values.begin(), values.end());
        LoadRepeatedBySql<T>(list_message, SQLBuilder(*table).SelectByKVIn(key, args));
    }
    // Equality lookup returning every matching row (WHERE key = value).
    template <typename T>
    void FindMultiByKv(::google::protobuf::Message &list_message, const std::string &key,
                       const std::string &value) {
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        if (!table->HasField(key))
            throw std::invalid_argument("unknown field: " + key);
        LoadRepeatedBySql<T>(list_message,
                             SQLBuilder(*table).SelectWhere("`" + key + "` = ?", {value}));
    }
    // List query with ORDER BY / LIMIT / OFFSET modifiers.
    template <typename T>
    void FindAllWithOptions(::google::protobuf::Message &list_message,
                            const std::string &where_clause, const QueryOptions &opts) {
        if (opts.for_update && !in_transaction_)
            throw std::logic_error("FOR UPDATE requires Transaction()");
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        LoadRepeatedBySql<T>(list_message, SQLBuilder(*table).SelectWhere(where_clause, {}, opts));
    }
    // Offset pagination (page_index is 1-based).
    template <typename T>
    void FindPage(::google::protobuf::Message &list_message, const std::string &where_clause,
                  int page_index, int page_size) {
        if (page_index < 1 || page_size < 1) {
            throw std::invalid_argument("page index and size must be positive");
        }
        QueryOptions opts;
        opts.limit = page_size;
        if (static_cast<int64_t>(page_index - 1) * page_size > INT32_MAX)
            throw std::invalid_argument("page offset overflow");
        opts.offset = (page_index - 1) * page_size;
        FindAllWithOptions<T>(list_message, where_clause, opts);
    }
    // Keyset (cursor) pagination: returns up to page_size rows whose
    // cursor_field is greater than cursor_value, ordered ascending. Pass an
    // empty cursor_value for the first page.
    template <typename T>
    void FindPageByCursor(::google::protobuf::Message &list_message,
                          const std::string &where_clause, const std::string &cursor_field,
                          const std::string &cursor_value, bool has_cursor, int page_size) {
        if (page_size < 1) {
            throw std::invalid_argument("page size must be positive");
        }
        T scratch;
        MessageTable *table = TableFor(scratch);
        if (nullptr == table) {
            return;
        }
        if (!table->HasField(cursor_field)) {
            throw std::invalid_argument("unknown cursor field: " + cursor_field);
        }
        std::string where = where_clause.empty() ? std::string("1=1") : where_clause;
        std::vector<SqlValue> args;
        if (has_cursor) {
            where = "(" + where + ") AND `" + cursor_field + "` > ?";
            args.push_back(cursor_value);
        }
        QueryOptions opts;
        opts.order_by = "`" + cursor_field + "` ASC";
        opts.limit = page_size;
        LoadRepeatedBySql<T>(list_message, SQLBuilder(*table).SelectWhere(where, args, opts));
    }

    // Aggregates -------------------------------------------------------------
    int64_t Count(const ::google::protobuf::Message &message);
    int64_t CountByWhere(const ::google::protobuf::Message &message,
                         const std::string &where_clause);
    bool Exists(const ::google::protobuf::Message &message, const std::string &where_clause);
    bool ExistsByPrimaryKey(const ::google::protobuf::Message &message);

    // Transactions -----------------------------------------------------------
    // Runs fn inside START TRANSACTION / COMMIT. Returning false (or throwing)
    // rolls back. Deferred cache invalidations are flushed only on commit.
    bool Transaction(const std::function<bool(Database &)> &fn);

    // Cache-aside ------------------------------------------------------------
    void EnableCache(Cache *cache, int ttl_seconds);
    std::string CacheKey(const ::google::protobuf::Message &message);
    void InvalidateCache(const ::google::protobuf::Message &message);

    TableMap &tables() { return tables_; }

  private:
    MessageTable *TableFor(const ::google::protobuf::Message &message);

    // Reads a single scalar (row[0]) produced by a COUNT / EXISTS query.
    int64_t QueryScalar(const Statement &sql, int64_t default_value);

    // Deletes (or defers, inside a transaction) the cache entry for message.
    void InvalidateMessage(MessageTable *table, const ::google::protobuf::Message &message);
    void FlushPendingCacheDeletes();

    template <typename T>
    const ::google::protobuf::FieldDescriptor *
    ListField(const ::google::protobuf::Message &message) const {
        const auto *desc = message.GetDescriptor();
        if (desc->field_count() != 1 || !desc->field(0)->is_repeated() ||
            desc->field(0)->is_map() || desc->field(0)->message_type() != T::descriptor())
            throw std::invalid_argument(
                "list message must have exactly one repeated field of the requested type");
        return desc->field(0);
    }

    template <typename T>
    void ForEachRepeated(const ::google::protobuf::Message &list_message,
                         const std::function<void(const T &)> &fn) {
        const auto *descriptor = list_message.GetDescriptor();
        const auto *reflection = list_message.GetReflection();
        const auto *field = ListField<T>(list_message);
        auto repeated = reflection->GetRepeatedFieldRef<T>(list_message, field);
        for (auto it = repeated.begin(); it != repeated.end(); ++it) {
            fn(*it);
        }
    }

    template <typename T> void ClearRepeated(::google::protobuf::Message &list_message) {
        const auto *descriptor = list_message.GetDescriptor();
        const auto *reflection = list_message.GetReflection();
        const auto *field = ListField<T>(list_message);
        reflection->ClearField(&list_message, field);
    }

    template <typename T>
    void LoadRepeatedBySql(::google::protobuf::Message &list_message, const Statement &sql) {
        if (sql.sql.empty()) {
            ClearRepeated<T>(list_message);
            return;
        }
        const auto *descriptor = list_message.GetDescriptor();
        const auto *reflection = list_message.GetReflection();
        const auto *field = ListField<T>(list_message);
        auto repeated = reflection->GetMutableRepeatedFieldRef<T>(&list_message, field);
        repeated.Clear();

        T scratch;
        auto fill = [&repeated, &scratch](const ResultRowPtr &row) -> bool {
            scratch.Clear();
            ParseFromRow(scratch, *row);
            repeated.Add(scratch);
            return true;
        };
        QueryResultRowProcessor(sql, fill);
    }

    TableMap tables_;
    std::string db_name_;

    Cache *cache_{nullptr};
    int cache_ttl_seconds_{0};

    bool in_transaction_{false};
    std::vector<std::string> pending_cache_deletes_;
};

using DatabasePtr = std::shared_ptr<Database>;

} // namespace pbmysql
