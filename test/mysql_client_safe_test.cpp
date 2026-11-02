#include "src/mysql_wrapper/mysql_client.h"
#include "src/mysql_wrapper/value_codec.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>
#include <mysqld_error.h>

namespace {
constexpr char kIntegrityConstraintViolationSqlState[] = "23000";
constexpr unsigned int kTestConnectTimeoutSeconds = 5;
std::string ClientTestEnv(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    return value ? value : fallback;
}
unsigned int ClientTestPort() {
    const char *port = std::getenv("PBMYSQL_TEST_PORT");
    return port ? static_cast<unsigned int>(std::stoul(port)) : MYSQL_PORT;
}
void ConnectClientTest(MysqlClient &client, const pbmysql::ConnectionOptions &options = {}) {
    client.Connect(
        ClientTestEnv("PBMYSQL_TEST_HOST", "127.0.0.1"), ClientTestEnv("PBMYSQL_TEST_USER", "root"),
        ClientTestEnv("PBMYSQL_TEST_PASSWORD", ""),
        ClientTestEnv("PBMYSQL_TEST_DATABASE", "proto2mysql_test"), ClientTestPort(), options);
}
std::string Cell(const MysqlClient::ResultRowPtr &row, size_t column = 0) {
    if (!row || column >= row->size() || !(*row)[column])
        throw std::runtime_error("expected non-NULL result cell");
    return std::string((*row)[column], row->length(column));
}
} // namespace

TEST(MysqlClientSafety, ResultRowOwnsBinaryAndNullValues) {
    enum Column : std::size_t { kBinary, kNull, kEmptyText, kColumnCount };
    constexpr char kBinaryValue[] = "a\0b";
    const std::string binary(kBinaryValue, sizeof(kBinaryValue) - 1);
    ResultRow row({binary, std::nullopt, std::string()});
    ASSERT_EQ(row.size(), kColumnCount);
    EXPECT_EQ(std::string(row[kBinary], row.length(kBinary)), binary);
    EXPECT_EQ(row[kNull], nullptr);
    EXPECT_EQ(row.length(kNull), 0u);
    ASSERT_NE(row[kEmptyText], nullptr);
    EXPECT_EQ(row.length(kEmptyText), 0u);
    EXPECT_THROW(row[kColumnCount], std::out_of_range);
}
TEST(MysqlClientSafety, UnopenedConnectionFailsAndDestructorIsSafe) {
    MysqlClient client;
    EXPECT_FALSE(client.Connected());
    EXPECT_THROW(client.Execute(std::string("SELECT 1")), std::logic_error);
    EXPECT_THROW(client.Execute(pbmysql::Statement{"SELECT ?", {int64_t(1)}}), std::logic_error);
    EXPECT_THROW(client.QueryOne(std::string("SELECT 1")), std::logic_error);
    client.Disconnect();
    client.Disconnect();
}
TEST(MysqlClientSafety, ErrorPreservesServerCodeAndSqlState) {
    MysqlError error(ER_DUP_ENTRY, "duplicate", kIntegrityConstraintViolationSqlState);
    EXPECT_EQ(error.value(), static_cast<unsigned int>(ER_DUP_ENTRY));
    EXPECT_EQ(error.message(), "duplicate");
    EXPECT_EQ(error.sql_state(), kIntegrityConstraintViolationSqlState);
    EXPECT_STREQ(error.what(), "duplicate");
}
TEST(MysqlClientIntegration, PreparedBinaryNullAndIntegerRangesRoundTrip) {
    enum Column : std::size_t {
        kSignedInteger,
        kUnsignedInteger,
        kFloatingPoint,
        kBinary,
        kEmptyText,
        kNull,
        kColumnCount
    };
    const char *host = std::getenv("PBMYSQL_TEST_HOST");
    if (!host)
        GTEST_SKIP() << "set PBMYSQL_TEST_HOST to enable isolated MySQL tests";
    MysqlClient client;
    ConnectClientTest(client);
    constexpr std::size_t kLargeBinaryBytes = 1024;
    constexpr std::size_t kEarlyEmbeddedNulOffset = 7;
    constexpr std::size_t kLateEmbeddedNulOffset = 900;
    std::string binary(kLargeBinaryBytes, 'x');
    binary[kEarlyEmbeddedNulOffset] = '\0';
    binary[kLateEmbeddedNulOffset] = '\0';
    pbmysql::Statement statement{"SELECT ?, ?, ?, ?, ?, ?",
                                 {std::numeric_limits<int64_t>::min(),
                                  std::numeric_limits<uint64_t>::max(), 1.000000002, binary,
                                  std::string(), nullptr}};
    auto row = client.QueryOne(statement);
    ASSERT_TRUE(row);
    ASSERT_EQ(row->size(), kColumnCount);
    EXPECT_EQ(std::string((*row)[kSignedInteger], row->length(kSignedInteger)),
              "-9223372036854775808");
    EXPECT_EQ(std::string((*row)[kUnsignedInteger], row->length(kUnsignedInteger)),
              "18446744073709551615");
    EXPECT_DOUBLE_EQ(std::stod(std::string((*row)[kFloatingPoint], row->length(kFloatingPoint))),
                     1.000000002);
    EXPECT_EQ(std::string((*row)[kBinary], row->length(kBinary)), binary);
    ASSERT_NE((*row)[kEmptyText], nullptr);
    EXPECT_EQ(row->length(kEmptyText), 0u);
    EXPECT_EQ((*row)[kNull], nullptr);
    EXPECT_THROW(client.Execute(pbmysql::Statement{"SELECT ?", {}}), std::invalid_argument);
    EXPECT_THROW(client.Execute(std::string("THIS IS INVALID SQL")), MysqlError);
    EXPECT_TRUE(client.QueryOne(std::string("SELECT 1")));
}

