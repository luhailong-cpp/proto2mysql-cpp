#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/database_limits.h"
#include "src/mysql_wrapper/sql_builder.h"
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/timestamp.pb.h>
#include <gtest/gtest.h>
#include <limits>

namespace {
using google::protobuf::FieldDescriptorProto;
using google::protobuf::Message;
using namespace pbmysql;

class BuilderFixture : public ::testing::Test {
  protected:
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    std::unique_ptr<Message> row;
    const google::protobuf::FieldDescriptor *F(const std::string &name) const {
        return row->GetDescriptor()->FindFieldByName(name);
    }
    void SetUp() override {
        // Make timestamp's generated file available as the underlay dependency.
        (void)google::protobuf::Timestamp::descriptor();
        google::protobuf::FileDescriptorProto file;
        file.set_name("builder_fixture.proto");
        file.set_package("tests");
        file.set_syntax("proto3");
        file.add_dependency("google/protobuf/timestamp.proto");
        auto *message = file.add_message_type();
        message->set_name("Row");
        auto *unknown = message->mutable_options()->GetReflection()->MutableUnknownFields(
            message->mutable_options());
        unknown->AddLengthDelimited(proto2mysql::kTableNameFieldNumber, "items");
        unknown->AddLengthDelimited(proto2mysql::kPrimaryKeyFieldNumber, "id");
        auto add = [&](const char *name, int number, FieldDescriptorProto::Type type,
                       bool repeated = false) {
            auto *f = message->add_field();
            f->set_name(name);
            f->set_number(number);
            f->set_type(type);
            f->set_label(repeated ? FieldDescriptorProto::LABEL_REPEATED
                                  : FieldDescriptorProto::LABEL_OPTIONAL);
            return f;
        };
        add("id", 1, FieldDescriptorProto::TYPE_UINT64);
        add("name", 2, FieldDescriptorProto::TYPE_STRING);
        add("payload", 3, FieldDescriptorProto::TYPE_BYTES);
        add("gold", 4, FieldDescriptorProto::TYPE_UINT64);
        add("ratio", 5, FieldDescriptorProto::TYPE_FLOAT);
        add("score", 6, FieldDescriptorProto::TYPE_DOUBLE);
        add("tags", 7, FieldDescriptorProto::TYPE_STRING, true);
        add("created_at", 8, FieldDescriptorProto::TYPE_MESSAGE)
            ->set_type_name(".google.protobuf.Timestamp");
        const auto *built = pool.BuildFile(file);
        ASSERT_NE(built, nullptr);
        row.reset(factory.GetPrototype(built->message_type(0))->New());
        row->GetReflection()->SetUInt64(row.get(), F("id"), 7);
    }
};

TEST_F(BuilderFixture, TypedBinaryParametersAndUnsetTimestamp) {
    row->GetReflection()->SetUInt64(row.get(), F("gold"), std::numeric_limits<uint64_t>::max());
    row->GetReflection()->SetString(row.get(), F("name"), "O'Reilly\\");
    row->GetReflection()->SetString(row.get(), F("payload"), std::string("a\0b", 3));
    row->GetReflection()->SetFloat(row.get(), F("ratio"), 0.1f);
    MessageTable table(*row);
    SQLBuilder builder(table);
    auto insert = builder.Insert(*row);
    EXPECT_EQ(insert.sql.find("O'Reilly"), std::string::npos);
    ASSERT_EQ(insert.args.size(), 8u);
    EXPECT_EQ(std::get<uint64_t>(insert.args[3]), std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(std::get<std::string>(insert.args[2]), std::string("a\0b", 3));
    EXPECT_EQ(std::get<std::string>(insert.args[4]), "0.1");
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(insert.args[7]));
}

TEST_F(BuilderFixture, CollectionsUseOriginalWireTagAndReplaceOldContents) {
    row->GetReflection()->AddString(row.get(), F("tags"), "x");
    EXPECT_EQ(std::get<std::string>(FieldValue(*row, F("tags"))), std::string("\x3a\x01x", 3));
    MessageTable table(*row);
    SQLBuilder builder(table);
    auto values = builder.Insert(*row).args;
    row->GetReflection()->AddString(row.get(), F("tags"), "old");
    row->GetReflection()->SetString(row.get(), F("name"), "stale");
    ParseRow(*row, values);
    EXPECT_EQ(row->GetReflection()->FieldSize(*row, F("tags")), 1);
    EXPECT_EQ(row->GetReflection()->GetString(*row, F("name")), "");
    values[6] = std::string{};
    ParseRow(*row, values);
    EXPECT_EQ(row->GetReflection()->FieldSize(*row, F("tags")), 0);
}

TEST_F(BuilderFixture, FailedParseIsAtomicAndUnsignedParsingIsStrict) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    auto values = builder.Insert(*row).args;
    values[0] = std::string("18446744073709551616");
    EXPECT_THROW(ParseRow(*row, values), std::invalid_argument);
    EXPECT_EQ(row->GetReflection()->GetUInt64(*row, F("id")), 7u);
    values[0] = std::string("-1");
    EXPECT_THROW(ParseRow(*row, values), std::invalid_argument);
    values[0] = std::string("7garbage");
    EXPECT_THROW(ParseRow(*row, values), std::invalid_argument);
}

