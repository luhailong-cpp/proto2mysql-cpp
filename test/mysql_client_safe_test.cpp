#include <gtest/gtest.h>
#include <cstdlib>
#include <limits>
#include "src/mysql_wrapper/mysql_client.h"
#include "src/mysql_wrapper/value_codec.h"

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
    const char* host = std::getenv("PBMYSQL_TEST_HOST");
    if (!host) GTEST_SKIP() << "set PBMYSQL_TEST_HOST to enable isolated MySQL tests";
    auto env = [](const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value ? std::string(value) : std::string(fallback);
    };
    MysqlClient client;
    client.Connect(host, env("PBMYSQL_TEST_USER", "root"), env("PBMYSQL_TEST_PASSWORD", ""),
                   env("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
                   static_cast<unsigned int>(std::stoul(env("PBMYSQL_TEST_PORT", "3306"))));
    std::string binary(1024, 'x');
    binary[7] = '\0'; binary[900] = '\0';
    pbmysql::Statement statement{"SELECT ?, ?, ?, ?, ?, ?", {
        std::numeric_limits<int64_t>::min(), std::numeric_limits<uint64_t>::max(),
        1.000000002, binary, std::string(), nullptr}};
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
