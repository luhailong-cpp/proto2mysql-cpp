#include "runtime_test.pb.h"
#include "src/mysql_wrapper/database.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace {
using namespace pbmysql;
using Row = pbmysql::test::RuntimeRow;
const char *Env(const char *key, const char *fallback) {
    const char *value = std::getenv(key);
    return value ? value : fallback;
}
class MysqlIntegration : public ::testing::Test {
  protected:
    Database db;
    TableOptions options;
    std::string table;
    void Connect(Database &target) {
        target.Connect(Env("PBMYSQL_TEST_HOST", "127.0.0.1"), Env("PBMYSQL_TEST_USER", "root"),
                       Env("PBMYSQL_TEST_PASSWORD", ""),
                       Env("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
                       static_cast<unsigned>(std::stoul(Env("PBMYSQL_TEST_PORT", "3306"))));
        target.SetDbName(Env("PBMYSQL_TEST_DATABASE", "proto2mysql_test"));
        target.RegisterTable(Row::default_instance(), options);
    }
    void SetUp() override {
        if (!std::getenv("PBMYSQL_TEST_HOST"))
            GTEST_SKIP() << "set PBMYSQL_TEST_HOST to opt in to MySQL integration";
        static std::atomic<unsigned> sequence{0};
        table = "p2m_it_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(sequence++);
        options.table_name = table;
        Connect(db);
        db.CreateOrUpdateTable(Row::default_instance());
    }
    void TearDown() override {
        if (db.Connected() && !table.empty())
            db.Execute("DROP TABLE IF EXISTS " + QuoteIdentifier(table));
    }
    Row Target(uint64_t id = 1) {
        Row row;
        row.set_id(id);
        row.set_email("email-" + std::to_string(id));
        row.set_epoch(1);
        row.set_seq(1);
        return row;
    }
};
TEST_F(MysqlIntegration, ExactSaveBinaryZeroUniqueConflictAndIgnore) {
    auto row = Target();
    row.set_gold(100);
    row.set_data(std::string("a\0b'\\\xff", 7));
    EXPECT_EQ(db.SaveConditional(row).outcome, SaveOutcome::Inserted);
    EXPECT_EQ(db.SaveConditional(row).outcome, SaveOutcome::Unchanged);
    row.set_gold(0);
    row.set_explicit_zero(0);
    EXPECT_EQ(db.SaveConditional(row).outcome, SaveOutcome::Updated);
    Row loaded;
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.gold(), 0u);
    EXPECT_EQ(loaded.data(), row.data());
    EXPECT_TRUE(loaded.has_explicit_zero());
    auto conflict = Target(2);
    conflict.set_email(row.email());
    conflict.set_gold(999);
    try {
        db.Save(conflict);
        FAIL() << "alternate unique key must not update id=1";
    } catch (const MysqlError &error) {
        EXPECT_EQ(error.value(), 1062u);
    }
    EXPECT_FALSE(db.InsertIgnore(conflict));
    loaded.Clear();
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.gold(), 0u);
    conflict.set_email(std::string(192, 'e'));
    EXPECT_THROW(db.InsertIgnore(conflict), std::invalid_argument);
    EXPECT_TRUE(db.GenerateMigrationSql(Row::default_instance()).empty());
}
TEST_F(MysqlIntegration, FencedSavesPreserveColumnsAndRejectReplay) {
    auto row = Target();
    SaveOptions options{{"epoch", "seq"}, false, {}};
    EXPECT_EQ(db.SaveConditional(row, options).outcome, SaveOutcome::Inserted);
    EXPECT_EQ(db.SaveConditional(row, options).outcome, SaveOutcome::Rejected);
    row.set_seq(2);
    row.set_gold(10);
    EXPECT_EQ(db.SaveConditional(row, options).outcome, SaveOutcome::Updated);
    auto old = row;
    old.set_seq(1);
    old.set_gold(99);
    auto rejected = db.SaveConditional(old, options);
    EXPECT_EQ(rejected.outcome, SaveOutcome::Rejected);
    EXPECT_EQ(rejected.stored, (std::vector<uint64_t>{1, 2}));
    options.allow_equal = true;
    EXPECT_EQ(db.SaveConditional(row, options).outcome, SaveOutcome::Unchanged);
    row.set_gold(11);
    EXPECT_EQ(db.SaveConditional(row, options).outcome, SaveOutcome::Updated);
    old.set_epoch(0);
    old.set_seq(0);
    old.set_gold(12);
    EXPECT_TRUE(db.SaveConditional(old, {{}, false, {"epoch", "seq"}}).Applied());
    Row loaded;
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.epoch(), 1u);
    EXPECT_EQ(loaded.seq(), 2u);
    EXPECT_EQ(loaded.gold(), 12u);
}
TEST_F(MysqlIntegration, TransactionRollbackAndPreparedQueries) {
    auto row = Target();
    db.Save(row);
    EXPECT_FALSE(db.Transaction([&](Database &tx) {
        row.set_gold(99);
        tx.Save(row);
        EXPECT_TRUE(tx.FindOneByPrimaryKeyForUpdate(row));
        return false;
    }));
    row.Clear();
    row.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(row));
    EXPECT_EQ(row.gold(), 0u);
    EXPECT_THROW(db.Transaction([&](Database &tx) {
        tx.Save(row);
        tx.Execute("INSERT INTO `table_that_does_not_exist` VALUES (1)");
        return true;
    }),
                 MysqlError);
    auto injected = Target(2);
    injected.set_email("x' OR 1=1 --");
    db.Save(injected);
    Row found;
    ASSERT_TRUE(db.FindOneByKv(found, "email", injected.email()));
    EXPECT_EQ(found.id(), 2u);
    pbmysql::test::RuntimeRows list;
    db.FindAllByWhereWithArgs<Row>(list, "`email` = ?", {injected.email()});
    ASSERT_EQ(list.items_size(), 1);
    EXPECT_EQ(list.items(0).id(), 2u);
    db.IncrByPrimaryKey(row, "gold", 1);
    EXPECT_TRUE(db.DecrByPrimaryKeyIfEnough(row, "gold", 1));
    EXPECT_FALSE(db.DecrByPrimaryKeyIfEnough(row, "gold", 1));
}
TEST_F(MysqlIntegration, ConcurrentFencesCannotOverwriteNewerRows) {
    auto seed = Target();
    db.Save(seed);
    std::atomic<unsigned> errors{0};
    auto worker = [&](unsigned start) {
        try {
            Database connection;
            Connect(connection);
            for (unsigned n = start; n < start + 20; ++n) {
                auto row = Target();
                row.set_seq(n);
                row.set_gold(n);
                try {
                    connection.SaveConditional(row, {{"epoch", "seq"}, false, {}});
                } catch (const ConditionalSaveError &error) {
                    if (error.kind != ConditionalSaveError::Kind::Race)
                        ++errors;
                }
            }
        } catch (...) {
            ++errors;
        }
    };
    std::thread first(worker, 2), second(worker, 22);
    first.join();
    second.join();
    EXPECT_EQ(errors, 0u);
    Row loaded;
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.seq(), 41u);
    EXPECT_EQ(loaded.gold(), 41u);
}
TEST_F(MysqlIntegration, OnlineMigrationPreservesRenamedDataAndRejectsUnsafeDrift) {
    auto row = Target();
    row.set_data(std::string("saved\0blob", 10));
    db.Save(row);
    const auto quoted = QuoteIdentifier(table);
    db.Execute("ALTER TABLE " + quoted +
               " CHANGE COLUMN `data` `old_data` MEDIUMBLOB COMMENT 'pb:6'");
    auto migration = db.GenerateMigrationSql(row);
    EXPECT_NE(migration.find("CHANGE COLUMN `old_data` `data`"), std::string::npos);
    db.CreateOrUpdateTable(row);
    Row loaded;
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.data(), row.data());
    auto indexed = options;
    indexed.indexes = StringVector{"gold"};
    db.RegisterTable(Row::default_instance(), indexed);
    EXPECT_NE(db.GenerateMigrationSql(row).find("ADD KEY"), std::string::npos);
    db.CreateOrUpdateTable(row);
    EXPECT_TRUE(db.GenerateMigrationSql(row).empty());
    // Signedness drift must stop before any ADD/MODIFY is applied.
    db.Execute("ALTER TABLE " + quoted +
               " MODIFY COLUMN `gold` bigint NOT NULL DEFAULT 0 COMMENT 'pb:3'");
    EXPECT_THROW(db.GenerateMigrationSql(row), std::runtime_error);
    EXPECT_THROW(db.CreateOrUpdateTable(row), std::runtime_error);
    auto column =
        db.QueryOne(Statement{"SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS WHERE "
                              "TABLE_SCHEMA = ? AND TABLE_NAME = ? AND COLUMN_NAME = 'gold'",
                              {db.DbName(), table}});
    ASSERT_NE(column, nullptr);
    EXPECT_EQ(std::string((*column)[0], column->length(0)).find("unsigned"), std::string::npos);
    loaded.Clear();
    loaded.set_id(1);
    ASSERT_TRUE(db.FindOneByPrimaryKey(loaded));
    EXPECT_EQ(loaded.data(), row.data());
}