namespace {
class MysqlClientContract : public testing::Test {
  protected:
    void SetUp() override {
        if (!std::getenv("PBMYSQL_TEST_HOST"))
            GTEST_SKIP() << "set PBMYSQL_TEST_HOST for integration";
        ConnectClientTest(client);
    }
    void TearDown() override {
        if (!client.Connected())
            return;
        try {
            client.Execute(std::string("DROP TABLE IF EXISTS pbmysql_client_result_contract"));
        } catch (...) {
        }
        if (procedure_created) {
            try {
                client.Execute(
                    std::string("DROP PROCEDURE IF EXISTS pbmysql_client_result_contract"));
            } catch (...) {
            }
        }
    }
    MysqlClient client;
    bool procedure_created = false;
};
} // namespace

TEST(MysqlClientSafety, NullRawPointersAreRejectedUnlessRowIsEmpty) {
    EXPECT_THROW(ResultRow(nullptr, nullptr, nullptr, 1), std::invalid_argument);
    EXPECT_EQ(ResultRow(nullptr, nullptr, nullptr, 0).size(), 0u);
}
TEST_F(MysqlClientContract, CharsetAndBoundValuesRespectConnectionSqlMode) {
    enum Column : std::size_t { kClientCharset, kConnectionCharset };
    auto charset =
        client.QueryOne(std::string("SELECT @@character_set_client, @@character_set_connection"));
    EXPECT_EQ(Cell(charset, kClientCharset), "utf8mb4");
    EXPECT_EQ(Cell(charset, kConnectionCharset), "utf8mb4");
    client.Execute(std::string(
        "SET SESSION sql_mode = 'STRICT_TRANS_TABLES,NO_BACKSLASH_ESCAPES,ANSI_QUOTES'"));
    std::string value = "\xF0\x9F\x8E\xAE'\\\"";
    constexpr char kEmbeddedNulSuffix[] = "\0tail";
    value.append(kEmbeddedNulSuffix, sizeof(kEmbeddedNulSuffix) - 1);
    auto row = client.QueryOne(pbmysql::Statement{"SELECT ?", {value}});
    EXPECT_EQ(Cell(row), value);
    EXPECT_NE(Cell(client.QueryOne(std::string("SELECT @@SESSION.sql_mode")))
                  .find("NO_BACKSLASH_ESCAPES"),
              std::string::npos);
}
TEST_F(MysqlClientContract, ChangedRowCountsAndLastInsertIdAreAccurate) {
    client.Execute(std::string("DROP TABLE IF EXISTS pbmysql_client_result_contract"));
    client.Execute(std::string("CREATE TABLE pbmysql_client_result_contract (id BIGINT UNSIGNED "
                               "NOT NULL AUTO_INCREMENT PRIMARY KEY, value INT NOT NULL)"));
    client.Execute(pbmysql::Statement{
        "INSERT INTO pbmysql_client_result_contract(value) VALUES (?)", {int64_t(7)}});
    const uint64_t id = client.LastInsertId();
    EXPECT_GT(id, 0u);
    EXPECT_EQ(client.AffectedRows(), 1u);
    client.Execute(pbmysql::Statement{
        "UPDATE pbmysql_client_result_contract SET value = ? WHERE id = ?", {int64_t(7), id}});
    EXPECT_EQ(client.AffectedRows(), 0u);
    client.Execute(pbmysql::Statement{
        "UPDATE pbmysql_client_result_contract SET value = ? WHERE id = ?", {int64_t(8), id}});
    EXPECT_EQ(client.AffectedRows(), 1u);
    client.Execute(
        pbmysql::Statement{"DELETE FROM pbmysql_client_result_contract WHERE id = ?", {id}});
    EXPECT_EQ(client.AffectedRows(), 1u);
    client.Execute(
        pbmysql::Statement{"DELETE FROM pbmysql_client_result_contract WHERE id = ?", {id}});
    EXPECT_EQ(client.AffectedRows(), 0u);
}
TEST_F(MysqlClientContract, CallbackExitOrExceptionLeavesConnectionUsable) {
    enum Column : std::size_t { kNull, kEmptyText, kRetainedText };
    auto row = client.QueryOne(std::string("SELECT NULL, '', 'retained'"));
    ASSERT_TRUE(row);
    EXPECT_EQ((*row)[kNull], nullptr);
    EXPECT_EQ(row->length(kEmptyText), 0u);
    EXPECT_EQ(Cell(row, kRetainedText), "retained");
    EXPECT_FALSE(client.QueryOne(std::string("SELECT 1 WHERE 0=1")));
    EXPECT_EQ(Cell(row, kRetainedText), "retained");
    size_t visited = 0;
    client.Query(std::string("SELECT 1 UNION ALL SELECT 2"),
                 [&visited](const MYSQL_ROW &, const unsigned long *, uint32_t) {
                     ++visited;
                     return false;
                 });
    EXPECT_EQ(visited, 1u);
    EXPECT_THROW(client.QueryResultRowProcessor(std::string("SELECT 1 UNION ALL SELECT 2"),
                                                [](const MysqlClient::ResultRowPtr &) -> bool {
                                                    throw std::runtime_error("callback failure");
                                                }),
                 std::runtime_error);
    EXPECT_EQ(Cell(client.QueryOne(std::string("SELECT 42"))), "42");
    EXPECT_THROW(client.QueryResultRowProcessor(
                     pbmysql::Statement{"SELECT ? UNION ALL SELECT ?", {int64_t(1), int64_t(2)}},
                     [](const MysqlClient::ResultRowPtr &) -> bool {
                         throw std::runtime_error("callback failure");
                     }),
                 std::runtime_error);
    EXPECT_EQ(Cell(client.QueryOne(pbmysql::Statement{"SELECT ?", {int64_t(43)}})), "43");
}
TEST_F(MysqlClientContract, ExplicitMultiStatementsDrainAllResultsAndPropagateLaterErrors) {
    MysqlClient multi;
    pbmysql::ConnectionOptions options;
    options.client_flags = CLIENT_MULTI_STATEMENTS;
    options.connect_timeout_seconds = kTestConnectTimeoutSeconds;
    ConnectClientTest(multi, options);
    EXPECT_EQ(Cell(multi.QueryOne(std::string("SELECT 1; SELECT 2"))), "1");
    EXPECT_EQ(Cell(multi.QueryOne(std::string("SELECT 3"))), "3");
    EXPECT_THROW(multi.Execute(std::string("SELECT 1; SELECT column_that_does_not_exist")),
                 MysqlError);
    EXPECT_EQ(Cell(multi.QueryOne(std::string("SELECT 4"))), "4");
}
TEST_F(MysqlClientContract, StoredProcedureResultsDrainAfterRawAndPreparedCallbacks) {
    if (Cell(client.QueryOne(std::string("SELECT VERSION()"))).find("TiDB") != std::string::npos)
        GTEST_SKIP() << "TiDB does not implement stored procedures";
    client.Execute(std::string("DROP PROCEDURE IF EXISTS pbmysql_client_result_contract"));
    client.Execute(std::string(
        "CREATE PROCEDURE pbmysql_client_result_contract() BEGIN SELECT 1; SELECT 2; END"));
    procedure_created = true;
    EXPECT_EQ(Cell(client.QueryOne(std::string("CALL pbmysql_client_result_contract()"))), "1");
    EXPECT_EQ(Cell(client.QueryOne(std::string("SELECT 3"))), "3");
    EXPECT_EQ(
        Cell(client.QueryOne(pbmysql::Statement{"CALL pbmysql_client_result_contract()", {}})),
        "1");
    EXPECT_THROW(client.QueryResultRowProcessor(
                     pbmysql::Statement{"CALL pbmysql_client_result_contract()", {}},
                     [](const MysqlClient::ResultRowPtr &) -> bool {
                         throw std::runtime_error("callback failure");
                     }),
                 std::runtime_error);
    EXPECT_EQ(Cell(client.QueryOne(std::string("SELECT 4"))), "4");
}
TEST_F(MysqlClientContract, FailedConnectCanRetryAndDoubleConnectPreservesConnection) {
    MysqlClient retry;
    EXPECT_THROW(retry.Connect(ClientTestEnv("PBMYSQL_TEST_HOST", "127.0.0.1"),
                               ClientTestEnv("PBMYSQL_TEST_USER", "root"),
                               ClientTestEnv("PBMYSQL_TEST_PASSWORD", ""),
                               "pbmysql_nonexistent_database_for_connect_contract",
                               ClientTestPort()),
                 MysqlError);
    EXPECT_FALSE(retry.Connected());
    ConnectClientTest(retry);
    EXPECT_TRUE(retry.Connected());
    EXPECT_THROW(ConnectClientTest(retry), std::logic_error);
    EXPECT_EQ(Cell(retry.QueryOne(std::string("SELECT 5"))), "5");
}
