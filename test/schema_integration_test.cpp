#include "proto2mysql_option.pb.h"
#include "runtime_test.pb.h"
#include "src/mysql_wrapper/database.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <google/protobuf/dynamic_message.h>
#include <gtest/gtest.h>

namespace {
using namespace pbmysql;
using namespace google::protobuf;
const char *Setting(const char *name, const char *fallback) {
    auto *v = std::getenv(name);
    return v ? v : fallback;
}
class SchemaIntegration : public ::testing::Test {
  protected:
    DescriptorPool pool{DescriptorPool::generated_pool()};
    DynamicMessageFactory factory{&pool};
    Database db;
    std::string table;
    void SetUp() override {
        if (!std::getenv("PBMYSQL_TEST_HOST"))
            GTEST_SKIP() << "set PBMYSQL_TEST_HOST to run database schema integration";
        static std::atomic<unsigned> sequence{0};
        table = "p2m_schema_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(sequence++);
        db.Connect(Setting("PBMYSQL_TEST_HOST", "127.0.0.1"), Setting("PBMYSQL_TEST_USER", "root"),
                   Setting("PBMYSQL_TEST_PASSWORD", ""),
                   Setting("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
                   static_cast<unsigned>(std::stoul(Setting("PBMYSQL_TEST_PORT", "3306"))));
        db.SetDbName(Setting("PBMYSQL_TEST_DATABASE", "proto2mysql_test"));
    }
    void TearDown() override {
        if (!db.Connected() || table.empty())
            return;
        for (const auto &suffix :
             {std::string(), std::string("__p2m_new"), std::string("__p2m_old")})
            try {
                db.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(table + suffix));
            } catch (...) {
            }
    }
    MigrationReport Report(const Message &message) {
        try {
            db.GenerateMigrationSql(message);
            ADD_FAILURE() << "expected legacy key error";
        } catch (const SchemaError &e) {
            EXPECT_TRUE(e.Is(SchemaErrorCode::LegacyKeyColumn));
            return e.Report();
        }
        return {};
    }
    void ExecuteReport(const MigrationReport &report) {
        for (const auto &sql : report.statements)
            db.Execute(sql);
    }
    const Message &ShadowPrototype() {
        FileDescriptorProto file;
        file.set_name("shadow_runtime.proto");
        file.set_syntax("proto3");
        file.add_dependency("proto2mysql_option.proto");
        auto *m = file.add_message_type();
        m->set_name("ShadowRow");
        m->mutable_options()->SetExtension(proto2mysql::table_name, table);
        m->mutable_options()->SetExtension(proto2mysql::primary_key, "name");
        m->mutable_options()->SetExtension(proto2mysql::auto_increment_key, "id");
        m->mutable_options()->SetExtension(proto2mysql::index, "id");
        auto field = [&](const char *name, int number, FieldDescriptorProto::Type type) {
            auto *f = m->add_field();
            f->set_name(name);
            f->set_number(number);
            f->set_type(type);
            f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
        };
        field("name", 1, FieldDescriptorProto::TYPE_STRING);
        field("id", 2, FieldDescriptorProto::TYPE_UINT64);
        field("counter", 3, FieldDescriptorProto::TYPE_UINT32);
        field("data", 4, FieldDescriptorProto::TYPE_BYTES);
        auto *built = pool.BuildFile(file);
        if (!built)
            throw std::runtime_error("cannot build shadow fixture");
        return *factory.GetPrototype(built->message_type(0));
    }
};
TEST_F(SchemaIntegration, LegacyUniqueMigrationPreservesRowsAndOnlyExecutesOnExplicitRequest) {
    using Row = pbmysql::test::RuntimeRow;
    TableOptions options;
    options.table_name = table;
    db.RegisterTable(Row::default_instance(), options);
    db.CreateOrUpdateTable(Row::default_instance());
    db.Execute("ALTER TABLE " + QuoteIdentifier(table) + " DROP INDEX " +
               QuoteIdentifier("uk_" + table));
    db.Execute("ALTER TABLE " + QuoteIdentifier(table) +
               " MODIFY COLUMN `email` MEDIUMTEXT COMMENT 'pb:2'");
    db.Execute("ALTER TABLE " + QuoteIdentifier(table) + " ADD UNIQUE KEY " +
               QuoteIdentifier("uk_" + table) + " (`email`(191))");
    db.Execute(Statement{"INSERT INTO " + QuoteIdentifier(table) +
                             " (`id`,`email`,`gold`) VALUES (?,?,?),(?,?,?)",
                         {uint64_t{1}, std::string("Keep Case "), uint64_t{99}, uint64_t{2},
                          nullptr, uint64_t{17}}});
    auto report = Report(Row::default_instance());
    ASSERT_FALSE(report.shadow_rebuild);
    ASSERT_FALSE(report.statements.empty());
    auto before = db.QueryOne("SELECT IS_NULLABLE,COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS "
                              "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='" +
                              table + "' AND COLUMN_NAME='email'");
    ASSERT_TRUE(before);
    EXPECT_EQ(std::string((*before)[0]), "YES");
    ExecuteReport(report);
    EXPECT_TRUE(db.GenerateMigrationSql(Row::default_instance()).empty());
    Row loaded;
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.email(), "Keep Case ");
    EXPECT_EQ(loaded.gold(), 99u);
    loaded.Clear();
    loaded.set_id(2);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.email(), "");
    EXPECT_EQ(loaded.gold(), 17u);
    Row distinct;
    distinct.set_id(3);
    distinct.set_email("keep case ");
    EXPECT_NO_THROW(db.Insert(distinct));
    distinct.set_id(4);
    distinct.set_email("Keep Case");
    EXPECT_NO_THROW(db.Insert(distinct));
}
TEST_F(SchemaIntegration,
       ShadowPrimaryMigrationPreservesWideDataExtraIndexAndAutoIncrementWatermark) {
    const auto &prototype = ShadowPrototype();
    db.RegisterTable(prototype);
    db.Execute("CREATE TABLE " + QuoteIdentifier(table) +
               " (`name` VARCHAR(191) COLLATE utf8mb4_unicode_ci NOT NULL DEFAULT '' COMMENT "
               "'pb:1', `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'pb:2', `counter` "
               "BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'pb:3', `data` LONGBLOB COMMENT 'pb:4', "
               "PRIMARY KEY (`name`), KEY " +
               QuoteIdentifier("idx_" + table + "_0") +
               " (`id`), KEY `custom_data` (`data`(12))) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    const std::string payload("a\0b'\\\xff", 7);
    db.Execute(Statement{"INSERT INTO " + QuoteIdentifier(table) +
                             " (`name`,`id`,`counter`,`data`) VALUES (?,?,?,?)",
                         {std::string("Name "), uint64_t{7}, uint64_t{5000000000ULL}, payload}});
    db.Execute("ALTER TABLE " + QuoteIdentifier(table) + " AUTO_INCREMENT=10001");
    const auto report = Report(prototype);
    ASSERT_TRUE(report.shadow_rebuild);
    EXPECT_NE(report.statements[1].find("counter` bigint unsigned"), std::string::npos);
    EXPECT_NE(report.statements[1].find("KEY `custom_data`"), std::string::npos);
    ExecuteReport(report);
    EXPECT_TRUE(db.GenerateMigrationSql(prototype).empty());
    auto row = db.QueryOne("SELECT `id`,`counter`,`data` FROM " + QuoteIdentifier(table) +
                           " WHERE `name`='Name '");
    ASSERT_TRUE(row);
    EXPECT_EQ(std::string((*row)[1]), "5000000000");
    EXPECT_EQ(std::string((*row)[2], row->length(2)), payload);
    db.Execute(Statement{"INSERT INTO " + QuoteIdentifier(table) + " (`name`) VALUES (?)",
                         {std::string("next")}});
    EXPECT_GE(db.LastInsertId(), 10001u);
    auto extra = db.QueryOne("SELECT COUNT(*) FROM INFORMATION_SCHEMA.STATISTICS WHERE "
                             "TABLE_SCHEMA=DATABASE() AND TABLE_NAME='" +
                             table + "' AND INDEX_NAME='custom_data'");
    ASSERT_TRUE(extra);
    EXPECT_EQ(std::string((*extra)[0]), "1");
}
TEST_F(SchemaIntegration, MissingPrimaryAndAutoIncrementColumnAreAddedWithDependentIndexes) {
    const auto &prototype = ShadowPrototype();
    db.RegisterTable(prototype);
    // Missing id is a secondary-index auto column: its new index is installed
    // in the same ALTER. Ordinary string PK remains a full binary key.
    db.Execute(
        "CREATE TABLE " + QuoteIdentifier(table) +
        " (`name` VARCHAR(191) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT '' "
        "COMMENT 'pb:1', `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'pb:2', `counter` "
        "INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'pb:3', `data` MEDIUMBLOB COMMENT 'pb:4', KEY " +
        QuoteIdentifier("idx_" + table + "_0") + " (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    const auto script = db.GenerateMigrationSql(prototype);
    EXPECT_NE(script.find("ADD PRIMARY KEY (`name`)"), std::string::npos);
    db.CreateOrUpdateTable(prototype);
    EXPECT_TRUE(db.GenerateMigrationSql(prototype).empty());
}
TEST_F(SchemaIntegration, UnsafeDriftAndExpandOnlyNeverRunDDL) {
    using Row = pbmysql::test::RuntimeRow;
    TableOptions options;
    options.table_name = table;
    db.RegisterTable(Row::default_instance(), options);
    db.CreateOrUpdateTable(Row::default_instance());
    db.Execute("ALTER TABLE " + QuoteIdentifier(table) +
               " MODIFY COLUMN `gold` BIGINT NOT NULL DEFAULT 0 COMMENT 'pb:3'");
    try {
        db.CreateOrUpdateTable(Row::default_instance());
        FAIL();
    } catch (const SchemaError &e) {
        EXPECT_EQ(e.Code(), SchemaErrorCode::UnsafeSchemaConversion);
    }
    auto metadata = db.QueryOne("SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS WHERE "
                                "TABLE_SCHEMA=DATABASE() AND TABLE_NAME='" +
                                table + "' AND COLUMN_NAME='gold'");
    ASSERT_TRUE(metadata);
    EXPECT_EQ(std::string((*metadata)[0]), "bigint");
}
} // namespace
