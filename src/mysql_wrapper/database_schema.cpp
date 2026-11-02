#include "db_base.pb.h"
#include "src/mysql_wrapper/database.h"
#include "src/mysql_wrapper/identifier_utils.h"
#include "src/mysql_wrapper/text_encoding.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mysqld_error.h>
#include <set>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pbmysql {
namespace {
std::string Cell(const ResultRow &row, size_t index) {
    if (index >= row.size() || !row[index])
        throw std::runtime_error("missing or NULL database metadata");
    return std::string(row[index], row.length(index));
}
StringVector ValidateMappings(const Database::TableMap &tables) {
    StringVector keys;
    std::set<std::string> names;
    for (const auto &entry : tables) {
        if (!names.insert(FoldIdentifier(entry.second.TableName())).second)
            throw std::invalid_argument("multiple messages map to the same physical table: " +
                                        entry.second.TableName());
        entry.second.CreateTableSql();
        keys.push_back(entry.first);
    }
    std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
        const auto &left = tables.at(a).TableName();
        const auto &right = tables.at(b).TableName();
        return left == right ? a < b : left < right;
    });
    return keys;
}
inline constexpr std::int64_t kSchemaLockWaitSeconds = 30;
enum class AdvisoryLockResult : std::int64_t { TimedOut, Acquired };
enum class LockReleaseResult : std::int64_t { NotHeld, Released };
inline constexpr std::int64_t kMissingLockResult = -1;
enum DatabaseMetadataColumn : std::size_t { DatabaseName, CaseInsensitiveTableNames };
std::string SyncLockName(const std::string &database) {
    return "proto2mysql:sync:" + Fnv1a32Hex(database);
}
bool UnsupportedAdvisoryLock(const MysqlError &error) {
    if (error.value() != ER_UNKNOWN_ERROR && error.value() != ER_NOT_SUPPORTED_YET &&
        error.value() != ER_SP_DOES_NOT_EXIST)
        return false;
    auto message = error.message();
    std::transform(message.begin(), message.end(), message.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return message.find("get_lock") != std::string::npos &&
           (message.find("does not exist") != std::string::npos ||
            message.find("doesn't exist") != std::string::npos ||
            message.find("not support") != std::string::npos ||
            message.find("unsupported") != std::string::npos);
}
void WriteFileAtomically(const std::string &name, const std::string &content) {
    static std::atomic<uint64_t> sequence{0};
    const auto destination = std::filesystem::u8path(name);
    auto temporary = destination;
    temporary += ".p2m-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence++) + ".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << content))
            throw std::runtime_error("cannot write SQL file: " + name);
        output.close();
        if (!output)
            throw std::runtime_error("cannot close SQL file: " + name);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "cannot replace SQL file");
#else
        std::filesystem::rename(temporary, destination);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace

