#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <gtest/gtest.h>

#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/message_table.h"

namespace {
using namespace google::protobuf;
using pbmysql::MessageTable;
using pbmysql::TableOptions;

struct SchemaFixture {
    DescriptorPool pool{DescriptorPool::generated_pool()};
    DynamicMessageFactory factory{&pool};
    FileDescriptorProto file;
    DescriptorProto *message;
    SchemaFixture() {
        file.set_name("schema_fixture.proto");
        file.set_syntax("proto3");
        file.add_dependency("proto2mysql_option.proto");
        file.add_dependency("db_base.proto");
        message = file.add_message_type();
        message->set_name("Record");
        message->mutable_options()->SetExtension(proto2mysql::table_name, "record");
        message->mutable_options()->SetExtension(proto2mysql::primary_key, "id");
        Add("id", 1, FieldDescriptorProto::TYPE_UINT64);
        Add("name", 2, FieldDescriptorProto::TYPE_STRING);
        Add("data", 3, FieldDescriptorProto::TYPE_BYTES);
    }
    FieldDescriptorProto *Add(const std::string &name, int number,
                              FieldDescriptorProto::Type type) {
        auto *f = message->add_field();
        f->set_name(name);
        f->set_number(number);
        f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
        f->set_type(type);
        return f;
    }
    const Message &Build() {
        const auto *f = pool.BuildFile(file);
        if (!f)
            throw std::runtime_error("invalid fixture descriptor");
        return *factory.GetPrototype(f->message_type(0));
    }
};
void Column(MessageTable &table, const std::string &name, const std::string &type,
            const std::string &comment, const std::string &nullable = "NO",
            const char *default_value = "0", const std::string &extra = "",
            const char *collation = nullptr) {
    const char *row[] = {name.c_str(),  type.c_str(),  comment.c_str(), nullable.c_str(),
                         default_value, extra.c_str(), collation};
    unsigned long lengths[7]{};
    for (int i = 0; i < 7; ++i)
        if (row[i])
            lengths[i] = static_cast<unsigned long>(std::char_traits<char>::length(row[i]));
    table.OnSelectColumn(const_cast<MYSQL_ROW>(row), lengths, 7);
}
void Index(MessageTable &table, const std::string &name, bool unique, int sequence,
           const std::string &column, const char *prefix = nullptr) {
    const auto seq = std::to_string(sequence);
    const char *row[] = {
        name.c_str(), unique ? "0" : "1", seq.c_str(), column.c_str(), prefix, "BTREE", "A"};
    unsigned long lengths[7]{};
    for (int i = 0; i < 7; ++i)
        if (row[i])
            lengths[i] = static_cast<unsigned long>(std::char_traits<char>::length(row[i]));
    table.OnSelectIndex(const_cast<MYSQL_ROW>(row), lengths, 7);
}
void Existing(SchemaFixture &, MessageTable &table) {
    Column(table, "id", "bigint unsigned", "pb:1");
    Column(table, "name", "mediumtext", "pb:2", "YES", nullptr);
    Column(table, "data", "mediumblob", "pb:3", "YES", nullptr);
}

TEST(Schema, ReadsModernAndLegacyOptionsAndOwnsPrototype) {
    SchemaFixture f;
    const auto &prototype = f.Build();
    auto temporary = std::unique_ptr<Message>(prototype.New());
    MessageTable table(*temporary);
    temporary.reset();
    EXPECT_EQ(table.DefaultInstance().GetDescriptor(), prototype.GetDescriptor());
    EXPECT_EQ(table.TableName(), "record");
    EXPECT_EQ(table.PrimaryKeys(), pbmysql::StringVector({"id"}));
    const auto ddl = table.CreateTableSql();
    EXPECT_NE(ddl.find("`name` MEDIUMTEXT"), std::string::npos);
    EXPECT_NE(ddl.find("`data` MEDIUMBLOB"), std::string::npos);
    EXPECT_NE(ddl.find("COMMENT 'pb:3'"), std::string::npos);
    SchemaFixture legacy;
    legacy.message->mutable_options()->ClearExtension(proto2mysql::table_name);
    legacy.message->mutable_options()->ClearExtension(proto2mysql::primary_key);
    legacy.message->mutable_options()->SetExtension(OptionTableName, "old_record");
    legacy.message->mutable_options()->SetExtension(OptionPrimaryKey, "id");
    EXPECT_EQ(MessageTable(legacy.Build()).TableName(), "old_record");
}
TEST(Schema, KeyColumnsUseFullBinaryIdentityAndValidateUtf8) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::primary_key, "name");
    f.message->mutable_options()->SetExtension(proto2mysql::unique_key, "data");
    f.message->mutable_field(1)->mutable_options()->SetExtension(proto2mysql::max_length, 2);
    const auto &prototype = f.Build();
    MessageTable table(prototype);
    const auto ddl = table.CreateTableSql();
    EXPECT_NE(
        ddl.find("VARCHAR(2) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin NOT NULL DEFAULT ''"),
        std::string::npos);
    EXPECT_NE(ddl.find("VARBINARY(191) NOT NULL DEFAULT ''"), std::string::npos);
    auto m = std::unique_ptr<Message>(prototype.New());
    auto *field = m->GetDescriptor()->FindFieldByName("name");
    m->GetReflection()->SetString(m.get(), field, "ab");
    EXPECT_NO_THROW(table.ValidateMessage(*m));
    m->GetReflection()->SetString(m.get(), field, "abc");
    EXPECT_THROW(table.ValidateMessage(*m), std::invalid_argument);
    m->GetReflection()->SetString(m.get(), field, std::string("\xc0\xaf", 2));
    EXPECT_THROW(table.ValidateMessage(*m), std::invalid_argument);
}
TEST(Schema, RejectsInvalidOptionsBeforeProducingDDL) {
    SchemaFixture f;
    const auto &p = f.Build();
    TableOptions bad;
    bad.primary_keys = pbmysql::StringVector{"id", "missing"};
    EXPECT_THROW(MessageTable(p, bad), std::invalid_argument);
    bad = {};
    bad.auto_increment_key = "name";
    EXPECT_THROW(MessageTable(p, bad), std::invalid_argument);
    bad = {};
    bad.nullable_fields = pbmysql::StringVector{"id"};
    EXPECT_THROW(MessageTable(p, bad), std::invalid_argument);
    bad = {};
    bad.max_lengths = std::unordered_map<std::string, uint32_t>{{"name", 1}};
    EXPECT_THROW(MessageTable(p, bad), std::invalid_argument);
    bad = {};
    bad.primary_keys = pbmysql::StringVector{"name", "data"};
    bad.max_lengths = std::unordered_map<std::string, uint32_t>{{"name", 768}, {"data", 1}};
    EXPECT_THROW(MessageTable(p, bad), std::invalid_argument);
}
TEST(Schema, PreservesWiderColumnAndRenamesByFieldNumber) {
    SchemaFixture f;
    MessageTable table(f.Build());
    Column(table, "old_id", "bigint unsigned", "pb:1");
    Column(table, "name", "longtext", "", "YES", nullptr);
    Column(table, "data", "mediumblob", "pb:3", "YES", nullptr);
    const auto sql = table.AlterTableSql();
    EXPECT_NE(sql.find("CHANGE COLUMN `old_id` `id` bigint unsigned NOT NULL DEFAULT 0"),
              std::string::npos);
    EXPECT_NE(sql.find("MODIFY COLUMN `name` longtext COMMENT 'pb:2'"), std::string::npos);
    EXPECT_EQ(sql.find("DROP"), std::string::npos);
    table.SetExpandOnly(true);
    EXPECT_THROW(table.BuildAlterClauses(), std::runtime_error);
}
TEST(Schema, RejectsSignednessNullDefaultAndAutoIncrementDrift) {
    SchemaFixture f;
    const auto &p = f.Build();
    MessageTable a(p);
    Column(a, "id", "bigint", "pb:1");
    EXPECT_THROW(a.BuildAlterClauses(), std::runtime_error);
    MessageTable b(p);
    Column(b, "id", "bigint unsigned", "pb:1", "YES");
    EXPECT_THROW(b.BuildAlterClauses(), std::runtime_error);
    MessageTable c(p);
    Column(c, "id", "bigint unsigned", "pb:1", "NO", "1");
    EXPECT_THROW(c.BuildAlterClauses(), std::runtime_error);
    MessageTable d(p);
    Column(d, "id", "bigint unsigned", "pb:1", "NO", nullptr, "auto_increment");
    EXPECT_THROW(d.BuildAlterClauses(), std::runtime_error);
}
TEST(Schema, AddsMissingSecondaryIndexesAndChecksOnlinePrimaryKey) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::index, "name");
    MessageTable table(f.Build());
    Existing(f, table);
    table.ClearExistingIndexes();
    EXPECT_EQ(table.BuildAlterClauses().size(),
              2u); // Missing primary keys are added, never rewritten.
    Index(table, "PRIMARY", true, 1, "id");
    const auto clauses = table.BuildAlterClauses();
    ASSERT_EQ(clauses.size(), 1);
    EXPECT_EQ(clauses[0], "ADD KEY `idx_record_0` (`name`(191))");
    Index(table, "idx_record_0", false, 1, "name", "191");
    EXPECT_TRUE(table.BuildAlterClauses().empty());
    table.ClearExistingIndexes();
    Index(table, "PRIMARY", true, 1, "name");
    EXPECT_THROW(table.BuildAlterClauses(), std::runtime_error);
}
TEST(Schema, IndexRenamesFollowColumnFieldNumbers) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::index, "name");
    MessageTable table(f.Build());
    Column(table, "id", "bigint unsigned", "pb:1");
    Column(table, "old_name", "mediumtext", "pb:2", "YES", nullptr);
    Column(table, "data", "mediumblob", "pb:3", "YES", nullptr);
    table.ClearExistingIndexes();
    Index(table, "PRIMARY", true, 1, "id");
    Index(table, "idx_record_0", false, 1, "old_name", "191");
    ASSERT_EQ(table.BuildAlterClauses().size(), 1);
    EXPECT_NE(table.AlterTableSql().find("CHANGE COLUMN `old_name` `name`"), std::string::npos);
}
TEST(Schema, MetadataQueriesEscapeNamesIndependentOfSqlMode) {
    SchemaFixture f;
    TableOptions opts;
    opts.table_name = "a'b`c";
    MessageTable table(f.Build(), opts);
    table.SetDbName("db' OR 1=1");
    EXPECT_EQ(table.SelectColumnsSql().find("db' OR 1=1"), std::string::npos);
    EXPECT_NE(table.SelectColumnsSql().find("X'"), std::string::npos);
    EXPECT_NE(table.CreateTableSql().find("`a'b``c`"), std::string::npos);
}
TEST(Schema, EmptyWritesAndCompositePKInFailClosed) {
    SchemaFixture f;
    TableOptions opts;
    opts.primary_keys = pbmysql::StringVector{"id", "name"};
    MessageTable table(f.Build(), opts);
    EXPECT_THROW(table.DeleteByWhereSql(" \n\t"), std::invalid_argument);
    EXPECT_THROW(table.SelectAllByPrimaryKeyInSql({"1"}, nullptr), std::invalid_argument);
    EXPECT_THROW(table.DeleteByKvSql("id OR 1=1", "", nullptr), std::invalid_argument);
}
TEST(Schema, AutoIncrementMayLeadSecondaryOrUniqueKey) {
    SchemaFixture f;
    const auto &prototype = f.Build();
    TableOptions opts;
    opts.primary_keys = pbmysql::StringVector{"name"};
    opts.auto_increment_key = "id";
    opts.indexes = pbmysql::StringVector{"id"};
    EXPECT_NO_THROW(MessageTable table(prototype, opts));
    opts.indexes = pbmysql::StringVector{};
    opts.unique_key = "id";
    EXPECT_NO_THROW(MessageTable table(prototype, opts));
    opts.unique_key = "name,id";
    EXPECT_THROW(MessageTable table(prototype, opts), std::invalid_argument);
}
TEST(Schema, UnsupportedWireKindsAreRejectedBeforeDDL) {
    for (auto kind : {FieldDescriptorProto::TYPE_SINT32, FieldDescriptorProto::TYPE_SINT64,
                      FieldDescriptorProto::TYPE_FIXED32, FieldDescriptorProto::TYPE_FIXED64,
                      FieldDescriptorProto::TYPE_SFIXED32, FieldDescriptorProto::TYPE_SFIXED64}) {
        SchemaFixture f;
        f.Add("unsupported", 4, kind);
        const auto &prototype = f.Build();
        EXPECT_THROW(MessageTable table(prototype), std::invalid_argument);
    }
}
TEST(Schema, TiDBOptionsIgnoreClusteredShardAndClampPreSplit) {
    SchemaFixture f;
    TableOptions opts;
    opts.tidb_shard_row_id_bits = 4;
    opts.tidb_pre_split_regions = 10;
    MessageTable clustered(f.Build(), opts);
    EXPECT_EQ(clustered.CreateTableSql().find("SHARD_ROW_ID_BITS"), std::string::npos);
    opts.tidb_nonclustered_pk = true;
    MessageTable sharded(clustered.DefaultInstance(), opts);
    EXPECT_NE(sharded.CreateTableSql().find("SHARD_ROW_ID_BITS=4 PRE_SPLIT_REGIONS=4"),
              std::string::npos);
    opts.tidb_shard_row_id_bits = 16;
    EXPECT_THROW(MessageTable table(clustered.DefaultInstance(), opts), std::invalid_argument);
}
TEST(Schema, LongIndexNamesRetainUniqueDeterministicSuffix) {
    SchemaFixture f;
    TableOptions opts;
    opts.table_name = std::string(64, 'a');
    opts.indexes = pbmysql::StringVector{"id", "name"};
    MessageTable table(f.Build(), opts);
    const auto sql = table.CreateTableSql();
    EXPECT_EQ(sql, table.CreateTableSql());
    EXPECT_NE(sql.find("idx_"), std::string::npos);
    EXPECT_EQ(sql.find(std::string(65, 'a')), std::string::npos);
}
} // namespace