TEST_F(BuilderFixture, TimestampKeepsMicrosecondsAndNullClearsPresence) {
    auto *timestamp = row->GetReflection()->MutableMessage(row.get(), F("created_at"));
    timestamp->GetReflection()->SetInt64(timestamp,
                                         timestamp->GetDescriptor()->FindFieldByName("seconds"), 0);
    timestamp->GetReflection()->SetInt32(
        timestamp, timestamp->GetDescriptor()->FindFieldByName("nanos"), 123456789);
    EXPECT_EQ(std::get<std::string>(FieldValue(*row, F("created_at"))),
              "1970-01-01 00:00:00.123456");
    MessageTable table(*row);
    SQLBuilder builder(table);
    auto values = builder.Insert(*row).args;
    ParseRow(*row, values);
    timestamp = row->GetReflection()->MutableMessage(row.get(), F("created_at"));
    EXPECT_EQ(timestamp->GetReflection()->GetInt32(
                  *timestamp, timestamp->GetDescriptor()->FindFieldByName("nanos")),
              123456000);
    values[7] = nullptr;
    ParseRow(*row, values);
    EXPECT_FALSE(row->GetReflection()->HasField(*row, F("created_at")));
}

TEST_F(BuilderFixture, ExplicitUpdateWritesZeroAndPrimaryKeyStaysInWhere) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    auto update = builder.UpdateFieldsByPK(*row, {"gold"});
    EXPECT_EQ(update.sql, "UPDATE `items` SET `gold` = ? WHERE `id` = ?");
    ASSERT_EQ(update.args.size(), 2u);
    EXPECT_EQ(std::get<uint64_t>(update.args[0]), 0u);
    EXPECT_EQ(std::get<uint64_t>(update.args[1]), 7u);
    EXPECT_EQ(builder.UpdateByPK(*row).sql, "UPDATE `items` SET `id` = ? WHERE `id` = ?");
}

TEST_F(BuilderFixture, MutationRejectsEmptyWhereAndUnknownColumns) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    EXPECT_THROW(builder.DeleteWhere(" WHERE \t"), std::invalid_argument);
    EXPECT_THROW(builder.UpdateAssignsWhere({SetCol("gold", uint64_t(1))}, " "),
                 std::invalid_argument);
    EXPECT_THROW(builder.SelectColumns({"unknown"}), std::invalid_argument);
    EXPECT_THROW(builder.DeleteByKVIn("id", {}), std::invalid_argument);
    EXPECT_THROW(builder.SelectByPKIn({std::string("-1")}), std::invalid_argument);
    EXPECT_EQ(builder.DeleteWhere("1=1").sql, "DELETE FROM `items` WHERE 1=1");
}

TEST_F(BuilderFixture, ArithmeticRejectsTextAndDecrementUsesGuard) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    EXPECT_THROW(builder.IncrByPK(*row, "name", 1), std::invalid_argument);
    EXPECT_THROW(builder.UpsertAdd(*row, {}), std::invalid_argument);
    EXPECT_THROW(builder.UpsertAdd(*row, {"payload"}), std::invalid_argument);
    EXPECT_THROW(builder.DecrByPKIfEnough(*row, "gold", -1), std::invalid_argument);
    EXPECT_THROW(builder.UpdateAssignsByPK(*row, {AddCol("gold", std::string("oops"))}),
                 std::invalid_argument);
    EXPECT_THROW(
        builder.UpdateAssignsByPK(*row, {SubCol("gold", std::numeric_limits<double>::infinity())}),
        std::invalid_argument);
    const auto decrement = builder.DecrByPKIfEnough(*row, "gold", 100);
    EXPECT_EQ(decrement.sql,
              "UPDATE `items` SET `gold` = `gold` - ? WHERE `id` = ? AND `gold` >= ?");
    ASSERT_EQ(decrement.args.size(), 3u);
    EXPECT_EQ(std::get<int64_t>(decrement.args[0]), 100);
    EXPECT_EQ(std::get<uint64_t>(decrement.args[1]), 7u);
    EXPECT_EQ(std::get<int64_t>(decrement.args[2]), 100);
}