TEST_F(MysqlIntegration, TypedQueriesPaginationAndSingleRowCardinalityMatchGo) {
    auto first = Target(9007199254740992ULL), second = Target(9007199254740993ULL);
    db.BatchInsert({&first, &second});
    Row loaded;
    ASSERT_TRUE(db.FindOneByKV(loaded, "id", std::to_string(second.id())));
    EXPECT_EQ(loaded.id(), second.id());
    pbmysql::test::RuntimePage page;
    page.set_total(99);
    page.set_continuation("keep");
    db.FindPageByCursor(page, "WHERE `gold` >= ?", {uint64_t{0}}, "id",
                        SqlValue{std::to_string(first.id())}, 10);
    ASSERT_EQ(page.items_size(), 1);
    EXPECT_EQ(page.items(0).id(), second.id());
    EXPECT_EQ(page.total(), 99);
    EXPECT_EQ(page.continuation(), "keep");
    EXPECT_EQ(db.CountByWhereWithArgs(page, "`id` >= ?", {first.id()}), 2);
    EXPECT_TRUE(db.Exists(page, "`id` = ?", {second.id()}));
    EXPECT_THROW(db.FindOneByWhereWithArgs(loaded, "`gold` >= ?", {uint64_t{0}}),
                 MultipleRowsError);
    QueryOptions options;
    options.order_by = "`id` DESC";
    options.offset = 100;
    ASSERT_TRUE(db.FindOneWithOptions(loaded, "", {}, options));
    EXPECT_EQ(loaded.id(), second.id());
    db.BatchDelete({&first, &second});
    EXPECT_EQ(db.Count(page), 0);
    const auto canonical = db.DbName();
    EXPECT_THROW(db.SetDbName("wrong_database"), std::invalid_argument);
    EXPECT_EQ(db.DbName(), canonical);
}
TEST_F(MysqlIntegration, OldWriterPreservesColumnsOnlyKnownToNewerDescriptor) {
    auto row = Target();
    db.Save(row);
    google::protobuf::FileDescriptorSet files;
    Row::descriptor()->file()->CopyTo(files.add_file());
    auto *added = files.mutable_file(0)->mutable_message_type(0)->add_field();
    added->set_name("new_counter");
    added->set_number(100);
    added->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
    added->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);
    auto source = std::make_shared<DescriptorSet>(files);
    auto newer = source->NewMessage("pbmysql.test.RuntimeRow");
    Database newer_db;
    newer_db.Connect(Env("PBMYSQL_TEST_HOST", "127.0.0.1"), Env("PBMYSQL_TEST_USER", "root"),
                     Env("PBMYSQL_TEST_PASSWORD", ""),
                     Env("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
                     static_cast<unsigned>(std::stoul(Env("PBMYSQL_TEST_PORT", "3306"))));
    newer_db.RegisterTable(*newer, options);
    newer_db.CreateOrUpdateTable(*newer);
    const auto *fields = newer->GetDescriptor();
    const auto *reflection = newer->GetReflection();
    reflection->SetUInt64(newer.get(), fields->FindFieldByName("id"), row.id());
    reflection->SetString(newer.get(), fields->FindFieldByName("email"), row.email());
    reflection->SetUInt64(newer.get(), fields->FindFieldByName("new_counter"), 123456789);
    newer_db.Save(*newer);
    row.set_gold(77);
    db.Save(row);
    ASSERT_TRUE(newer_db.FindOneByPK(*newer));
    EXPECT_EQ(reflection->GetUInt64(*newer, fields->FindFieldByName("new_counter")), 123456789u);
    EXPECT_EQ(reflection->GetUInt64(*newer, fields->FindFieldByName("gold")), 77u);
}

