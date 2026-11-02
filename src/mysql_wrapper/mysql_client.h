#pragma once

#include "src/mysql_wrapper/mysql_result.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mysql.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace pbmysql {
struct Statement;
// Native connector options. The defaults preserve changed-row counts used by
// conditional saves. CLIENT_FOUND_ROWS changes that outcome classification.
struct ConnectionOptions {
    std::string unix_socket;
    std::string charset = "utf8mb4";
    unsigned long client_flags = 0;
    unsigned int connect_timeout_seconds = 0;
    unsigned int read_timeout_seconds = 0;
    unsigned int write_timeout_seconds = 0;
};
} // namespace pbmysql

class MysqlError : public std::runtime_error {
  public:
    MysqlError(unsigned int code, std::string message, std::string sql_state = {})
        : std::runtime_error(message), code_(code), state_(std::move(sql_state)) {}
    explicit operator bool() const { return code_ != 0; }
    std::string message() const { return what(); }
    const std::string &sql_state() const { return state_; }
    unsigned int value() const { return code_; }

  private:
    unsigned int code_;
    std::string state_;
};

struct MYSQL_RES_Deleter {
    void operator()(MYSQL_RES *result) const {
        if (result)
            mysql_free_result(result);
    }
};

// A client owns one connection. Use a separate Database per concurrent worker.
// Reconnection is deliberately disabled so a transaction cannot silently vanish.
class MysqlClient {
  public:
    using MysqlResult = std::unique_ptr<MYSQL_RES, MYSQL_RES_Deleter>;
    using ResultRowPtr = std::unique_ptr<ResultRow>;
    using RowProcessor = std::function<bool(const MYSQL_ROW &, const unsigned long *, uint32_t)>;
    using ResultRowProcessor = std::function<bool(const ResultRowPtr &)>;

    MysqlClient() = default;
    virtual ~MysqlClient();
    MysqlClient(const MysqlClient &) = delete;
    MysqlClient &operator=(const MysqlClient &) = delete;
    MysqlClient(MysqlClient &&) = delete;
    MysqlClient &operator=(MysqlClient &&) = delete;

    template <typename ConnectionParamClass> void Connect(const ConnectionParamClass &info) {
        Connect(info.db_host(), info.db_user(), info.db_password(), info.db_dbname(),
                static_cast<unsigned int>(info.db_port()));
    }
    void Connect(const std::string &host, const std::string &user, const std::string &password,
                 const std::string &database, unsigned int port = 3306);
    void Connect(const std::string &host, const std::string &user, const std::string &password,
                 const std::string &database, unsigned int port,
                 const pbmysql::ConnectionOptions &options);
    void Disconnect() noexcept;
    bool Connected() const noexcept { return connection_ != nullptr; }

    virtual void Execute(const std::string &query);
    virtual void Execute(const pbmysql::Statement &statement);
    virtual ResultRowPtr QueryOne(const std::string &query);
    virtual ResultRowPtr QueryOne(const pbmysql::Statement &statement);
    virtual void Query(const std::string &query, const RowProcessor &processor);
    virtual void Query(const pbmysql::Statement &statement, const RowProcessor &processor);
    virtual void QueryResultRowProcessor(const std::string &query,
                                         const ResultRowProcessor &processor);
    virtual void QueryResultRowProcessor(const pbmysql::Statement &statement,
                                         const ResultRowProcessor &processor);
    virtual uint64_t LastInsertId();
    virtual uint64_t AffectedRows();

  protected:
    MYSQL *connection() { return connection_; }
    const MYSQL *connection() const { return connection_; }

  private:
    void RequireConnection() const;
    MysqlResult RealQuery(const std::string &query);
    MYSQL *connection_ = nullptr;
    uint64_t last_insert_id_ = 0;
    uint64_t affected_rows_ = 0;
};

namespace pbmysql {
using MysqlError = ::MysqlError;
using MysqlClient = ::MysqlClient;
using ResultRow = ::ResultRow;
} // namespace pbmysql
