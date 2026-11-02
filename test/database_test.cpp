#include "runtime_test.pb.h"
#include "src/mysql_wrapper/cache_codec.h"
#include "src/mysql_wrapper/database.h"
#include <deque>
#include <functional>
#include <gtest/gtest.h>
#include <limits>
#include <unordered_map>

namespace {
using namespace pbmysql;
using Row = pbmysql::test::RuntimeRow;
using Cells = std::vector<std::optional<std::string>>;
struct FakeDatabase : Database {
    std::deque<uint64_t> writes;
    std::deque<std::optional<Cells>> reads;
    std::deque<std::vector<Cells>> row_sets;
    std::vector<Statement> statements;
    std::vector<std::string> controls;
    std::function<void(const Statement &)> on_write;
    std::function<void(const std::string &)> on_control;
    uint64_t affected = 0;
    FakeDatabase() { RegisterTable(Row::default_instance()); }
    void Execute(const Statement &statement) override {
        statements.push_back(statement);
        if (on_write)
            on_write(statement);
        if (writes.empty())
            throw std::runtime_error("unexpected write");
        affected = writes.front();
        writes.pop_front();
    }
    void Execute(const std::string &sql) override {
        controls.push_back(sql);
        if (on_control)
            on_control(sql);
    }
    ResultRowPtr QueryOne(const Statement &statement) override {
        statements.push_back(statement);
        if (reads.empty())
            throw std::runtime_error("unexpected read");
        auto row = reads.front();
        reads.pop_front();
        if (!row)
            return nullptr;
        return std::make_unique<ResultRow>(*row);
    }
    uint64_t AffectedRows() override { return affected; }
    void QueryResultRowProcessor(const Statement &statement,
                                 const ResultRowProcessor &processor) override {
        if (row_sets.empty()) {
            auto row = QueryOne(statement);
            if (row)
                processor(row);
            return;
        }
        statements.push_back(statement);
        auto rows = row_sets.front();
        row_sets.pop_front();
        for (auto &cells : rows) {
            auto row = std::make_unique<ResultRow>(std::move(cells));
            if (!processor(row))
                break;
        }
    }
};
struct MemoryCache : Cache {
    std::unordered_map<std::string, std::string> values;
    std::vector<std::string> deleted;
    bool fail = false;
    bool Get(const std::string &key, std::string *value) override {
        if (fail)
            throw std::runtime_error("cache down");
        auto it = values.find(key);
        if (it == values.end())
            return false;
        *value = it->second;
        return true;
    }
    void Set(const std::string &key, const std::string &value, int) override {
        if (fail)
            throw std::runtime_error("cache down");
        values[key] = value;
    }
    void Del(const std::string &key) override {
        if (fail)
            throw std::runtime_error("cache down");
        values.erase(key);
        deleted.push_back(key);
    }
};
Row Target() {
    Row row;
    row.set_id(1);
    row.set_email("e");
    row.set_epoch(7);
    row.set_seq(9);
    return row;
}
SaveOptions Fence(bool equal = false) { return {{"epoch", "seq"}, equal, {}}; }
TEST(DatabaseSave, FullRowSaveWritesZeroAndUsesExactPrimaryKey) {
    FakeDatabase db;
    db.writes = {1};
    auto row = Target();
    db.Save(row);
    ASSERT_EQ(db.statements.size(), 1u);
    const auto &statement = db.statements[0];
    EXPECT_EQ(statement.sql.find("ON DUPLICATE"), std::string::npos);
    EXPECT_NE(statement.sql.find("`gold` = ?"), std::string::npos);
    EXPECT_EQ(std::get<uint64_t>(statement.args[1]), 0u);
    EXPECT_EQ(std::get<uint64_t>(statement.args.back()), 1u);
}
TEST(DatabaseSave, AlternateUniqueConflictRethrows1062WithoutUpsert) {
    FakeDatabase db;
    db.writes = {0, 0};
    db.reads = {std::nullopt};
    db.on_write = [](const Statement &statement) {
        if (statement.sql.find("INSERT INTO") == 0)
            throw MysqlError(1062, "duplicate");
    };
    try {
        db.Save(Target());
        FAIL() << "duplicate must be returned";
    } catch (const MysqlError &error) {
        EXPECT_EQ(error.value(), 1062u);
    }
    ASSERT_EQ(db.statements.size(), 4u);
    for (const auto &statement : db.statements)
        EXPECT_EQ(statement.sql.find("ON DUPLICATE"), std::string::npos);
    EXPECT_NE(db.statements.back().sql.find("FOR UPDATE"), std::string::npos);
}
TEST(DatabaseSave, ConcurrentPrimaryInsertRetriesUpdate) {
    FakeDatabase db;
    db.writes = {0, 1};
    db.reads = {std::nullopt, Cells{"6", "100"}};
    db.on_write = [](const Statement &statement) {
        if (statement.sql.find("INSERT INTO") == 0)
            throw MysqlError(1062, "concurrent PK insert");
    };
    auto result = db.SaveConditional(Target(), Fence());
    EXPECT_EQ(result.outcome, SaveOutcome::Updated);
    EXPECT_TRUE(result.Applied());
}
TEST(DatabaseSave, RejectedFenceLeavesCacheAndDoesNotInsert) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    db.writes = {0};
    db.reads = {Cells{"8", "2"}};
    auto result = db.SaveConditional(Target(), Fence());
    EXPECT_EQ(result.outcome, SaveOutcome::Rejected);
    EXPECT_EQ(result.stored, (std::vector<uint64_t>{8, 2}));
    EXPECT_FALSE(result.Applied());
    EXPECT_EQ(db.statements.size(), 2u);
    EXPECT_TRUE(cache.deleted.empty());
}
TEST(DatabaseSave, LexicographicGuardBindsFullUnsignedRange) {
    FakeDatabase db;
    db.writes = {1};
    auto row = Target();
    row.set_epoch(std::numeric_limits<uint64_t>::max());
    db.SaveConditional(row, Fence(true));
    const auto &statement = db.statements.front();
    EXPECT_NE(statement.sql.find("(`epoch` < ?) OR (`epoch` = ? AND `seq` <= ?)"),
              std::string::npos);
    ASSERT_GE(statement.args.size(), 3u);
    EXPECT_EQ(std::get<uint64_t>(statement.args[statement.args.size() - 3]),
              std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(std::get<uint64_t>(statement.args.back()), 9u);
}
TEST(DatabaseSave, EqualFenceUsesCurrentReadBytewiseMatch) {
    FakeDatabase db;
    db.writes = {0};
    db.reads = {Cells{"7", "9"}, Cells{"1"}};
    auto result = db.SaveConditional(Target(), Fence(true));
    EXPECT_EQ(result.outcome, SaveOutcome::Unchanged);
    EXPECT_TRUE(result.Applied());
    EXPECT_NE(db.statements.back().sql.find("BINARY `data` <=> BINARY ?"), std::string::npos);
    EXPECT_NE(db.statements.back().sql.find("FOR UPDATE"), std::string::npos);
}
TEST(DatabaseSave, BoundedRaceIsReportedAndInvalidatesCache) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    db.writes = {0, 0};
    db.reads = {Cells{"6", "9"}, Cells{"6", "10"}};
    try {
        db.SaveConditional(Target(), Fence());
        FAIL();
    } catch (const ConditionalSaveError &error) {
        EXPECT_EQ(error.kind, ConditionalSaveError::Kind::Race);
        EXPECT_EQ(error.stored, (std::vector<uint64_t>{6, 10}));
    }
    EXPECT_EQ(cache.deleted.size(), 1u);
}
TEST(DatabaseSave, StableMismatchIsDifferentFromRace) {
    FakeDatabase db;
    db.writes = {0, 0};
    db.reads = {Cells{"7", "9"}, std::nullopt, Cells{"7", "9"}, std::nullopt};
    try {
        db.SaveConditional(Target(), Fence(true));
        FAIL();
    } catch (const ConditionalSaveError &error) {
        EXPECT_EQ(error.kind, ConditionalSaveError::Kind::MatchMismatch);
    }
}
TEST(DatabaseSave, InvalidOptionsFailBeforeAnySql) {
    FakeDatabase db;
    auto row = Target();
    EXPECT_THROW(db.SaveConditional(row, {{"email"}, false, {}}), std::invalid_argument);
    EXPECT_THROW(db.SaveConditional(row, {{"id"}, false, {}}), std::invalid_argument);
    EXPECT_THROW(db.SaveConditional(row, {{"epoch", "epoch"}, false, {}}), std::invalid_argument);
    EXPECT_THROW(db.SaveConditional(row, {{"epoch"}, false, {"epoch"}}), std::invalid_argument);
    row.set_epoch(0);
    row.set_seq(0);
    EXPECT_THROW(db.SaveConditional(row, Fence()), std::invalid_argument);
    EXPECT_TRUE(db.statements.empty());
}
TEST(DatabaseSave, PreserveFieldsOnlyAffectsUpdate) {
    FakeDatabase db;
    db.writes = {0, 1};
    db.reads = {std::nullopt};
    auto result = db.SaveConditional(Target(), {{}, false, {"epoch", "seq"}});
    EXPECT_EQ(result.outcome, SaveOutcome::Inserted);
    EXPECT_EQ(db.statements.front().sql.find("`epoch` = ?"), std::string::npos);
    EXPECT_NE(db.statements.back().sql.find("`epoch`"), std::string::npos);
}
TEST(DatabaseSave, InsertIgnoreOnlyCatchesDuplicate) {
    FakeDatabase db;
    db.on_write = [](const Statement &) { throw MysqlError(1062, "duplicate"); };
    EXPECT_FALSE(db.InsertIgnore(Target()));
    db.on_write = [](const Statement &) { throw MysqlError(1406, "too long"); };
    EXPECT_THROW(db.InsertIgnore(Target()), MysqlError);
}
TEST(DatabaseSave, VersionUpdateHandlesCollectionsWithoutSingularReflection) {
    FakeDatabase db;
    pbmysql::test::CollectionRow row;
    row.set_id(5);
    row.set_version(2);
    row.add_tags("tag");
    (*row.mutable_counts())["n"] = 3;
    db.RegisterTable(row);
    db.writes = {1};
    EXPECT_TRUE(db.UpdateIfVersion(row, "version"));
    ASSERT_EQ(db.statements.size(), 1u);
    EXPECT_NE(db.statements.front().sql.find("`tags` = ?"), std::string::npos);
    EXPECT_NE(db.statements.front().sql.find("`counts` = ?"), std::string::npos);
    EXPECT_EQ(std::get<uint64_t>(db.statements.front().args.back()), 2u);
}
TEST(DatabaseSave, CollectionEqualityComparesTheBytesActuallyWritten) {
    FakeDatabase db;
    pbmysql::test::CollectionRow row;
    row.set_id(5);
    row.set_version(2);
    row.add_tags("tag");
    for (unsigned i = 0; i < 100; ++i)
        (*row.mutable_counts())["key" + std::to_string(i)] = i;
    db.RegisterTable(row);
    db.writes = {0};
    db.reads = {Cells{"5"}, Cells{"1"}};
    auto result = db.SaveConditional(row);
    EXPECT_EQ(result.outcome, SaveOutcome::Unchanged);
    ASSERT_EQ(db.statements.size(), 3u);
    // The guard reads PK first; the row comparison then binds exactly the
    // collection bytes used in UPDATE, even for unordered protobuf maps.
    EXPECT_EQ(db.statements[0].args[1], db.statements[2].args[2]);
    EXPECT_EQ(db.statements[0].args[2], db.statements[2].args[3]);
}
TEST(DatabaseTransactions, ExceptionRollsBackAndDoesNotInvalidate) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    db.writes = {1};
    EXPECT_THROW(db.Transaction([](Database &tx) {
        tx.Save(Target());
        throw std::runtime_error("failed");
        return true;
    }),
                 std::runtime_error);
    EXPECT_EQ(db.controls, (std::vector<std::string>{"START TRANSACTION", "ROLLBACK"}));
    EXPECT_TRUE(cache.deleted.empty());
    EXPECT_TRUE(db.Transaction([](Database &) { return true; }));
}
TEST(DatabaseTransactions, CommitDefersAndDeduplicatesInvalidation) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    db.writes = {1, 1};
    const auto committed = db.Transaction([&](Database &tx) {
        tx.Save(Target());
        tx.Save(Target());
        EXPECT_TRUE(cache.deleted.empty());
        return true;
    });
    EXPECT_TRUE(committed);
    EXPECT_EQ(cache.deleted.size(), 1u);
    EXPECT_EQ(db.controls.back(), "COMMIT");
}
TEST(DatabaseTransactions, NestedTransactionsAndDdlFailBeforeImplicitCommit) {
    FakeDatabase db;
    const auto committed = db.Transaction([&](Database &tx) {
        EXPECT_THROW(tx.Transaction([](Database &) { return true; }), std::logic_error);
        EXPECT_THROW(tx.CreateOrUpdateTable(Target()), std::logic_error);
        return true;
    });
    EXPECT_TRUE(committed);
    EXPECT_EQ(db.controls.size(), 2u);
    Row row;
    EXPECT_THROW(db.FindOneByPrimaryKeyForUpdate(row), std::logic_error);
}
TEST(DatabaseTransactions, CommitErrorInvalidatesUnknownOutcomeAndResetsState) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    db.writes = {1};
    db.on_control = [](const std::string &sql) {
        if (sql == "COMMIT")
            throw MysqlError(2013, "lost connection");
    };
    EXPECT_THROW(db.Transaction([](Database &tx) {
        tx.Save(Target());
        return true;
    }),
                 MysqlError);
    EXPECT_EQ(cache.deleted.size(), 1u);
    db.on_control = {};
    EXPECT_TRUE(db.Transaction([](Database &) { return true; }));
}
TEST(DatabaseCache, CorruptEnvelopePreservesPrimaryKeyForDbFallback) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    auto row = Target();
    auto key = db.CacheKey(row);
    cache.values[key] = EncodeCacheEntry(row) + std::string(1, '\xff');
    db.reads = {Cells{"1", "db-email", "3", "7", "9", std::string("a\0b", 3), std::nullopt}};
    ASSERT_TRUE(db.FindOneByPrimaryKey(row));
    EXPECT_EQ(std::get<uint64_t>(db.statements[0].args[0]), 1u);
    EXPECT_EQ(row.email(), "db-email");
    EXPECT_EQ(row.data(), std::string("a\0b", 3));
}
TEST(DatabaseCache, EnvelopeIsCompatibleAndRejectsMissingWriterFields) {
    auto row = Target();
    auto encoded = EncodeCacheEntry(row);
    Row out;
    ASSERT_TRUE(DecodeCacheEntry(encoded, out));
    EXPECT_EQ(out.SerializeAsString(), row.SerializeAsString());
    std::string old("P2MC\x01", 5);
    AppendCacheVarint(old, 1);
    AppendCacheVarint(old, 1);
    old += row.SerializeAsString();
    out.set_id(55);
    EXPECT_FALSE(DecodeCacheEntry(old, out));
    EXPECT_EQ(out.id(), 55u);
    EXPECT_FALSE(DecodeCacheEntry(row.SerializeAsString(), out));
    EXPECT_FALSE(DecodeCacheEntry(std::string("P2MC\x01\xff", 6), out));
}
TEST(DatabaseCache, CompositeComponentsAndTableNameAreEscaped) {
    FakeDatabase db;
    db.RegisterTable(pbmysql::test::CompositeRow::default_instance());
    pbmysql::test::CompositeRow a, b;
    a.set_realm("x:y");
    a.set_id("z%");
    b.set_realm("x");
    b.set_id("y:z%");
    EXPECT_NE(db.CacheKey(a), db.CacheKey(b));
    EXPECT_EQ(db.CacheKey(a), "pb:composite_rows:x%3Ay:z%25");
}
TEST(DatabaseCache, CacheFailureDegradesAndTransactionBypassesReads) {
    FakeDatabase db;
    MemoryCache cache;
    cache.fail = true;
    db.EnableCache(&cache, 60);
    auto row = Target();
    db.reads = {Cells{"1", "e", "0", "7", "9", "", std::nullopt}};
    EXPECT_TRUE(db.FindOneByPrimaryKey(row));
    cache.fail = false;
    cache.values[db.CacheKey(row)] = EncodeCacheEntry(row);
    db.reads = {std::nullopt};
    EXPECT_TRUE(db.Transaction([&](Database &tx) {
        EXPECT_FALSE(tx.FindOneByPrimaryKey(row));
        return true;
    }));
}
TEST(DatabaseRegistration, AutoRegistrationAndListShapeAreChecked) {
    FakeDatabase db;
    db.RegisterAllTables(Row::descriptor()->file());
    EXPECT_EQ(db.tables().size(), 3u);
    Row wrong;
    EXPECT_THROW(db.FindAll<Row>(wrong), std::invalid_argument);
}
TEST(DatabaseSchema, LongTableNamesUseBoundedStableAdvisoryLockNames) {
    FakeDatabase db;
    db.SetDbName("game");
    TableOptions options;
    options.table_name = std::string(64, 't');
    db.RegisterTable(Row::default_instance(), options);
    db.reads = {Cells{"1"}, Cells{"0"}, Cells{"1"}};
    db.CreateOrUpdateTable(Target());
    ASSERT_EQ(db.statements.size(), 3u);
    EXPECT_LE(std::get<std::string>(db.statements.front().args[0]).size(), 64u);
    EXPECT_EQ(db.statements.front().args[0], db.statements.back().args[0]);
}
TEST(DatabaseSchema, UnconfirmedReleaseIsReported) {
    FakeDatabase db;
    db.reads = {Cells{"1"}, Cells{"0"}, Cells{"0"}};
    EXPECT_THROW(db.CreateOrUpdateTable(Target()), std::runtime_error);
    EXPECT_NE(db.statements.back().sql.find("RELEASE_LOCK"), std::string::npos);
}
TEST(DatabaseSchema, MigrationFailureStillReleasesLockAndPreservesOriginalError) {
    FakeDatabase db;
    db.reads = {Cells{"1"}, Cells{"0"}, Cells{"1"}};
    db.on_control = [](const std::string &) { throw MysqlError(1064, "DDL failed"); };
    try {
        db.CreateOrUpdateTable(Target());
        FAIL();
    } catch (const MysqlError &error) {
        EXPECT_EQ(error.value(), 1064u);
    }
    EXPECT_NE(db.statements.back().sql.find("RELEASE_LOCK"), std::string::npos);
}