TEST_F(MysqlIntegration, SharedClusterUsesCurrentReadsAcrossTwoDatabaseNodes) {
    const char *second_host = std::getenv("PBMYSQL_TEST_HOST2");
    if (!second_host)
        GTEST_SKIP() << "set PBMYSQL_TEST_HOST2 to run cross-node cluster verification";
    Database second;
    second.Connect(second_host, Env("PBMYSQL_TEST_USER", "root"), Env("PBMYSQL_TEST_PASSWORD", ""),
                   Env("PBMYSQL_TEST_DATABASE", "proto2mysql_test"),
                   static_cast<unsigned>(std::stoul(Env("PBMYSQL_TEST_PORT2", "3306"))));
    second.RegisterTable(Row::default_instance(), options);
    auto row = Target();
    row.set_epoch(2);
    row.set_seq(1);
    row.set_gold(21);
    ASSERT_TRUE(second.SaveConditional(row, {{"epoch", "seq"}, false, {}}).Applied());
    auto replay = row;
    replay.set_epoch(1);
    replay.set_seq(999);
    replay.set_gold(999);
    auto result = db.SaveConditional(replay, {{"epoch", "seq"}, false, {}});
    EXPECT_EQ(result.outcome, SaveOutcome::Rejected);
    EXPECT_EQ(result.stored, (std::vector<uint64_t>{2, 1}));
    Row current;
    current.set_id(row.id());
    const auto committed = db.Transaction([&](Database &tx) {
        EXPECT_TRUE(tx.FindOneByPK(current));
        auto latest = row;
        latest.set_epoch(3);
        latest.set_gold(31);
        EXPECT_TRUE(second.SaveConditional(latest, {{"epoch", "seq"}, false, {}}).Applied());
        auto old = row;
        old.set_seq(888);
        const auto rejected = tx.SaveConditional(old, {{"epoch", "seq"}, false, {}});
        EXPECT_EQ(rejected.outcome, SaveOutcome::Rejected);
        EXPECT_EQ(rejected.stored, (std::vector<uint64_t>{3, 1}));
        return true;
    });
    EXPECT_TRUE(committed);
    current.Clear();
    current.set_id(row.id());
    ASSERT_TRUE(db.FindOneByPK(current));
    EXPECT_EQ(current.epoch(), 3u);
    EXPECT_EQ(current.gold(), 31u);
}

