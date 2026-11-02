#include "src/mysql_wrapper/mysql_client.h"
#include "src/mysql_wrapper/value_codec.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <type_traits>
#include <vector>

namespace {
constexpr int kNoMoreResultSets = -1;
constexpr std::size_t kInitialResultColumnBufferBytes = 256;

[[noreturn]] void ThrowConnectionError(MYSQL *mysql) {
    throw MysqlError(mysql_errno(mysql), mysql_error(mysql), mysql_sqlstate(mysql));
}
[[noreturn]] void ThrowStatementError(MYSQL_STMT *stmt) {
    throw MysqlError(mysql_stmt_errno(stmt), mysql_stmt_error(stmt), mysql_stmt_sqlstate(stmt));
}
struct StatementDeleter {
    void operator()(MYSQL_STMT *stmt) const {
        if (stmt)
            mysql_stmt_close(stmt);
    }
};
using BindBool = std::remove_pointer_t<decltype(MYSQL_BIND{}.is_null)>;

void InitializeClientThread() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        if (mysql_library_init(0, nullptr, nullptr) != 0)
            throw std::runtime_error("mysql_library_init failed");
    });
    struct ThreadState {
        ThreadState() {
            if (mysql_thread_init() != 0)
                throw std::runtime_error("mysql_thread_init failed");
        }
        ~ThreadState() { mysql_thread_end(); }
    };
    thread_local ThreadState thread_state;
    (void)thread_state;
}

// CALL can produce several result sets even without multi-statements enabled.
// Drain them on early callback exit and on exceptions, preserving connection
// usability. Normal completion also propagates errors from later results.
class RemainingStatementResults {
  public:
    explicit RemainingStatementResults(MYSQL_STMT *statement) : statement_(statement) {}
    ~RemainingStatementResults() {
        if (active_) {
            try {
                Finish();
            } catch (...) {
            }
        }
    }
    void Finish() {
        mysql_stmt_free_result(statement_);
        for (;;) {
            const int result = mysql_stmt_next_result(statement_);
            if (result == kNoMoreResultSets)
                break;
            if (result != 0)
                ThrowStatementError(statement_);
            if (mysql_stmt_store_result(statement_))
                ThrowStatementError(statement_);
            mysql_stmt_free_result(statement_);
        }
        active_ = false;
    }

  private:
    MYSQL_STMT *statement_;
    bool active_ = true;
};
} // namespace

