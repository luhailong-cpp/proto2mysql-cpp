#include "src/mysql_wrapper/mysql_client.h"
#include "src/mysql_wrapper/value_codec.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>

TEST(MysqlClientSafety, ResultRowOwnsBinaryAndNullValues) {
    ResultRow row({std::string("a\0b", 3), std::nullopt, std::string()});
    ASSERT_EQ(row.size(), 3u);
    EXPECT_EQ(std::string(row[0], row.length(0)), std::string("a\0b", 3));
    EXPECT_EQ(row[1], nullptr);
    EXPECT_EQ(row.length(1), 0u);
    ASSERT_NE(row[2], nullptr);
    EXPECT_EQ(row.length(2), 0u);
    EXPECT_THROW(row[3], std::out_of_range);
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
    MysqlError error(1062, "duplicate", "23000");
    EXPECT_EQ(error.value(), 1062u);
    EXPECT_EQ(error.message(), "duplicate");
    EXPECT_EQ(error.sql_state(), "23000");
    EXPECT_STREQ(error.what(), "duplicate");
}
TEST(MysqlClientIntegration, PreparedBinaryNullAndIntegerRangesRoundTrip) {
    const char *host = std::getenv("PBMYSQL_TEST_HOST");
    if (!host)
        GTEST_SKIP() << "set PBMYSQL_TEST_HOST to enable isolated MySQL tests";
    auto env = [](const char *key, const char *fallback) {
        const char *value = std::getenv(key);
        return value ? std::string(value) : std::string(fallback);
    };
    MysqlClient client;
    client.Connect(host, env("PBMYSQL_TEST_USER", "root"), env("PBMYSQL_TEST_PASSWORD", ""),
                   env("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
                   static_cast<unsigned int>(std::stoul(env("PBMYSQL_TEST_PORT", "3306"))));
    std::string binary(1024, 'x');
    binary[7] = '\0';
    binary[900] = '\0';
    pbmysql::Statement statement{"SELECT ?, ?, ?, ?, ?, ?",
                                 {std::numeric_limits<int64_t>::min(),
                                  std::numeric_limits<uint64_t>::max(), 1.000000002, binary,
                                  std::string(), nullptr}};
    auto row = client.QueryOne(statement);
    ASSERT_TRUE(row);
    ASSERT_EQ(row->size(), 6u);
    EXPECT_EQ(std::string((*row)[0], row->length(0)), "-9223372036854775808");
    EXPECT_EQ(std::string((*row)[1], row->length(1)), "18446744073709551615");
    EXPECT_DOUBLE_EQ(std::stod(std::string((*row)[2], row->length(2))), 1.000000002);
    EXPECT_EQ(std::string((*row)[3], row->length(3)), binary);
    ASSERT_NE((*row)[4], nullptr);
    EXPECT_EQ(row->length(4), 0u);
    EXPECT_EQ((*row)[5], nullptr);
    EXPECT_THROW(client.Execute(pbmysql::Statement{"SELECT ?", {}}), std::invalid_argument);
    EXPECT_THROW(client.Execute(std::string("THIS IS INVALID SQL")), MysqlError);
    EXPECT_TRUE(client.QueryOne(std::string("SELECT 1")));
}

namespace {
std::string ClientTestEnv(const char *name, const char *fallback) {
    const char *value = std::getenv(name);
    return value ? value : fallback;
}
void ConnectClientTest(MysqlClient &client, const pbmysql::ConnectionOptions &options = {}) {
    client.Connect(
        ClientTestEnv("PBMYSQL_TEST_HOST", "127.0.0.1"), ClientTestEnv("PBMYSQL_TEST_USER", "root"),
        ClientTestEnv("PBMYSQL_TEST_PASSWORD", ""),
        ClientTestEnv("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
        static_cast<unsigned int>(std::stoul(ClientTestEnv("PBMYSQL_TEST_PORT", "3306"))), options);
}
std::string Cell(const MysqlClient::ResultRowPtr &row, size_t column = 0) {
    if (!row || column >= row->size() || !(*row)[column])
        throw std::runtime_error("expected non-NULL result cell");
    return std::string((*row)[column], row->length(column));
}
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
    auto charset =
        client.QueryOne(std::string("SELECT @@character_set_client, @@character_set_connection"));
    EXPECT_EQ(Cell(charset, 0), "utf8mb4");
    EXPECT_EQ(Cell(charset, 1), "utf8mb4");
    client.Execute(std::string(
        "SET SESSION sql_mode = 'STRICT_TRANS_TABLES,NO_BACKSLASH_ESCAPES,ANSI_QUOTES'"));
    std::string value = "\xF0\x9F\x8E\xAE'\\\"";
    value += std::string("\0tail", 5);
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
    auto row = client.QueryOne(std::string("SELECT NULL, '', 'retained'"));
    ASSERT_TRUE(row);
    EXPECT_EQ((*row)[0], nullptr);
    EXPECT_EQ(row->length(1), 0u);
    EXPECT_EQ(Cell(row, 2), "retained");
    EXPECT_FALSE(client.QueryOne(std::string("SELECT 1 WHERE 0=1")));
    EXPECT_EQ(Cell(row, 2), "retained");
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
    options.connect_timeout_seconds = 5;
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
    EXPECT_THROW(
        retry.Connect(
            ClientTestEnv("PBMYSQL_TEST_HOST", "127.0.0.1"),
            ClientTestEnv("PBMYSQL_TEST_USER", "root"), ClientTestEnv("PBMYSQL_TEST_PASSWORD", ""),
            "pbmysql_nonexistent_database_for_connect_contract",
            static_cast<unsigned int>(std::stoul(ClientTestEnv("PBMYSQL_TEST_PORT", "3306")))),
        MysqlError);
    EXPECT_FALSE(retry.Connected());
    ConnectClientTest(retry);
    EXPECT_TRUE(retry.Connected());
    EXPECT_THROW(ConnectClientTest(retry), std::logic_error);
    EXPECT_EQ(Cell(retry.QueryOne(std::string("SELECT 5"))), "5");
}
