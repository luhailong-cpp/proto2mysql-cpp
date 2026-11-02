#include "src/mysql_wrapper/database.h"

namespace pbmysql {
void Database::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::Disconnect();
}
void Database::Execute(const std::string &sql) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::Execute(sql);
}
void Database::Execute(const Statement &statement) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::Execute(statement);
}
ExecutionResult Database::ExecuteStatement(const Statement &statement) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    Execute(statement);
    return {AffectedRows(), LastInsertId()};
}
MysqlClient::ResultRowPtr Database::QueryOne(const std::string &sql) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return MysqlClient::QueryOne(sql);
}
MysqlClient::ResultRowPtr Database::QueryOne(const Statement &statement) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return MysqlClient::QueryOne(statement);
}
void Database::Query(const std::string &sql, const RowProcessor &processor) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::Query(sql, processor);
}
void Database::Query(const Statement &statement, const RowProcessor &processor) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::Query(statement, processor);
}
void Database::QueryResultRowProcessor(const std::string &sql,
                                       const ResultRowProcessor &processor) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::QueryResultRowProcessor(sql, processor);
}
void Database::QueryResultRowProcessor(const Statement &statement,
                                       const ResultRowProcessor &processor) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    MysqlClient::QueryResultRowProcessor(statement, processor);
}
uint64_t Database::LastInsertId() {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return MysqlClient::LastInsertId();
}
uint64_t Database::AffectedRows() {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return MysqlClient::AffectedRows();
}
} // namespace pbmysql