void Database::Connect(const std::string &host, const std::string &user,
                       const std::string &password, const std::string &database,
                       unsigned int port) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot connect during a transaction");
    MysqlClient::Connect(host, user, password, database, port);
    try {
        SetDbName(database);
    } catch (...) {
        Disconnect();
        throw;
    }
}
void Database::Connect(const std::string &host, const std::string &user,
                       const std::string &password, const std::string &database, unsigned int port,
                       const ConnectionOptions &options) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot connect during a transaction");
    MysqlClient::Connect(host, user, password, database, port, options);
    try {
        SetDbName(database);
    } catch (...) {
        Disconnect();
        throw;
    }
}
void Database::Close() {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot close database during a transaction");
    Disconnect();
    pending_cache_deletes_.clear();
}
void Database::SetDbName(const std::string &name) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot change database during a transaction");
    std::string canonical = name;
    if (Connected()) {
        auto row = QueryAtMostOne({"SELECT DATABASE(), @@lower_case_table_names", {}});
        if (!row)
            throw std::runtime_error("cannot determine connected database");
        const auto current = Cell(*row, DatabaseName);
        const bool case_insensitive = Cell(*row, CaseInsensitiveTableNames) != "0";
        if (case_insensitive ? FoldIdentifier(current) != FoldIdentifier(name) : current != name)
            throw std::invalid_argument("connected database differs from requested database; "
                                        "choose the database when connecting");
        canonical = current;
    }
    db_name_ = canonical;
    for (auto &entry : tables_)
        entry.second.SetDbName(db_name_);
}
void Database::SetExpandOnly(bool enabled) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    expand_only_ = enabled;
    for (auto &entry : tables_)
        entry.second.SetExpandOnly(enabled);
}
void Database::RegisterTable(const Message &prototype, const TableOptions &options) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot register tables during a transaction");
    MessageTable table(prototype, options);
    for (const auto &source : descriptor_sets_)
        if (!source->Files().empty() &&
            source->Files().front()->pool() == prototype.GetDescriptor()->file()->pool())
            table.SetDescriptorOwner(source);
    table.SetDbName(db_name_);
    table.SetExpandOnly(expand_only_);
    auto candidate = tables_;
    candidate.insert_or_assign(std::string(prototype.GetDescriptor()->full_name()),
                               std::move(table));
    ValidateMappings(candidate);
    tables_ = std::move(candidate);
}
StringVector
Database::RegisterFiles(const std::vector<const google::protobuf::FileDescriptor *> &roots,
                        DescriptorSet *source) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("cannot register tables during a transaction");
    auto candidate = tables_;
    std::set<const google::protobuf::FileDescriptor *> visited;
    StringVector registered;
    std::function<void(const google::protobuf::Descriptor *, bool)> message;
    message = [&](const auto *descriptor, bool db_file) {
        const bool modern = db_file && TableNameFromDescriptor(descriptor).has_value();
        const bool legacy = descriptor->options().HasExtension(OptionTableName);
        if (!descriptor->options().map_entry() && (modern || legacy)) {
            const auto *prototype =
                source ? source->Prototype(descriptor)
                       : google::protobuf::MessageFactory::generated_factory()->GetPrototype(
                             descriptor);
            if (!prototype)
                throw std::invalid_argument(
                    "dynamic descriptors require a shared DescriptorSet source");
            MessageTable table(*prototype);
            for (const auto &retained : descriptor_sets_)
                if (retained.get() == source)
                    table.SetDescriptorOwner(retained);
            table.SetDbName(db_name_);
            table.SetExpandOnly(expand_only_);
            std::string key(descriptor->full_name());
            candidate.insert_or_assign(key, std::move(table));
            registered.push_back(key);
        }
        for (int i = 0; i < descriptor->nested_type_count(); ++i)
            message(descriptor->nested_type(i), db_file);
    };
    std::function<void(const google::protobuf::FileDescriptor *)> file;
    file = [&](const auto *current) {
        if (!current)
            throw std::invalid_argument("null file descriptor");
        if (!visited.insert(current).second)
            return;
        const bool db_file = FileHasDBOption(current);
        for (int i = 0; i < current->message_type_count(); ++i)
            message(current->message_type(i), db_file);
        for (int i = 0; i < current->dependency_count(); ++i)
            file(current->dependency(i));
    };
    for (const auto *root : roots)
        file(root);
    ValidateMappings(candidate);
    tables_ = std::move(candidate);
    std::sort(registered.begin(), registered.end());
    return registered;
}
StringVector Database::RegisterAllTables() {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return RegisterFiles(RegisteredProtoFiles(), nullptr);
}
StringVector Database::RegisterAllTables(const google::protobuf::FileDescriptor *file) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return RegisterFiles({file}, nullptr);
}
StringVector
Database::RegisterAllTables(const std::vector<const google::protobuf::FileDescriptor *> &files) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return RegisterFiles(files, nullptr);
}
StringVector Database::RegisterAllTables(const std::shared_ptr<DescriptorSet> &source) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (!source)
        throw std::invalid_argument("null descriptor source");
    const bool retained = std::find(descriptor_sets_.begin(), descriptor_sets_.end(), source) !=
                          descriptor_sets_.end();
    if (!retained)
        descriptor_sets_.push_back(source);
    try {
        return RegisterFiles(source->Files(), source.get());
    } catch (...) {
        if (!retained)
            descriptor_sets_.pop_back();
        throw;
    }
}
StringVector Database::RegisterDescriptorSet(const std::string &path) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return RegisterAllTables(DescriptorSet::Load(path));
}
std::unique_ptr<Database::Message> Database::NewMessage(const std::string &name) const {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto found = tables_.find(name);
    if (found == tables_.end())
        throw std::invalid_argument("table not registered: " + name);
    return std::unique_ptr<Message>(found->second.DefaultInstance().New());
}
StringVector Database::SortedTableKeys() const {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return ValidateMappings(tables_);
}
MessageTable *Database::TableFor(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    auto found = tables_.find(std::string(message.GetDescriptor()->full_name()));
    if (found == tables_.end())
        throw std::invalid_argument("table not registered: " +
                                    std::string(message.GetDescriptor()->full_name()));
    if (found->second.Descriptor() != message.GetDescriptor())
        throw std::invalid_argument("message descriptor does not match registered table");
    return &found->second;
}
SQLBuilder Database::Builder(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return SQLBuilder(*TableFor(message));
}
std::string Database::GetCreateTableSql(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return TableFor(message)->CreateTableSql();
}
void Database::WriteCreateTableSql(std::ostream &output) const {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto keys = SortedTableKeys();
    std::ostringstream buffer;
    for (const auto &key : keys)
        buffer << tables_.at(key).CreateTableSql() << "\n\n";
    output << buffer.str();
    if (!output)
        throw std::runtime_error("failed writing schema SQL");
}
void Database::DumpCreateTableSqlFile(const std::string &path) const {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    std::ostringstream output;
    WriteCreateTableSql(output);
    WriteFileAtomically(path, output.str());
}
bool Database::IsTableExists(const std::string &name) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    return QueryScalar({"SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA = ? AND "
                        "TABLE_NAME = ?",
                        {db_name_, name}},
                       0) > 0;
}
void Database::RefreshSchemaMetadata(MessageTable &table) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    table.ClearExistingColumns();
    Query(table.SelectColumnsSql(),
          [&](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table.OnSelectColumn(row, lengths, size);
          });
    table.ClearExistingIndexes();
    Query(table.SelectIndexesSql(),
          [&](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table.OnSelectIndex(row, lengths, size);
          });
    table.ClearTableMetadata();
    Query(table.SelectTableMetadataSql(),
          [&](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table.OnSelectTableMetadata(row, lengths, size);
          });
    table.ClearDependencies();
    Query(table.SelectForeignKeysSql(),
          [&](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table.OnSelectForeignKey(row, lengths, size);
          });
    Query(table.SelectTriggersSql(),
          [&](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t size) {
              return table.OnSelectTrigger(row, lengths, size);
          });
}
StringVector Database::MigrationStatements(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    SortedTableKeys();
    auto *table = TableFor(message);
    if (!IsTableExists(table->TableName()))
        return {table->CreateTableSql()};
    RefreshSchemaMetadata(*table);
    return table->BuildMigrationStatements();
}
std::string Database::GenerateMigrationSql(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto statements = MigrationStatements(message);
    std::string output;
    for (const auto &statement : statements)
        if (!statement.empty()) {
            if (!output.empty())
                output += '\n';
            output += statement;
            if (output.back() != ';')
                output += ';';
        }
    return output;
}
void Database::WriteMigrationSql(std::ostream &output,
                                 const std::vector<const Message *> &messages) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    SortedTableKeys();
    std::set<std::string> seen;
    for (const auto *message : messages) {
        if (!message)
            throw std::invalid_argument("null migration message");
        auto *table = TableFor(*message);
        if (!seen.insert(FoldIdentifier(table->TableName())).second)
            throw std::invalid_argument("duplicate migration table: " + table->TableName());
    }
    std::ostringstream buffer;
    for (const auto *message : messages) {
        const auto sql = GenerateMigrationSql(*message);
        if (!sql.empty())
            buffer << sql << "\n\n";
    }
    output << buffer.str();
    if (!output)
        throw std::runtime_error("failed writing migration SQL");
}
void Database::DumpMigrationSqlFile(const std::string &path,
                                    const std::vector<const Message *> &messages) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    std::ostringstream output;
    WriteMigrationSql(output, messages);
    WriteFileAtomically(path, output.str());
}
void Database::SynchronizeTable(const Message &message, bool create_if_missing) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    if (in_transaction_)
        throw std::logic_error("schema DDL implicitly commits transactions");
    SortedTableKeys();
    TableFor(message);
    const auto lock = SyncLockName(db_name_);
    bool held = false;
    try {
        const auto acquired = QueryScalar({"SELECT GET_LOCK(?, ?)", {lock, kSchemaLockWaitSeconds}},
                                          kMissingLockResult);
        if (acquired == static_cast<std::int64_t>(AdvisoryLockResult::Acquired))
            held = true;
        else {
            if (acquired != static_cast<std::int64_t>(AdvisoryLockResult::TimedOut))
                Disconnect();
            throw std::runtime_error("schema lock unavailable");
        }
    } catch (const MysqlError &error) {
        if (!UnsupportedAdvisoryLock(error)) {
            Disconnect();
            throw;
        }
        std::clog << "proto2mysql: backend does not support GET_LOCK; schema sync requires "
                     "external serialization\n";
    } catch (...) {
        Disconnect();
        throw;
    }
    auto release = [&] {
        if (!held)
            return;
        try {
            if (QueryScalar({"SELECT RELEASE_LOCK(?)", {lock}},
                            static_cast<std::int64_t>(LockReleaseResult::NotHeld)) !=
                static_cast<std::int64_t>(LockReleaseResult::Released))
                throw std::runtime_error("schema lock release was not confirmed");
        } catch (...) {
            Disconnect();
            throw;
        }
    };
    try {
        if (!create_if_missing && !IsTableExists(TableFor(message)->TableName()))
            throw std::runtime_error("cannot update fields of a missing table");
        for (const auto &statement : MigrationStatements(message))
            Execute(statement);
    } catch (...) {
        try {
            release();
        } catch (...) {
        }
        throw;
    }
    release();
}
void Database::CreateOrUpdateTable(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    SynchronizeTable(message, true);
}
void Database::UpdateTableField(const Message &message) {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    SynchronizeTable(message, false);
}
void Database::SyncAllTables() {
    std::lock_guard<std::recursive_mutex> operation_lock(operation_mutex_);
    const auto keys = SortedTableKeys();
    for (const auto &key : keys)
        CreateOrUpdateTable(tables_.at(key).DefaultInstance());
}
} // namespace pbmysql