TEST_F(BuilderFixture, IgnorePreservesErrorsAndBatchAndGuardPreserveArgOrder) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    auto ignore = builder.InsertIgnore(*row);
    EXPECT_EQ(ignore.sql.find("INSERT IGNORE"), std::string::npos);
    EXPECT_NE(ignore.sql.find("ON DUPLICATE KEY UPDATE `id` = `id`"), std::string::npos);
    auto batch = builder.BatchUpsertWith({row.get(), row.get()}, {AddCol("gold", int64_t(9))});
    ASSERT_EQ(batch.args.size(), 17u);
    EXPECT_EQ(std::get<int64_t>(batch.args.back()), 9);
    row->GetReflection()->SetString(row.get(), F("name"), "new");
    auto guarded = builder.UpdateByPKIf(*row, "`gold` >= ?", {uint64_t(10)});
    ASSERT_EQ(guarded.args.size(), 4u);
    EXPECT_EQ(std::get<uint64_t>(guarded.args[0]), 7u);
    EXPECT_EQ(std::get<std::string>(guarded.args[1]), "new");
    EXPECT_EQ(std::get<uint64_t>(guarded.args[2]), 7u);
    EXPECT_EQ(std::get<uint64_t>(guarded.args[3]), 10u);
}

TEST_F(BuilderFixture, SelectionOptionsLocksAndLimitedDelete) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    QueryOptions opts;
    opts.order_by = "`id` DESC";
    opts.limit = 20;
    opts.offset = 40;
    opts.for_update = true;
    auto selected = builder.SelectByPKIn({uint64_t(7), uint64_t(8)}, opts);
    EXPECT_NE(selected.sql.find("`id` IN (?, ?) ORDER BY `id` DESC LIMIT 20 OFFSET 40 FOR UPDATE"),
              std::string::npos);
    EXPECT_EQ(builder.DeleteWhereLimit("`gold` = ?", {uint64_t(0)}, "`id`", 10).sql,
              "DELETE FROM `items` WHERE `gold` = ? ORDER BY `id` LIMIT 10");
    EXPECT_THROW(builder.DeleteWhereLimit("1=1", {}, "", 0), std::invalid_argument);
}

TEST_F(BuilderFixture, NonFiniteNumbersAreRejectedBeforeReturningStatement) {
    MessageTable table(*row);
    SQLBuilder builder(table);
    row->GetReflection()->SetDouble(row.get(), F("score"),
                                    std::numeric_limits<double>::quiet_NaN());
    EXPECT_THROW(builder.Insert(*row), std::invalid_argument);
    row->GetReflection()->SetDouble(row.get(), F("score"), std::numeric_limits<double>::infinity());
    EXPECT_THROW(builder.Insert(*row), std::invalid_argument);
}

TEST_F(BuilderFixture, PredicateValuesKeepIntegerAndFloatPrecision) {
    SQLBuilder builder(*row);
    const auto maximum = std::numeric_limits<uint64_t>::max();
    const auto integers = builder.SelectByPKIn({std::to_string(maximum)});
    ASSERT_EQ(integers.args.size(), 1u);
    EXPECT_EQ(std::get<uint64_t>(integers.args[0]), maximum);
    const auto floats = builder.SelectByKVIn("ratio", {std::string("0.1")});
    ASSERT_EQ(floats.args.size(), 1u);
    EXPECT_EQ(std::get<double>(floats.args[0]), static_cast<double>(0.1f));
    EXPECT_THROW(
        builder.UpdateAssignsByPK(*row, {SetCol("gold", std::string("18446744073709551616"))}),
        std::invalid_argument);
}

