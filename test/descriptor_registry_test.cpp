#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "runtime_test.pb.h"
#include "src/mysql_wrapper/database.h"
#include "src/mysql_wrapper/identifier_utils.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

PBMYSQL_REGISTER_PROTO_FILE(pbmysql::test::RuntimeRow)

namespace {
inline constexpr int kNewSchemaColumnNumber = 100;
using namespace pbmysql;
using Row = pbmysql::test::RuntimeRow;
TEST(DescriptorRegistry, ScansGeneratedRootsAndDependenciesWithoutPerTableCalls) {
    Database db;
    const auto names = db.RegisterAllTables();
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(db.Tables().size(), 3u);
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    EXPECT_TRUE(FileHasDBOption(Row::descriptor()->file()));
    EXPECT_EQ(TableNameFromDescriptor(Row::descriptor()),
              std::optional<std::string>{"runtime_rows"});
    EXPECT_FALSE(TableNameFromDescriptor(pbmysql::test::RuntimePage::descriptor()).has_value());
    RegisterProtoFileName(std::string(Row::descriptor()->file()->name()));
    EXPECT_EQ(db.RegisterAllTables().size(), 3u);
}
TEST(DescriptorRegistry, InputSchemaWinsOverLinkedSchemaAndDatabaseOwnsItsLifetime) {
    Database db;
    {
        google::protobuf::FileDescriptorSet files;
        Row::descriptor()->file()->CopyTo(files.add_file());
        auto *added = files.mutable_file(0)->mutable_message_type(0)->add_field();
        added->set_name("new_column");
        added->set_number(kNewSchemaColumnNumber);
        added->set_label(google::protobuf::FieldDescriptorProto::LABEL_OPTIONAL);
        added->set_type(google::protobuf::FieldDescriptorProto::TYPE_UINT64);
        auto source = std::make_shared<DescriptorSet>(files);
        EXPECT_EQ(db.RegisterAllTables(source).size(), 3u);
    }
    auto row = db.NewMessage("pbmysql.test.RuntimeRow");
    ASSERT_NE(row->GetDescriptor()->FindFieldByName("new_column"), nullptr);
    EXPECT_NE(db.Builder(*row).CreateTable().find("`new_column`"), std::string::npos);
    EXPECT_THROW(db.Builder(Row::default_instance()), std::invalid_argument);
}
TEST(DescriptorRegistry, RegistrationFailureIsAtomicAndCannotLoseExistingTables) {
    Database db;
    db.RegisterTable(Row::default_instance());
    google::protobuf::FileDescriptorSet files;
    Row::descriptor()->file()->CopyTo(files.add_file());
    auto *duplicate = files.mutable_file(0)->add_message_type();
    *duplicate = files.file(0).message_type(0);
    duplicate->set_name("DuplicateRow");
    auto source = std::make_shared<DescriptorSet>(files);
    EXPECT_THROW(db.RegisterAllTables(source), std::invalid_argument);
    EXPECT_EQ(db.Tables().size(), 1u);
    EXPECT_EQ(db.Builder(Row::default_instance()).TableName(), "runtime_rows");
}
TEST(DescriptorRegistry, InvalidDescriptorSetsFailWithReadableErrors) {
    google::protobuf::FileDescriptorSet empty;
    EXPECT_THROW(DescriptorSet source(empty), std::invalid_argument);
    auto *missing = empty.add_file();
    missing->set_name("missing.proto");
    missing->add_dependency("unknown_dependency.proto");
    EXPECT_THROW(DescriptorSet source(empty), std::invalid_argument);
    *empty.add_file() = empty.file(0);
    EXPECT_THROW(DescriptorSet source(empty), std::invalid_argument);
}
TEST(IdentifierMapping, UnicodeSimpleFoldMatchesGoAndDoesNotUseLocale) {
    EXPECT_EQ(FoldIdentifier("Keys"), FoldIdentifier("\xe2\x84\xaa"
                                                     "eys"));
    EXPECT_EQ(FoldIdentifier("S"), FoldIdentifier("\xc5\xbf"));
    EXPECT_EQ(FoldIdentifier("\xce\xa3"), FoldIdentifier("\xcf\x82"));
    EXPECT_NE(FoldIdentifier("Stra\xc3\x9f"
                             "e"),
              FoldIdentifier("STRASSE"));
    EXPECT_THROW(FoldIdentifier(std::string("\xff", 1)), std::invalid_argument);
}
TEST(IdentifierMapping, DuplicatePhysicalTablesFailBeforeAnyOutput) {
    Database db;
    TableOptions a;
    a.table_name = "Keys";
    db.RegisterTable(Row::default_instance(), a);
    TableOptions b;
    b.table_name = "\xe2\x84\xaa"
                   "eys";
    EXPECT_THROW(db.RegisterTable(pbmysql::test::CollectionRow::default_instance(), b),
                 std::invalid_argument);
    EXPECT_EQ(db.Tables().size(), 1u);
    // The mutable compatibility accessor cannot bypass export preflight.
    MessageTable duplicate(pbmysql::test::CollectionRow::default_instance(), b);
    db.Tables().emplace("manual-duplicate", std::move(duplicate));
    std::ostringstream output;
    EXPECT_THROW(db.WriteCreateTableSql(output), std::invalid_argument);
    EXPECT_TRUE(output.str().empty());
}
TEST(SchemaExport, SortsPhysicalTableNamesAndPreservesOutputOnFailure) {
    Database db;
    TableOptions a;
    a.table_name = "z_table";
    db.RegisterTable(Row::default_instance(), a);
    TableOptions b;
    b.table_name = "a_table";
    db.RegisterTable(pbmysql::test::CollectionRow::default_instance(), b);
    std::ostringstream output;
    db.WriteCreateTableSql(output);
    EXPECT_LT(output.str().find("`a_table`"), output.str().find("`z_table`"));
    const auto destination =
        std::filesystem::temp_directory_path() /
        ("pbmysql-export-failure-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sql");
    {
        std::ofstream file(destination);
        file << "previous schema";
    }
    db.Tables().emplace("duplicate",
                        MessageTable(pbmysql::test::CollectionRow::default_instance(), a));
    EXPECT_THROW(db.DumpCreateTableSqlFile(destination.string()), std::invalid_argument);
    std::ifstream file(destination);
    std::string content;
    std::getline(file, content);
    EXPECT_EQ(content, "previous schema");
    file.close();
    std::filesystem::remove(destination);
}
TEST(DatabaseOptions, ExpandOnlyPropagatesToExistingFutureAndTransactionTables) {
    Database db;
    db.RegisterTable(Row::default_instance());
    db.SetExpandOnly(true);
    db.RegisterTable(pbmysql::test::CollectionRow::default_instance());
    EXPECT_TRUE(db.ExpandOnly());
    for (const auto &entry : db.Tables())
        EXPECT_TRUE(entry.second.ExpandOnly());
    db.SetExpandOnly(false);
    for (const auto &entry : db.Tables())
        EXPECT_FALSE(entry.second.ExpandOnly());
}

TEST(DescriptorRegistry, DynamicBuilderKeepsItsDescriptorSourceAliveAfterDatabaseDestruction) {
    std::weak_ptr<DescriptorSet> source_lifetime;
    auto builder = [&] {
        google::protobuf::FileDescriptorSet files;
        Row::descriptor()->file()->CopyTo(files.add_file());
        auto source = std::make_shared<DescriptorSet>(files);
        source_lifetime = source;
        Database db;
        db.RegisterAllTables(source);
        auto row = db.NewMessage("pbmysql.test.RuntimeRow");
        return db.Builder(*row);
    }();
    EXPECT_FALSE(source_lifetime.expired());
    EXPECT_NE(builder.CreateTable().find("`runtime_rows`"), std::string::npos);
    auto row = std::unique_ptr<google::protobuf::Message>(builder.Table().DefaultInstance().New());
    EXPECT_FALSE(builder.Insert(*row).sql.empty());
}

TEST(DescriptorRegistry, EmptyTableNamesAreSkippedAndDynamicRootsCannotDangleInGlobalRegistry) {
    google::protobuf::FileDescriptorSet files;
    Row::descriptor()->file()->CopyTo(files.add_file());
    files.mutable_file(0)->mutable_message_type(0)->mutable_options()->SetExtension(
        proto2mysql::table_name, "");
    auto source = std::make_shared<DescriptorSet>(files);
    Database db;
    EXPECT_EQ(db.RegisterAllTables(source).size(), 2u);
    EXPECT_THROW(RegisterProtoFile(source->Files().front()), std::invalid_argument);
}
TEST(SchemaExport, Utf8DescriptorAndOutputPathsWorkWithNativeFilesystemEncoding) {
    auto directory = std::filesystem::temp_directory_path() /
                     std::filesystem::u8path("pbmysql-\xe8\xb7\xaf\xe5\xbe\x84-test");
    directory += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    const auto input = directory / std::filesystem::u8path("\xe6\x8f\x8f\xe8\xbf\xb0.pb");
    const auto output = directory / std::filesystem::u8path("\xe8\xbf\x81\xe7\xa7\xbb.sql");
    google::protobuf::FileDescriptorSet files;
    Row::descriptor()->file()->CopyTo(files.add_file());
    {
        std::ofstream file(input, std::ios::binary);
        ASSERT_TRUE(files.SerializeToOstream(&file));
    }
    const auto input_u8 = input.u8string(), output_u8 = output.u8string();
    auto source = DescriptorSet::Load(std::string(input_u8.begin(), input_u8.end()));
    Database db;
    db.RegisterAllTables(source);
    db.DumpCreateTableSqlFile(std::string(output_u8.begin(), output_u8.end()));
    std::ifstream file(output);
    std::string content((std::istreambuf_iterator<char>(file)), {});
    EXPECT_NE(content.find("`runtime_rows`"), std::string::npos);
    file.close();
    std::filesystem::remove(input);
    std::filesystem::remove(output);
    std::filesystem::remove(directory);
}
} // namespace
