#include "src/mysql_wrapper/database.h"

#include <algorithm>
#include <fstream>

#include "google/protobuf/message.h"

#include "muduo/base/Logging.h"
#include "src/util/utility.h"

#include "common_proto/db_base.pb.h"

namespace pbmysql {

void Database::SetDbName(const std::string& name) {
    db_name_ = name;
    for (auto& entry : tables_) {
        entry.second.SetDbName(name);
    }
}

void Database::RegisterTable(const ::google::protobuf::Message& default_instance) {
    const std::string& key = default_instance.GetDescriptor()->full_name();
    if (tables_.find(key) != tables_.end()) {
        return;
    }
    auto inserted = tables_.emplace(key, MessageTable(default_instance));
    inserted.first->second.SetDbName(db_name_);
}

namespace {

// Recursively registers every message descriptor (including nested types) that
// declares the (OptionTableName) option, using the generated default instance.
void RegisterTablesInDescriptor(Database& db, const ::google::protobuf::Descriptor* descriptor) {
    if (descriptor->options().HasExtension(OptionTableName)) {
        const auto* prototype =
            ::google::protobuf::MessageFactory::generated_factory()->GetPrototype(descriptor);
        if (prototype != nullptr) {
            db.RegisterTable(*prototype);
        } else {
            LOG_ERROR << "no generated prototype for " << descriptor->full_name();
        }
    }
    for (int32_t i = 0; i < descriptor->nested_type_count(); ++i) {
        RegisterTablesInDescriptor(db, descriptor->nested_type(i));
    }
}

}  // namespace

void Database::RegisterAllTables(const ::google::protobuf::FileDescriptor* file) {
    if (nullptr == file) {
        return;
    }
    for (int32_t i = 0; i < file->message_type_count(); ++i) {
        RegisterTablesInDescriptor(*this, file->message_type(i));
    }
}

void Database::WriteCreateTableSql(std::ostream& out) const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& entry : tables_) {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        out << tables_.at(name).CreateTableSql() << "\n\n";
    }
}

void Database::DumpCreateTableSqlFile(const std::string& file_path) const {
    std::ofstream file(file_path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        LOG_ERROR << "cannot open schema file: " << file_path;
        return;
    }
    WriteCreateTableSql(file);
}

std::string Database::GenerateMigrationSql(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return "";
    }
    if (!IsTableExists(table->TableName())) {
        return table->CreateTableSql();
    }
    table->ClearExistingColumns();
    auto cb = std::bind(&MessageTable::OnSelectColumn, table,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    Query(table->SelectColumnsSql(), cb);
    return table->AlterTableSql();
}

void Database::WriteMigrationSql(std::ostream& out,
    const std::vector<const ::google::protobuf::Message*>& messages) {
    for (const auto* message : messages) {
        if (nullptr == message) {
            continue;
        }
        std::string stmt = GenerateMigrationSql(*message);
        if (stmt.empty()) {
            continue;
        }
        out << stmt << "\n\n";
    }
}

void Database::DumpMigrationSqlFile(const std::string& file_path,
    const std::vector<const ::google::protobuf::Message*>& messages) {
    std::ofstream file(file_path, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
        LOG_ERROR << "cannot open migration file: " << file_path;
        return;
    }
    WriteMigrationSql(file, messages);
}

MessageTable* Database::TableFor(const ::google::protobuf::Message& message) {
    const std::string& key = message.GetDescriptor()->full_name();
    auto it = tables_.find(key);
    if (it == tables_.end()) {
        LOG_ERROR << "table not registered: " << key;
        return nullptr;
    }
    return &it->second;
}

std::string Database::GetCreateTableSql(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    return table ? table->CreateTableSql() : std::string();
}

void Database::CreateOrUpdateTable(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->CreateTableSql());
    table->ClearExistingColumns();
    auto cb = std::bind(&MessageTable::OnSelectColumn, table,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    Query(table->SelectColumnsSql(), cb);
    std::string alter = table->AlterTableSql();
    if (!alter.empty()) {
        Execute(alter);
    }
}