MysqlClient::~MysqlClient() { Disconnect(); }
void MysqlClient::Disconnect() noexcept {
    if (connection_) {
        try {
            InitializeClientThread();
        } catch (...) {
        }
        mysql_close(connection_);
    }
    connection_ = nullptr;
    last_insert_id_ = affected_rows_ = 0;
}
void MysqlClient::RequireConnection() const {
    if (!connection_)
        throw std::logic_error("MySQL connection is not open");
    InitializeClientThread();
}
void MysqlClient::Connect(const std::string &host, const std::string &user,
                          const std::string &password, const std::string &database,
                          unsigned int port) {
    Connect(host, user, password, database, port, pbmysql::ConnectionOptions{});
}
void MysqlClient::Connect(const std::string &host, const std::string &user,
                          const std::string &password, const std::string &database,
                          unsigned int port, const pbmysql::ConnectionOptions &options) {
    if (connection_)
        throw std::logic_error("MySQL connection is already open");
    if (options.charset.empty())
        throw std::invalid_argument("connection charset is empty");
    InitializeClientThread();
    MYSQL *next = mysql_init(nullptr);
    if (!next)
        throw std::runtime_error("mysql_init failed");
    try {
        BindBool reconnect = false;
        if (mysql_options(next, MYSQL_OPT_RECONNECT, &reconnect) != 0)
            ThrowConnectionError(next);
        if (options.connect_timeout_seconds &&
            mysql_options(next, MYSQL_OPT_CONNECT_TIMEOUT, &options.connect_timeout_seconds) != 0)
            ThrowConnectionError(next);
        if (options.read_timeout_seconds &&
            mysql_options(next, MYSQL_OPT_READ_TIMEOUT, &options.read_timeout_seconds) != 0)
            ThrowConnectionError(next);
        if (options.write_timeout_seconds &&
            mysql_options(next, MYSQL_OPT_WRITE_TIMEOUT, &options.write_timeout_seconds) != 0)
            ThrowConnectionError(next);
        if (mysql_options(next, MYSQL_SET_CHARSET_NAME, options.charset.c_str()) != 0)
            ThrowConnectionError(next);
        if (!mysql_real_connect(next, host.c_str(), user.c_str(), password.c_str(),
                                database.c_str(), port,
                                options.unix_socket.empty() ? nullptr : options.unix_socket.c_str(),
                                options.client_flags))
            ThrowConnectionError(next);
        if (mysql_set_character_set(next, options.charset.c_str()) != 0)
            ThrowConnectionError(next);
    } catch (...) {
        mysql_close(next);
        throw;
    }
    connection_ = next;
}
MysqlClient::MysqlResult MysqlClient::RealQuery(const std::string &query) {
    RequireConnection();
    if (query.empty())
        throw std::invalid_argument("empty SQL statement");
    if (query.size() > std::numeric_limits<unsigned long>::max())
        throw std::length_error("SQL statement exceeds client size limit");
    if (mysql_real_query(connection_, query.data(), static_cast<unsigned long>(query.size())) != 0)
        ThrowConnectionError(connection_);
    last_insert_id_ = mysql_insert_id(connection_);
    affected_rows_ = mysql_affected_rows(connection_);
    MysqlResult result(mysql_store_result(connection_));
    if (!result && mysql_field_count(connection_) != 0)
        ThrowConnectionError(connection_);
    while (mysql_more_results(connection_)) {
        const int next = mysql_next_result(connection_);
        if (next == kNoMoreResultSets)
            break;
        if (next != 0)
            ThrowConnectionError(connection_);
        MysqlResult extra(mysql_store_result(connection_));
        if (!extra && mysql_field_count(connection_) != 0)
            ThrowConnectionError(connection_);
        last_insert_id_ = mysql_insert_id(connection_);
        affected_rows_ = mysql_affected_rows(connection_);
    }
    return result;
}
void MysqlClient::Execute(const std::string &query) { RealQuery(query); }
void MysqlClient::Query(const std::string &query, const RowProcessor &processor) {
    auto result = RealQuery(query);
    if (!result)
        return;
    while (MYSQL_ROW row = mysql_fetch_row(result.get())) {
        auto *lengths = mysql_fetch_lengths(result.get());
        if (!lengths)
            ThrowConnectionError(connection_);
        if (!processor(row, lengths, mysql_num_fields(result.get())))
            return;
    }
    if (mysql_errno(connection_) != 0)
        ThrowConnectionError(connection_);
}
void MysqlClient::QueryResultRowProcessor(const std::string &query,
                                          const ResultRowProcessor &processor) {
    Query(query,
          [&processor](const MYSQL_ROW &row, const unsigned long *lengths, uint32_t columns) {
              auto owned = std::make_unique<ResultRow>(row, lengths, nullptr, columns);
              return processor(owned);
          });
}
MysqlClient::ResultRowPtr MysqlClient::QueryOne(const std::string &query) {
    ResultRowPtr first;
    QueryResultRowProcessor(query, [&first](const ResultRowPtr &row) {
        first = std::make_unique<ResultRow>(row->values());
        return false;
    });
    return first;
}