namespace {
TEST(SchemaMigration, MissingAutoIncrementPrimaryKeyMovesColumnAndDependentIndexTogether) {
    SchemaFixture fixture;
    fixture.Add("gold", 4, FieldDescriptorProto::TYPE_UINT64);
    TableOptions options;
    options.auto_increment_key = "id";
    options.indexes = pbmysql::StringVector{"id"};
    MessageTable table(fixture.Build(), options);
    Column(table, "name", "mediumtext", "pb:2", "YES", nullptr, "", "utf8mb4_unicode_ci");
    Column(table, "data", "mediumblob", "pb:3", "YES", nullptr);
    table.ClearExistingIndexes();
    const auto plan = table.PlanSchemaAlignment();
    ASSERT_EQ(plan.columns.size(), 1u);
    EXPECT_NE(plan.columns[0].find("ADD COLUMN `gold`"), std::string::npos);
    ASSERT_EQ(plan.primary_key.size(), 3u);
    EXPECT_NE(plan.primary_key[0].find("ADD COLUMN `id` bigint unsigned NOT NULL AUTO_INCREMENT"),
              std::string::npos);
    EXPECT_EQ(plan.primary_key[1], "ADD PRIMARY KEY (`id`)");
    EXPECT_EQ(plan.primary_key[2], "ADD KEY `idx_record_0` (`id`)");
    EXPECT_EQ(table.BuildMigrationStatements().size(), 2u);
}
TEST(SchemaMigration, CaseInsensitiveColumnsAndDuplicateIdentityFailClosed) {
    SchemaFixture fixture;
    MessageTable table(fixture.Build());
    Column(table, "ID", "bigint unsigned", "pb:1");
    Column(table, "NAME", "mediumtext", "pb:2", "YES", nullptr, "", "utf8mb4_unicode_ci");
    Column(table, "DATA", "mediumblob", "pb:3", "YES", nullptr);
    EXPECT_TRUE(table.BuildMigrationStatements().empty());
    Column(table, "id", "bigint unsigned", "");
    try {
        table.BuildMigrationStatements();
        FAIL();
    } catch (const pbmysql::SchemaError &e) {
        EXPECT_TRUE(e.Is(pbmysql::SchemaErrorCode::SchemaDrift));
    }
}
TEST(SchemaMigration, NameClaimsPrecedeFieldNumbersAndNeverCopyOneColumnTwice) {
    SchemaFixture fixture;
    fixture.message->mutable_field(1)->set_name("nickname");
    fixture.Add("name", 4, FieldDescriptorProto::TYPE_STRING);
    MessageTable table(fixture.Build());
    Column(table, "id", "bigint unsigned", "pb:1");
    Column(table, "name", "mediumtext", "pb:2", "YES", nullptr, "", "utf8mb4_unicode_ci");
    Column(table, "data", "mediumblob", "pb:3", "YES", nullptr);
    const auto sql = table.AlterTableSql();
    EXPECT_NE(sql.find("ADD COLUMN `nickname`"), std::string::npos);
    EXPECT_EQ(sql.find("CHANGE COLUMN `name` `nickname`"), std::string::npos);
    EXPECT_NE(sql.find("MODIFY COLUMN `name`"), std::string::npos);
}
TEST(SchemaMigration, LegacyUniqueKeyProducesManualAlterReportAndClassifiedError) {
    SchemaFixture fixture;
    fixture.message->mutable_options()->SetExtension(proto2mysql::unique_key, "name");
    MessageTable table(fixture.Build());
    table.SetDbName("schema_db");
    Existing(fixture, table);
    table.ClearExistingIndexes();
    Index(table, "PRIMARY", true, 1, "id");
    Index(table, "uk_record", true, 1, "name", "191");
    try {
        table.BuildMigrationStatements();
        FAIL();
    } catch (const pbmysql::SchemaError &e) {
        EXPECT_TRUE(e.Is(pbmysql::SchemaErrorCode::LegacyKeyColumn));
        EXPECT_TRUE(e.Is(pbmysql::SchemaErrorCode::SchemaDrift));
        const auto &r = e.Report();
        EXPECT_FALSE(r.shadow_rebuild);
        ASSERT_GE(r.statements.size(), 5u);
        EXPECT_NE(r.statements[0].find("STRICT_ALL_TABLES"), std::string::npos);
        EXPECT_NE(r.statements[1].find("DROP INDEX `uk_record`"), std::string::npos);
        EXPECT_NE(r.ToString().find("CHAR_LENGTH"), std::string::npos);
        EXPECT_NE(
            r.ToString().find("UPDATE `schema_db`.`record` SET `name` = '' WHERE `name` IS NULL"),
            std::string::npos);
        EXPECT_NE(r.ToString().find("MODIFY COLUMN `name` VARCHAR(191)"), std::string::npos);
        EXPECT_NE(r.statements.back().find("ADD UNIQUE KEY `uk_record` (`name`)"),
                  std::string::npos);
    }
}
TEST(SchemaMigration, LegacyPrimaryShadowPreservesWiderColumnsAndExtraIndexes) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::primary_key, "name");
    f.message->mutable_field(0)->set_type(FieldDescriptorProto::TYPE_UINT32);
    TableOptions options;
    options.auto_increment_key = "id";
    options.indexes = pbmysql::StringVector{"id"};
    MessageTable table(f.Build(), options);
    table.SetDbName("db");
    Column(table, "id", "bigint unsigned", "pb:1", "NO", nullptr, "auto_increment");
    Column(table, "old_name", "varchar(191)", "pb:2", "NO", "", "", "utf8mb4_unicode_ci");
    Column(table, "data", "longblob", "pb:3", "YES", nullptr);
    Column(table, "orphan", "mediumtext", "", "YES", nullptr, "", "utf8mb4_unicode_ci");
    table.ClearExistingIndexes();
    Index(table, "PRIMARY", true, 1, "old_name");
    Index(table, "idx_record_0", false, 1, "id");
    Index(table, "custom_data", false, 1, "data", "32");
    Index(table, "custom_name", true, 1, "old_name", "64");
    const auto r = table.LegacyMigrationReport();
    ASSERT_TRUE(r.shadow_rebuild);
    ASSERT_GE(r.statements.size(), 10u);
    const auto &create = r.statements[1];
    EXPECT_NE(create.find("`id` bigint unsigned NOT NULL AUTO_INCREMENT"), std::string::npos);
    EXPECT_NE(create.find("`data` longblob"), std::string::npos);
    EXPECT_NE(create.find("KEY `custom_data` (`data`(32))"), std::string::npos);
    EXPECT_NE(create.find("UNIQUE KEY `custom_name` (`name`)"), std::string::npos);
    EXPECT_EQ(create.find("`orphan`"), std::string::npos);
    EXPECT_NE(r.statements[2].find("SELECT `id`, `old_name`, `data`"), std::string::npos);
    EXPECT_NE(r.ToString().find("Orphan online column"), std::string::npos);
    EXPECT_NE(r.ToString().find("p2m_rebase_auto_increment"), std::string::npos);
    EXPECT_NE(r.statements.back().find("RENAME TABLE `db`.`record` TO `db`.`record__p2m_old`"),
              std::string::npos);
    for (const auto &sql : r.statements)
        EXPECT_EQ(sql.find("DROP TABLE"), std::string::npos);
}
TEST(SchemaMigration, ShadowReportsUnsupportedExtraIndexesAndConcreteDependencies) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::primary_key, "name");
    MessageTable table(f.Build());
    Existing(f, table);
    table.ClearExistingIndexes();
    Index(table, "PRIMARY", true, 1, "name", "191");
    const char *idx[] = {"fulltext_name", "1", "1", "name", nullptr, "FULLTEXT", nullptr};
    unsigned long il[] = {13, 1, 1, 4, 0, 8, 0};
    table.OnSelectIndex(const_cast<MYSQL_ROW>(idx), il, 7);
    table.ClearDependencies();
    const char *fk[] = {"db", "fk_child_parent", "child", "record"};
    unsigned long fl[] = {2, 15, 5, 6};
    table.OnSelectForeignKey(const_cast<MYSQL_ROW>(fk), fl, 4);
    const char *tr[] = {"audit_record", "AFTER", "INSERT"};
    unsigned long tl[] = {12, 5, 6};
    table.OnSelectTrigger(const_cast<MYSQL_ROW>(tr), tl, 3);
    const auto r = table.LegacyMigrationReport();
    EXPECT_NE(r.ToString().find("unsupported type fulltext"), std::string::npos);
    EXPECT_NE(r.ToString().find("fk_child_parent"), std::string::npos);
    EXPECT_NE(r.ToString().find("audit_record"), std::string::npos);
    EXPECT_EQ(r.statements[1].find("KEY `fulltext_name`"), std::string::npos);
}
TEST(SchemaMigration, ShadowRespectsIndexBudgetAndAddsChecksForReducedKeyWidth) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::primary_key, "name,data");
    MessageTable table(f.Build());
    Column(table, "id", "bigint unsigned", "pb:1");
    Column(table, "name", "varchar(768)", "pb:2", "NO", "", "", "utf8mb4_0900_bin");
    Column(table, "data", "mediumblob", "pb:3", "YES", nullptr);
    table.ClearExistingIndexes();
    Index(table, "PRIMARY", true, 1, "name");
    Index(table, "PRIMARY", true, 2, "data", "191");
    const auto r = table.LegacyMigrationReport();
    EXPECT_TRUE(r.shadow_rebuild);
    EXPECT_NE(r.statements[1].find("`name` VARCHAR(191)"), std::string::npos);
    EXPECT_NE(r.ToString().find("3072-byte index budget"), std::string::npos);
    EXPECT_NE(r.ToString().find("target `name` limit 191"), std::string::npos);
}
TEST(SchemaMigration, ModernOptionsFallbackAndCheckedOfflineDDL) {
    SchemaFixture f;
    f.message->mutable_options()->ClearExtension(proto2mysql::table_name);
    f.message->mutable_options()->ClearExtension(proto2mysql::primary_key);
    auto *opts = f.message->mutable_options();
    auto *u = opts->GetReflection()->MutableUnknownFields(opts);
    u->AddLengthDelimited(proto2mysql::kTableNameFieldNumber, "wire_record");
    u->AddLengthDelimited(proto2mysql::kPrimaryKeyFieldNumber, "name");
    auto *fieldopts = f.message->mutable_field(1)->mutable_options();
    fieldopts->GetReflection()->MutableUnknownFields(fieldopts)->AddVarint(
        proto2mysql::kMaxLengthFieldNumber, 37);
    const auto &prototype = f.Build();
    MessageTable table(prototype);
    EXPECT_EQ(table.TableName(), "wire_record");
    EXPECT_EQ(table.PrimaryKeys(), pbmysql::StringVector{"name"});
    EXPECT_NE(table.CreateTableSql().find("VARCHAR(37)"), std::string::npos);
    EXPECT_EQ(pbmysql::GenerateCreateTableSQL(prototype),
              pbmysql::GenerateCreateTableSQLChecked(prototype));
    TableOptions bad;
    bad.primary_keys = pbmysql::StringVector{"missing"};
    EXPECT_THROW(pbmysql::ValidateTableMessage(prototype, bad), std::invalid_argument);
    EXPECT_TRUE(pbmysql::GenerateCreateTableSQL(prototype, bad).empty());
}
TEST(SchemaMigration, PKValidationDoesNotInspectUnrelatedUniqueValue) {
    SchemaFixture f;
    f.message->mutable_options()->SetExtension(proto2mysql::unique_key, "name");
    const auto &p = f.Build();
    MessageTable table(p);
    std::unique_ptr<Message> m(p.New());
    m->GetReflection()->SetString(m.get(), m->GetDescriptor()->FindFieldByName("name"),
                                  std::string(192, 'x'));
    EXPECT_NO_THROW(table.ValidatePrimaryKeyMessage(*m));
    EXPECT_THROW(table.ValidateMessage(*m), std::invalid_argument);
}
} // namespace