TEST(DatabaseQueries, SingleRowQueriesRejectMultipleRowsWithoutChangingTheDestination) {
    FakeDatabase db;
    auto row = Target();
    row.set_email("unchanged");
    db.row_sets = {{{"1", "first", "0", "7", "9", "", std::nullopt},
                    {"2", "second", "0", "7", "9", "", std::nullopt}}};
    EXPECT_THROW(db.FindOneByWhereWithArgs(row, "`gold` >= ?", {uint64_t{0}}), MultipleRowsError);
    EXPECT_EQ(row.email(), "unchanged");
    EXPECT_EQ(db.statements.front().sql.find("LIMIT 1"), std::string::npos);
}
TEST(DatabaseQueries, SingleRowOptionsResetOffsetAndKeepTypedParameters) {
    FakeDatabase db;
    Row row;
    db.reads = {Cells{"1", "e", "0", "7", "9", "", std::nullopt}};
    QueryOptions options;
    options.limit = 99;
    options.offset = 100;
    options.order_by = "`id` DESC";
    ASSERT_TRUE(db.FindOneWithOptions(row, "`id` = ?", {uint64_t{1}}, options));
    EXPECT_NE(db.statements.front().sql.find("LIMIT 1"), std::string::npos);
    EXPECT_EQ(db.statements.front().sql.find("OFFSET"), std::string::npos);
    EXPECT_EQ(std::get<uint64_t>(db.statements.front().args[0]), 1u);
}
TEST(DatabaseQueries, ListsAllowScalarMetadataAndInferElementTypes) {
    FakeDatabase db;
    pbmysql::test::RuntimePage page;
    page.set_total(200);
    page.set_continuation("keep");
    page.add_items()->set_id(99);
    db.row_sets = {{{"1", "first", "0", "7", "9", "", std::nullopt},
                    {"2", "second", "0", "7", "9", "", std::nullopt}}};
    db.FindAllByWhereWithArgs(page, "`gold` >= ?", {uint64_t{0}});
    ASSERT_EQ(page.items_size(), 2);
    EXPECT_EQ(page.items(0).id(), 1u);
    EXPECT_EQ(page.items(1).id(), 2u);
    EXPECT_EQ(page.total(), 200);
    EXPECT_EQ(page.continuation(), "keep");
    db.FindAllByPKIn(page, {});
    EXPECT_EQ(page.items_size(), 0);
    EXPECT_EQ(page.total(), 200);
}
TEST(DatabaseQueries, TypedKvAndCursorValuesPreserveUnsignedPrecision) {
    FakeDatabase db;
    Row row;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    db.reads = {std::nullopt};
    EXPECT_FALSE(db.FindOneByKV(row, "id", std::to_string(maximum)));
    EXPECT_EQ(std::get<uint64_t>(db.statements.back().args[0]), maximum);
    pbmysql::test::RuntimePage page;
    db.row_sets = {{}};
    db.FindPageByCursor(page, "WHERE `gold` >= ?", {uint64_t{5}}, "id",
                        SqlValue{std::to_string(maximum)}, 20);
    EXPECT_EQ(std::get<uint64_t>(db.statements.back().args[0]), 5u);
    EXPECT_EQ(std::get<uint64_t>(db.statements.back().args[1]), maximum);
    EXPECT_NE(db.statements.back().sql.find("ORDER BY `id` ASC LIMIT 20"), std::string::npos);
}
TEST(DatabaseQueries, AggregatesAcceptListsAndParameterizedConditions) {
    FakeDatabase db;
    pbmysql::test::RuntimePage page;
    db.reads = {Cells{"3"}, Cells{"1"}};
    EXPECT_EQ(db.CountByWhereWithArgs(page, "`gold` > ?", {uint64_t{0}}), 3);
    EXPECT_TRUE(db.Exists(page, "`id` = ?", {uint64_t{1}}));
    EXPECT_EQ(std::get<uint64_t>(db.statements.back().args[0]), 1u);
}
TEST(DatabaseQueries, MultiQueryPreflightsEveryMessageAndKeepsArguments) {
    FakeDatabase db;
    Row a, b;
    EXPECT_THROW(db.FindMultiByWhereClauses({}), std::invalid_argument);
    EXPECT_THROW(db.FindMultiByWhereClauses({{&a, "`id` = ?", {uint64_t{1}}}, {nullptr, "", {}}}),
                 std::invalid_argument);
    EXPECT_TRUE(db.statements.empty());
    db.reads = {Cells{"1", "a", "0", "7", "9", "", std::nullopt},
                Cells{"2", "b", "0", "7", "9", "", std::nullopt}};
    db.FindMultiByWhereClauses({{&a, "`id` = ?", {uint64_t{1}}}, {&b, "`id` = ?", {uint64_t{2}}}});
    EXPECT_EQ(a.id(), 1u);
    EXPECT_EQ(b.id(), 2u);
}
TEST(DatabaseBatch, MixedDescriptorsFailBeforeAnyWrite) {
    FakeDatabase db;
    auto row = Target();
    pbmysql::test::CompositeRow different;
    db.RegisterTable(different);
    EXPECT_THROW(db.BatchInsert({&row, &different}), std::invalid_argument);
    EXPECT_THROW(db.BatchSave({&row, &different}), std::invalid_argument);
    EXPECT_THROW(db.BatchDelete({&row, &different}), std::invalid_argument);
    EXPECT_TRUE(db.statements.empty());
}
TEST(DatabaseBatch, DeleteUsesEveryComponentOfTheCompositePrimaryKey) {
    FakeDatabase db;
    db.RegisterTable(pbmysql::test::CompositeRow::default_instance());
    pbmysql::test::CompositeRow first, second;
    first.set_realm("r1");
    first.set_id("1");
    second.set_realm("r2");
    second.set_id("1");
    db.writes = {2};
    db.BatchDelete({&first, &second});
    ASSERT_EQ(db.statements.size(), 1u);
    EXPECT_NE(db.statements[0].sql.find("(`realm`, `id`) IN ((?, ?), (?, ?))"), std::string::npos);
    EXPECT_EQ(db.statements[0].args, (std::vector<SqlValue>{std::string("r1"), std::string("1"),
                                                            std::string("r2"), std::string("1")}));
}
TEST(DatabaseBatch, FailedLaterDeleteChunkInvalidatesCompletedRows) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    std::vector<Row> rows(1001);
    std::vector<const google::protobuf::Message *> messages;
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].set_id(i + 1);
        messages.push_back(&rows[i]);
    }
    unsigned writes = 0;
    db.on_write = [&](const Statement &) {
        if (++writes == 2)
            throw MysqlError(1205, "timeout");
    };
    db.writes = {1000};
    EXPECT_THROW(db.BatchDelete(messages), MysqlError);
    EXPECT_EQ(writes, 2u);
    EXPECT_EQ(cache.deleted.size(), 1000u);
}
TEST(DatabaseVersion, NumericVersionsAndEmptyUpdateRulesMatchGo) {
    FakeDatabase db;
    pbmysql::test::NumericVersionRow row;
    row.set_id(1);
    row.set_version(0.1f);
    row.set_label("label");
    TableOptions options;
    options.table_name = "numeric_versions";
    options.primary_keys = StringVector{"id"};
    db.RegisterTable(row, options);
    db.writes = {1};
    EXPECT_TRUE(db.UpdateIfVersion(row, "version"));
    EXPECT_EQ(std::get<double>(db.statements.back().args.back()), static_cast<double>(0.1f));
    row.clear_label();
    EXPECT_THROW(db.UpdateIfVersion(row, "version"), std::invalid_argument);
    EXPECT_THROW(db.UpdateFieldsIfVersion(row, "version", {}), std::invalid_argument);
    db.writes = {1};
    EXPECT_TRUE(db.UpdateFieldsIfVersion(row, "version", {"version"}));
    EXPECT_THROW(db.DecrByPKIfEnough(Target(), "gold", 0), std::invalid_argument);
}
TEST(DatabaseCache, BatchedInvalidationValidatesBeforeDeletingAnything) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    auto row = Target();
    EXPECT_THROW(db.InvalidateCache(std::vector<const google::protobuf::Message *>{&row, nullptr}),
                 std::invalid_argument);
    EXPECT_TRUE(cache.deleted.empty());
    db.InvalidateCache(std::vector<const google::protobuf::Message *>{&row});
    EXPECT_EQ(cache.deleted.size(), 1u);
}
TEST(DatabaseCreate, ConcurrentInsertRemainsADuplicateError) {
    FakeDatabase db;
    auto row = Target();
    db.reads = {std::nullopt};
    db.on_write = [](const Statement &) { throw MysqlError(1062, "concurrent insert"); };
    bool created = true;
    EXPECT_THROW(db.FindOrCreate(row, &created), MysqlError);
    EXPECT_FALSE(created);
}