TEST_F(BuilderFixture, InvalidBatchesAreRejectedBeforeReturningSQL) {
    SQLBuilder builder(*row);
    EXPECT_THROW(builder.BatchInsert({}), std::invalid_argument);
    EXPECT_THROW(builder.BatchInsert({row.get(), nullptr}), std::invalid_argument);
    EXPECT_THROW(builder.BatchInsert(std::vector<const Message *>(kBatchSize + 1, row.get())),
                 std::invalid_argument);
    google::protobuf::Timestamp different;
    EXPECT_THROW(builder.Insert(different), std::invalid_argument);
    row->Clear();
    EXPECT_THROW(builder.InsertSetFields(*row), std::invalid_argument);
}

TEST_F(BuilderFixture, QueryFallbackOffsetGuardAndInputArgumentsStayIndependent) {
    SQLBuilder builder(*row);
    EXPECT_EQ(builder.SelectColumns({}).sql, builder.SelectWhere().sql);
    EXPECT_NE(builder.SelectWhere().sql.find(" WHERE 1=1"), std::string::npos);
    EXPECT_EQ(builder.Count().sql, "SELECT COUNT(*) FROM `items` WHERE 1=1");
    EXPECT_EQ(builder.Exists().sql, "SELECT 1 FROM `items` WHERE 1=1 LIMIT 1");
    EXPECT_EQ(builder.SelectByPK(*row).sql.find("LIMIT"), std::string::npos);
    QueryOptions options;
    options.offset = 20;
    EXPECT_EQ(builder.SelectWhere("", {}, options).sql.find("OFFSET"), std::string::npos);
    options.limit = 5;
    options.for_update = true;
    EXPECT_NE(builder.SelectWhere("", {}, options).sql.find(" LIMIT 5 OFFSET 20 FOR UPDATE"),
              std::string::npos);
    std::vector<SqlValue> arguments{uint64_t(3), uint64_t(4)};
    auto guarded = builder.DeleteByPKIf(*row, "`gold` = ? OR `gold` = ?", arguments);
    EXPECT_EQ(guarded.sql, "DELETE FROM `items` WHERE (`id` = ?) AND (`gold` = ? OR `gold` = ?)");
    guarded.args[1] = uint64_t(99);
    EXPECT_EQ(std::get<uint64_t>(arguments[0]), 3u);
    EXPECT_THROW(builder.DecrByPKIfEnough(*row, "gold", 0), std::invalid_argument);
}

TEST_F(BuilderFixture, OpaquePredicateBytesAreNotParsedOrReserialized) {
    SQLBuilder builder(*row);
    const std::string wire("\x08\x01\x3a\x01x", 5);
    const auto selected = builder.SelectByKVIn("tags", {wire});
    EXPECT_EQ(std::get<std::string>(selected.args[0]), wire);
    const auto updated = builder.UpdateAssignsByPK(*row, {SetCol("tags", wire)});
    EXPECT_EQ(std::get<std::string>(updated.args[0]), wire);
    const auto timestamp =
        builder.NormalizeComparisonValue("created_at", std::string("2026-01-02"));
    EXPECT_EQ(std::get<std::string>(timestamp), "2026-01-02");
}

TEST_F(BuilderFixture, IdentityGuardedCompatibilityHelpersAndAllAssignOperators) {
    SQLBuilder builder(*row);
    row->GetReflection()->SetString(row.get(), F("name"), "new");
    const auto save = builder.GetSaveSQLWithArgs(*row);
    EXPECT_NE(save.sql.find("`name` = IF(`id` <=> VALUES(`id`), VALUES(`name`), `name`)"),
              std::string::npos);
    EXPECT_EQ(save.args.size(), 8u);
    const auto partial = builder.GetInsertOnDupUpdateSQLWithArgs(*row);
    EXPECT_NE(partial.sql.find("`name` = IF(`id` <=> VALUES(`id`), ?, `name`)"), std::string::npos);
    EXPECT_EQ(partial.sql.find("`gold` = IF"), std::string::npos);
    EXPECT_EQ(partial.args.size(), 9u);
    EXPECT_NE(builder.GetInsertOnDupKeyForPrimaryKeyWithArgs(*row).sql.find("UPDATE `id` = `id`"),
              std::string::npos);
    EXPECT_EQ(builder.GetBatchSaveSQLWithArgs({row.get(), row.get()}).args.size(), 16u);
    for (const auto &assign : {SetNew("gold"), AddNew("gold"), MinNew("gold"), MaxNew("gold"),
                               SetNewIfZero("gold"), KeepOld("gold")}) {
        const auto sql = builder.UpsertWith(*row, {assign});
        EXPECT_NE(sql.sql.find(assign.expression), std::string::npos);
        EXPECT_EQ(sql.args.size(), 8u);
    }
    auto repeated =
        builder.UpdateAssignsByPK(*row, {AddCol("gold", int64_t(1)), SubCol("gold", int64_t(2))});
    EXPECT_NE(repeated.sql.find("`gold` = `gold` + ?, `gold` = `gold` - ?"), std::string::npos);
    EXPECT_EQ(builder.BatchInsertIgnore({row.get(), row.get()}).args.size(), 16u);
    EXPECT_EQ(builder.BatchReplace({row.get(), row.get()}).sql.find("REPLACE INTO"), 0u);
    EXPECT_EQ(builder.BatchUpsert({row.get()}, {"name"}).args.size(), 8u);
    EXPECT_EQ(builder.SelectByPKForUpdate(*row).sql.find("FOR UPDATE"),
              builder.SelectByPKForUpdate(*row).sql.size() - 10);
}