void MysqlClient::QueryResultRowProcessor(const pbmysql::Statement &statement,
                                          const ResultRowProcessor &processor) {
    RequireConnection();
    if (statement.sql.empty())
        throw std::invalid_argument("empty SQL statement");
    std::unique_ptr<MYSQL_STMT, StatementDeleter> stmt(mysql_stmt_init(connection_));
    if (!stmt)
        ThrowConnectionError(connection_);
    if (statement.sql.size() > std::numeric_limits<unsigned long>::max())
        throw std::length_error("SQL statement exceeds client size limit");
    if (mysql_stmt_prepare(stmt.get(), statement.sql.data(),
                           static_cast<unsigned long>(statement.sql.size())))
        ThrowStatementError(stmt.get());
    if (mysql_stmt_param_count(stmt.get()) != statement.args.size())
        throw std::invalid_argument("SQL placeholder/argument count mismatch");
    std::vector<MYSQL_BIND> parameters(statement.args.size());
    std::vector<unsigned long> parameter_lengths(statement.args.size());
    for (size_t i = 0; i < statement.args.size(); ++i) {
        auto &bind = parameters[i];
        const auto &value = statement.args[i];
        bind.length = &parameter_lengths[i];
        if (std::holds_alternative<std::nullptr_t>(value)) {
            bind.buffer_type = MYSQL_TYPE_NULL;
        } else if (const auto *p = std::get_if<int64_t>(&value)) {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.buffer = const_cast<int64_t *>(p);
        } else if (const auto *p = std::get_if<uint64_t>(&value)) {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.is_unsigned = true;
            bind.buffer = const_cast<uint64_t *>(p);
        } else if (const auto *p = std::get_if<double>(&value)) {
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            bind.buffer = const_cast<double *>(p);
        } else {
            const auto &text = std::get<std::string>(value);
            if (text.size() > std::numeric_limits<unsigned long>::max())
                throw std::length_error("SQL argument exceeds client size limit");
            bind.buffer_type = MYSQL_TYPE_STRING;
            parameter_lengths[i] = static_cast<unsigned long>(text.size());
            bind.buffer = const_cast<char *>(text.data());
            bind.buffer_length = parameter_lengths[i];
        }
    }
    if (!parameters.empty() && mysql_stmt_bind_param(stmt.get(), parameters.data()))
        ThrowStatementError(stmt.get());
    if (mysql_stmt_execute(stmt.get()))
        ThrowStatementError(stmt.get());
    RemainingStatementResults remaining(stmt.get());
    last_insert_id_ = mysql_stmt_insert_id(stmt.get());
    affected_rows_ = mysql_stmt_affected_rows(stmt.get());
    MysqlResult metadata(mysql_stmt_result_metadata(stmt.get()));
    if (!metadata) {
        if (mysql_stmt_field_count(stmt.get()) != 0)
            ThrowStatementError(stmt.get());
        remaining.Finish();
        return;
    }
    if (mysql_stmt_store_result(stmt.get()))
        ThrowStatementError(stmt.get());
    const size_t count = mysql_num_fields(metadata.get());
    std::vector<MYSQL_BIND> columns(count);
    std::vector<std::vector<char>> buffers(count,
                                           std::vector<char>(kInitialResultColumnBufferBytes));
    std::vector<unsigned long> lengths(count);
    auto nulls = std::make_unique<BindBool[]>(count);
    auto errors = std::make_unique<BindBool[]>(count);
    for (size_t i = 0; i < count; ++i) {
        columns[i].buffer_type = MYSQL_TYPE_STRING;
        columns[i].buffer = buffers[i].data();
        columns[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
        columns[i].length = &lengths[i];
        columns[i].is_null = &nulls[i];
        columns[i].error = &errors[i];
    }
    if (mysql_stmt_bind_result(stmt.get(), columns.data()))
        ThrowStatementError(stmt.get());
    for (;;) {
        const int result = mysql_stmt_fetch(stmt.get());
        if (result == MYSQL_NO_DATA)
            break;
        if (result != 0 && result != MYSQL_DATA_TRUNCATED)
            ThrowStatementError(stmt.get());
        std::vector<std::optional<std::string>> values;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (nulls[i]) {
                values.emplace_back(std::nullopt);
                continue;
            }
            if (lengths[i] > buffers[i].size()) {
                std::string text(lengths[i], '\0');
                MYSQL_BIND full{};
                full.buffer_type = MYSQL_TYPE_STRING;
                full.buffer = text.data();
                full.buffer_length = lengths[i];
                if (mysql_stmt_fetch_column(stmt.get(), &full, static_cast<unsigned int>(i), 0))
                    ThrowStatementError(stmt.get());
                values.emplace_back(std::move(text));
            } else {
                values.emplace_back(std::string(buffers[i].data(), lengths[i]));
            }
        }
        auto row = std::make_unique<ResultRow>(std::move(values));
        if (!processor(row))
            break;
    }
    remaining.Finish();
}
void MysqlClient::Execute(const pbmysql::Statement &statement) {
    QueryResultRowProcessor(statement, [](const ResultRowPtr &) { return false; });
}
MysqlClient::ResultRowPtr MysqlClient::QueryOne(const pbmysql::Statement &statement) {
    ResultRowPtr first;
    QueryResultRowProcessor(statement, [&first](const ResultRowPtr &row) {
        first = std::make_unique<ResultRow>(row->values());
        return false;
    });
    return first;
}
void MysqlClient::Query(const pbmysql::Statement &statement, const RowProcessor &processor) {
    QueryResultRowProcessor(statement, [&processor](const ResultRowPtr &row) {
        std::vector<char *> values(row->size());
        std::vector<unsigned long> lengths(row->size());
        for (size_t i = 0; i < row->size(); ++i) {
            values[i] = const_cast<char *>((*row)[i]);
            lengths[i] = row->length(i);
        }
        MYSQL_ROW raw = values.data();
        return processor(raw, lengths.data(), static_cast<uint32_t>(row->size()));
    });
}
uint64_t MysqlClient::LastInsertId() { return last_insert_id_; }
uint64_t MysqlClient::AffectedRows() { return affected_rows_; }