void Database::SyncAllTables() {
    for (auto& entry : tables_) {
        CreateOrUpdateTable(entry.second.DefaultInstance());
    }
}

bool Database::IsTableExists(const std::string& table_name) {
    std::string sql = "SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = '" +
        db_name_ + "' AND TABLE_NAME = '" + table_name + "'";
    return QueryScalar(sql, 0) > 0;
}

int64_t Database::QueryScalar(const std::string& sql, int64_t default_value) {
    auto row = QueryOne(sql);
    if (nullptr == row || row->size() == 0 || (*row)[0] == nullptr) {
        return default_value;
    }
    return StringToNumber<int64_t>((*row)[0]);
}

// --- Inserts ---------------------------------------------------------------

void Database::Insert(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->InsertSql(message, connection()));
}

bool Database::InsertIgnore(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    Execute(table->InsertIgnoreSql(message, connection()));
    return AffectedRows() > 0;
}

uint64_t Database::InsertReturningId(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return 0;
    }
    Execute(table->InsertSql(message, connection()));
    return LastInsertId();
}

void Database::InsertOnDupUpdate(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->InsertOnDupUpdateSql(message, connection()));
    InvalidateMessage(table, message);
}

void Database::BatchInsert(const std::vector<const ::google::protobuf::Message*>& messages) {
    if (messages.empty()) {
        return;
    }
    // Match the Go library: chunk large batches into multi-row INSERTs.
    constexpr std::size_t kBatchInsertMaxSize = 1000;
    for (std::size_t start = 0; start < messages.size(); start += kBatchInsertMaxSize) {
        std::size_t end = std::min(start + kBatchInsertMaxSize, messages.size());
        std::vector<const ::google::protobuf::Message*> chunk(
            messages.begin() + start, messages.begin() + end);

        MessageTable* table = nullptr;
        for (const auto* message : chunk) {
            if (message != nullptr) {
                table = TableFor(*message);
                break;
            }
        }
        if (nullptr == table) {
            continue;
        }
        std::string sql = table->BatchInsertSql(chunk, connection());
        if (!sql.empty()) {
            Execute(sql);
        }
    }
}

void Database::Save(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->InsertOnDupUpdateSql(message, connection()));
    InvalidateMessage(table, message);
}

// --- Updates ---------------------------------------------------------------

void Database::Update(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->UpdateSql(message, connection()));
    InvalidateMessage(table, message);
}

void Database::UpdateByWhere(const ::google::protobuf::Message& message, const std::string& where_clause) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    std::string sql = table->UpdateByWhereSql(message, connection(), where_clause);
    if (!sql.empty()) {
        Execute(sql);
    }
}

void Database::UpdateKvByPrimaryKey(const ::google::protobuf::Message& message,
    const std::string& field, const std::string& value) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->UpdateKvByPrimaryKeySql(message, connection(), field, value));
    InvalidateMessage(table, message);
}

void Database::UpdateFieldsByPrimaryKey(const ::google::protobuf::Message& message, const StringVector& fields) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    std::string sql = table->UpdateFieldsByPrimaryKeySql(message, connection(), fields);
    if (sql.empty()) {
        LOG_ERROR << "UpdateFieldsByPrimaryKey: empty SQL (unknown field?) for " << table->TableName();
        return;
    }
    Execute(sql);
    InvalidateMessage(table, message);
}

bool Database::UpdateIfVersion(const ::google::protobuf::Message& message, const std::string& version_field) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    std::string sql = table->UpdateIfVersionSql(message, connection(), version_field);
    if (sql.empty()) {
        LOG_ERROR << "UpdateIfVersion: empty SQL for " << table->TableName();
        return false;
    }
    Execute(sql);
    bool updated = AffectedRows() > 0;
    if (updated) {
        InvalidateMessage(table, message);
    }
    return updated;
}