TEST_F(BuilderFixture, KeyValidationOnlyExaminesFieldsUsedByTheStatement) {
    row->GetReflection()->SetString(row.get(), F("name"), "overlong");
    TableOptions options;
    options.unique_key = "name";
    options.max_lengths = std::unordered_map<std::string, uint32_t>{{"name", 1}};
    MessageTable table(*row, options);
    SQLBuilder builder(table);
    EXPECT_NO_THROW(builder.SelectByPK(*row));
    EXPECT_NO_THROW(builder.DeleteByPK(*row));
    EXPECT_NO_THROW(builder.UpdateFieldsByPK(*row, {"gold"}));
    EXPECT_THROW(builder.Insert(*row), std::invalid_argument);
    EXPECT_THROW(builder.UpdateFieldsByPK(*row, {"name"}), std::invalid_argument);
    EXPECT_THROW(builder.NormalizeComparisonValue("name", std::string("long")),
                 std::invalid_argument);
}

TEST_F(BuilderFixture, CompoundIdentityGuardIncludesEveryKeyAndPKOnlySaveIsNoop) {
    TableOptions options;
    options.primary_keys = StringVector{"id", "name"};
    MessageTable table(*row, options);
    SQLBuilder builder(table);
    EXPECT_EQ(builder.PrimaryKeyWhere(*row).sql, "`id` = ? AND `name` = ?");
    EXPECT_NE(builder.GetSaveSQLWithArgs(*row).sql.find(
                  "IF(`id` <=> VALUES(`id`) AND `name` <=> VALUES(`name`),"),
              std::string::npos);
    EXPECT_THROW(builder.SelectByPKIn({uint64_t(1)}), std::invalid_argument);
    google::protobuf::Timestamp key_only;
    TableOptions pk_only;
    pk_only.primary_keys = StringVector{"seconds", "nanos"};
    SQLBuilder only(key_only, pk_only);
    EXPECT_NE(
        only.GetSaveSQLWithArgs(key_only).sql.find("ON DUPLICATE KEY UPDATE `seconds` = `seconds`"),
        std::string::npos);
    EXPECT_THROW(only.Upsert(key_only), std::invalid_argument);
}

TEST_F(BuilderFixture, BuilderOwnsMappingAcrossRegistryReplacementAndDestruction) {
    std::unique_ptr<SQLBuilder> builder;
    {
        std::unordered_map<std::string, MessageTable> registry;
        registry.emplace("first", MessageTable(*row));
        builder = std::make_unique<SQLBuilder>(registry.at("first"));
        auto replacement = registry;
        TableOptions renamed;
        renamed.table_name = "renamed_items";
        replacement.insert_or_assign("second", MessageTable(*row, renamed));
        registry = std::move(replacement);
        EXPECT_EQ(builder->TableName(), "items");
        EXPECT_EQ(builder->PrimaryKeyWhere(*row).args.size(), 1u);
    }
    EXPECT_EQ(builder->TableName(), "items");
    EXPECT_EQ(builder->Insert(*row).args.size(), 8u);
}