TEST_F(MysqlIntegration, OneDatabaseInstanceSerializesConcurrentOperationsAndResults) {
    auto seed = Target();
    db.Save(seed);
    std::atomic<unsigned> failures{0};
    auto increment = [&] {
        try {
            Row key;
            key.set_id(seed.id());
            for (unsigned i = 0; i < 12; ++i)
                db.IncrByPK(key, "gold", 1);
        } catch (...) {
            ++failures;
        }
    };
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 6; ++i)
        workers.emplace_back(increment);
    for (auto &worker : workers)
        worker.join();
    ASSERT_EQ(failures, 0u);
    Row current;
    current.set_id(seed.id());
    ASSERT_TRUE(db.FindOneByPK(current));
    EXPECT_EQ(current.gold(), 72u);
    workers.clear();
    for (unsigned i = 0; i < 6; ++i)
        workers.emplace_back([&, i] {
            try {
                auto row = Target(100 + i);
                const auto result = db.ExecuteStatement(db.Builder(row).Insert(row));
                if (result.affected_rows != 1)
                    ++failures;
            } catch (...) {
                ++failures;
            }
        });
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(failures, 0u);
    EXPECT_EQ(db.Count(current), 7);
}
TEST_F(MysqlIntegration, SharedConnectionCannotJoinAnotherThreadsUncommittedTransaction) {
    auto row = Target();
    db.Save(row);
    std::promise<void> attempted;
    auto attempted_future = attempted.get_future();
    std::future<void> worker;
    const auto committed = db.Transaction([&](Database &transaction) {
        auto changed = row;
        changed.set_gold(100);
        transaction.Save(changed);
        worker = std::async(std::launch::async, [&] {
            attempted.set_value();
            db.IncrByPK(row, "gold", 1);
        });
        attempted_future.wait();
        EXPECT_EQ(worker.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
        return false;
    });
    EXPECT_FALSE(committed);
    worker.get();
    Row current;
    current.set_id(row.id());
    ASSERT_TRUE(db.FindOneByPK(current));
    EXPECT_EQ(current.gold(), 1u);
}
} // namespace