bool Database::UpdateFieldsIfVersion(const ::google::protobuf::Message& message,
    const std::string& version_field, const StringVector& fields) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    std::string sql = table->UpdateFieldsIfVersionSql(message, connection(), version_field, fields);
    if (sql.empty()) {
        LOG_ERROR << "UpdateFieldsIfVersion: empty SQL for " << table->TableName();
        return false;
    }
    Execute(sql);
    bool updated = AffectedRows() > 0;
    if (updated) {
        InvalidateMessage(table, message);
    }
    return updated;
}

void Database::IncrByPrimaryKey(const ::google::protobuf::Message& message,
    const std::string& field, int64_t delta) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    std::string sql = table->IncrByPrimaryKeySql(message, connection(), field, delta);
    if (sql.empty()) {
        LOG_ERROR << "IncrByPrimaryKey: unknown field " << field << " in " << table->TableName();
        return;
    }
    Execute(sql);
    InvalidateMessage(table, message);
}

bool Database::DecrByPrimaryKeyIfEnough(const ::google::protobuf::Message& message,
    const std::string& field, int64_t delta) {
    if (delta < 0) {
        LOG_ERROR << "DecrByPrimaryKeyIfEnough: delta must be non-negative, got " << delta;
        return false;
    }
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    std::string sql = table->DecrByPrimaryKeyIfEnoughSql(message, connection(), field, delta);
    if (sql.empty()) {
        LOG_ERROR << "DecrByPrimaryKeyIfEnough: unknown field " << field << " in " << table->TableName();
        return false;
    }
    Execute(sql);
    bool updated = AffectedRows() > 0;
    if (updated) {
        InvalidateMessage(table, message);
    }
    return updated;
}

// --- Deletes ---------------------------------------------------------------

void Database::Delete(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->DeleteByPrimaryKeySql(message, connection()));
    InvalidateMessage(table, message);
}

void Database::DeleteByKv(const ::google::protobuf::Message& message,
    const std::string& key, const std::string& value) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->DeleteByKvSql(key, value, connection()));
}

void Database::DeleteByWhere(const ::google::protobuf::Message& message, const std::string& where_clause) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return;
    }
    Execute(table->DeleteByWhereSql(where_clause));
}

void Database::BatchDelete(const std::vector<const ::google::protobuf::Message*>& messages) {
    for (const auto* message : messages) {
        if (message != nullptr) {
            Delete(*message);
        }
    }
}

// --- Single-row queries ----------------------------------------------------

bool Database::FindOneByPrimaryKey(::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }

    // Cache lookup (only outside transactions, mirroring the Go library).
    if (cache_ != nullptr && !in_transaction_) {
        std::string cached;
        if (cache_->Get(table->CacheKey(message, connection()), &cached)) {
            if (message.ParseFromString(cached)) {
                return true;
            }
        }
    }

    auto row = QueryOne(table->SelectByPrimaryKeySql(message, connection()));
    if (nullptr == row) {
        return false;
    }
    ParseFromRow(message, *row);

    if (cache_ != nullptr && !in_transaction_) {
        cache_->Set(table->CacheKey(message, connection()), message.SerializeAsString(), cache_ttl_seconds_);
    }
    return true;
}

bool Database::FindOneByPrimaryKeyForUpdate(::google::protobuf::Message& message) {
    if (!in_transaction_) {
        LOG_ERROR << "FindOneByPrimaryKeyForUpdate must be called inside Transaction()";
        return false;
    }
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    auto row = QueryOne(table->SelectByPrimaryKeyForUpdateSql(message, connection()));
    if (nullptr == row) {
        return false;
    }
    ParseFromRow(message, *row);
    return true;
}