TEST(CodecDeterministic, LargeMapsAndNestedMapsKeepStableWireBytes) {
    google::protobuf::DescriptorPool pool;
    google::protobuf::FileDescriptorProto file;
    file.set_name("deterministic_maps.proto");
    file.set_package("maptest");
    file.set_syntax("proto3");
    auto *row_descriptor = file.add_message_type();
    row_descriptor->set_name("Row");
    auto add_field = [](google::protobuf::DescriptorProto *message, const char *name, int number,
                        FieldDescriptorProto::Type type, bool repeated = false,
                        const char *type_name = "") {
        auto *field = message->add_field();
        field->set_name(name);
        field->set_number(number);
        field->set_type(type);
        field->set_label(repeated ? FieldDescriptorProto::LABEL_REPEATED
                                  : FieldDescriptorProto::LABEL_OPTIONAL);
        if (*type_name)
            field->set_type_name(type_name);
    };
    auto add_map_entry = [&](google::protobuf::DescriptorProto *owner, const char *name) {
        auto *entry = owner->add_nested_type();
        entry->set_name(name);
        entry->mutable_options()->set_map_entry(true);
        add_field(entry, "key", 1, FieldDescriptorProto::TYPE_STRING);
        add_field(entry, "value", 2, FieldDescriptorProto::TYPE_UINT64);
    };
    add_field(row_descriptor, "id", 1, FieldDescriptorProto::TYPE_UINT64);
    add_map_entry(row_descriptor, "AttributesEntry");
    add_field(row_descriptor, "attributes", 2, FieldDescriptorProto::TYPE_MESSAGE, true,
              ".maptest.Row.AttributesEntry");
    auto *payload = row_descriptor->add_nested_type();
    payload->set_name("Payload");
    add_map_entry(payload, "LabelsEntry");
    add_field(payload, "labels", 1, FieldDescriptorProto::TYPE_MESSAGE, true,
              ".maptest.Row.Payload.LabelsEntry");
    add_field(row_descriptor, "payload", 3, FieldDescriptorProto::TYPE_MESSAGE, false,
              ".maptest.Row.Payload");
    const auto *built = pool.BuildFile(file);
    ASSERT_NE(built, nullptr);
    google::protobuf::DynamicMessageFactory factory(&pool);
    std::unique_ptr<Message> first(factory.GetPrototype(built->message_type(0))->New());
    std::unique_ptr<Message> reversed(first->New());
    const auto *descriptor = first->GetDescriptor();
    const auto *attributes = descriptor->FindFieldByName("attributes");
    const auto *payload_field = descriptor->FindFieldByName("payload");
    auto fill_map = [](Message *owner, const google::protobuf::FieldDescriptor *field,
                       bool reverse) {
        for (int i = 0; i < 128; ++i) {
            const int index = reverse ? 127 - i : i;
            auto *entry = owner->GetReflection()->AddMessage(owner, field);
            entry->GetReflection()->SetString(entry, entry->GetDescriptor()->FindFieldByName("key"),
                                              "key-" + std::to_string(index));
            entry->GetReflection()->SetUInt64(entry,
                                              entry->GetDescriptor()->FindFieldByName("value"),
                                              uint64_t(index) * 9007199254740993ULL);
        }
    };
    for (auto *message : {first.get(), reversed.get()}) {
        const bool reverse = message == reversed.get();
        fill_map(message, attributes, reverse);
        auto *child = message->GetReflection()->MutableMessage(message, payload_field);
        fill_map(child, child->GetDescriptor()->FindFieldByName("labels"), reverse);
    }
    const auto expected_map = FieldValue(*first, attributes);
    const auto expected_nested = FieldValue(*first, payload_field);
    EXPECT_EQ(FieldValue(*reversed, attributes), expected_map);
    EXPECT_EQ(FieldValue(*reversed, payload_field), expected_nested);
    TableOptions options;
    options.primary_keys = StringVector{"id"};
    MessageTable table(*first, options);
    SQLBuilder builder(table);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(FieldValue(*first, attributes), expected_map);
        EXPECT_EQ(FieldValue(*first, payload_field), expected_nested);
        const auto update = builder.UpdateFieldsByPK(*first, {"attributes", "payload"});
        ASSERT_EQ(update.args.size(), 3u);
        EXPECT_EQ(update.args[0], expected_map);
        EXPECT_EQ(update.args[1], expected_nested);
        ParseRow(*first, {uint64_t(0), expected_map, expected_nested});
    }
}
} // namespace