TEST(DatabaseCache, PrimaryKeyChangesInvalidateBothOldAndNewIdentity) {
    FakeDatabase db;
    MemoryCache cache;
    db.EnableCache(&cache, 60);
    auto row = Target();
    db.writes = {1};
    db.UpdateKVByPK(row, "id", uint64_t{2});
    EXPECT_EQ(cache.deleted, (std::vector<std::string>{"pb:runtime_rows:1", "pb:runtime_rows:2"}));
    cache.deleted.clear();
    db.writes = {1};
    db.IncrByPK(row, "id", 2);
    EXPECT_EQ(cache.deleted, (std::vector<std::string>{"pb:runtime_rows:1", "pb:runtime_rows:3"}));
}

TEST(DatabaseQueries, FailedListQueryOrDecodePreservesRowsAndScalarMetadata) {
    FakeDatabase db;
    pbmysql::test::RuntimePage page;
    page.set_total(9);
    page.add_items()->set_id(77);
    EXPECT_THROW(db.FindAll(page), std::runtime_error);
    ASSERT_EQ(page.items_size(), 1);
    EXPECT_EQ(page.items(0).id(), 77u);
    EXPECT_EQ(page.total(), 9);
    db.row_sets = {{{"1", "first", "0", "7", "9", "", std::nullopt},
                    {"not-an-id", "bad", "0", "7", "9", "", std::nullopt}}};
    EXPECT_THROW(db.FindAll(page), std::invalid_argument);
    ASSERT_EQ(page.items_size(), 1);
    EXPECT_EQ(page.items(0).id(), 77u);
    EXPECT_EQ(page.total(), 9);
}
} // namespace