bool Database::FindOrCreate(::google::protobuf::Message& message, bool* created) {
    if (created != nullptr) {
        *created = false;
    }
    if (FindOneByPrimaryKey(message)) {
        return true;
    }
    Insert(message);
    if (created != nullptr) {
        *created = true;
    }
    return true;
}

bool Database::FindOneWithOptions(::google::protobuf::Message& message,
    const std::string& where_clause, const QueryOptions& opts) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    auto row = QueryOne(table->SelectOneWithOptionsSql(where_clause, opts));
    if (nullptr == row) {
        return false;
    }
    ParseFromRow(message, *row);
    return true;
}

void Database::FindMultiByWhereClauses(const std::vector<MultiQuery>& queries) {
    for (const auto& query : queries) {
        if (nullptr == query.message) {
            continue;
        }
        FindOneByWhere(*query.message, query.where_clause);
    }
}

    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    auto row = QueryOne(table->SelectByKvSql(key, value));
    if (nullptr == row) {
        return false;
    }
    ParseFromRow(message, *row);
    return true;
}

bool Database::FindOneByWhere(::google::protobuf::Message& message, const std::string& where_clause) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    auto row = QueryOne(table->SelectByWhereSql(where_clause));
    if (nullptr == row) {
        return false;
    }
    ParseFromRow(message, *row);
    return true;
}

// --- Aggregates ------------------------------------------------------------

int64_t Database::Count(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    return table ? QueryScalar(table->CountSql(), 0) : 0;
}

int64_t Database::CountByWhere(const ::google::protobuf::Message& message, const std::string& where_clause) {
    MessageTable* table = TableFor(message);
    return table ? QueryScalar(table->CountByWhereSql(where_clause), 0) : 0;
}

bool Database::Exists(const ::google::protobuf::Message& message, const std::string& where_clause) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    return QueryOne(table->ExistsByWhereSql(where_clause)) != nullptr;
}

bool Database::ExistsByPrimaryKey(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (nullptr == table) {
        return false;
    }
    return QueryOne(table->ExistsByPrimaryKeySql(message, connection())) != nullptr;
}

// --- Transactions ----------------------------------------------------------

bool Database::Transaction(const std::function<bool(Database&)>& fn) {
    Execute("START TRANSACTION");
    in_transaction_ = true;
    pending_cache_deletes_.clear();

    bool committed = false;
    try {
        if (fn(*this)) {
            Execute("COMMIT");
            committed = true;
        } else {
            Execute("ROLLBACK");
        }
    } catch (...) {
        Execute("ROLLBACK");
        in_transaction_ = false;
        pending_cache_deletes_.clear();
        throw;
    }

    in_transaction_ = false;
    if (committed) {
        FlushPendingCacheDeletes();
    } else {
        pending_cache_deletes_.clear();
    }
    return committed;
}

// --- Cache-aside -----------------------------------------------------------

void Database::EnableCache(Cache* cache, int ttl_seconds) {
    cache_ = cache;
    cache_ttl_seconds_ = ttl_seconds;
}

std::string Database::CacheKey(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    return table ? table->CacheKey(message, connection()) : std::string();
}

void Database::InvalidateCache(const ::google::protobuf::Message& message) {
    MessageTable* table = TableFor(message);
    if (table != nullptr) {
        InvalidateMessage(table, message);
    }
}

void Database::InvalidateMessage(MessageTable* table, const ::google::protobuf::Message& message) {
    if (cache_ == nullptr) {
        return;
    }
    std::string key = table->CacheKey(message, connection());
    if (in_transaction_) {
        pending_cache_deletes_.push_back(key);
    } else {
        cache_->Del(key);
    }
}

void Database::FlushPendingCacheDeletes() {
    if (cache_ == nullptr) {
        pending_cache_deletes_.clear();
        return;
    }
    for (const auto& key : pending_cache_deletes_) {
        cache_->Del(key);
    }
    pending_cache_deletes_.clear();
}

}  // namespace pbmysql
