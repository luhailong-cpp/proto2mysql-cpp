#include "src/mysql_wrapper/sql_builder.h"
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/timestamp.pb.h>
#include <gtest/gtest.h>

namespace {
using namespace pbmysql;
using google::protobuf::FieldDescriptorProto;
using google::protobuf::Message;
class CodecParity : public ::testing::Test {
  protected:
    google::protobuf::DescriptorPool pool{google::protobuf::DescriptorPool::generated_pool()};
    google::protobuf::DynamicMessageFactory factory{&pool};
    std::unique_ptr<Message> message;
    const google::protobuf::FieldDescriptor *F(const std::string &name) const {
        return message->GetDescriptor()->FindFieldByName(name);
    }
    void SetUp() override {
        (void)google::protobuf::Timestamp::descriptor();
        google::protobuf::FileDescriptorProto file;
        file.set_name("codec_optional.proto");
        file.set_package("codec");
        file.set_syntax("proto2");
        file.add_dependency("google/protobuf/timestamp.proto");
        auto *row = file.add_message_type();
        row->set_name("Row");
        auto *enumeration = row->add_enum_type();
        enumeration->set_name("Tier");
        auto *value = enumeration->add_value();
        value->set_name("ZERO");
        value->set_number(0);
        value = enumeration->add_value();
        value->set_name("SEVEN");
        value->set_number(7);
        auto *child = row->add_nested_type();
        child->set_name("Child");
        auto *count = child->add_field();
        count->set_name("count");
        count->set_number(1);
        count->set_type(FieldDescriptorProto::TYPE_INT32);
        count->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
        auto add = [&](const char *name, FieldDescriptorProto::Type type, const char *initial = "",
                       const char *type_name = "") {
            auto *field = row->add_field();
            field->set_name(name);
            field->set_number(row->field_size());
            field->set_type(type);
            field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
            if (*initial)
                field->set_default_value(initial);
            if (*type_name)
                field->set_type_name(type_name);
        };
        add("i32", FieldDescriptorProto::TYPE_INT32, "42");
        add("i64", FieldDescriptorProto::TYPE_INT64, "-7");
        add("u32", FieldDescriptorProto::TYPE_UINT32, "13");
        add("u64", FieldDescriptorProto::TYPE_UINT64, "123");
        add("ratio", FieldDescriptorProto::TYPE_FLOAT, "1.5");
        add("score", FieldDescriptorProto::TYPE_DOUBLE, "2.5");
        add("flag", FieldDescriptorProto::TYPE_BOOL, "true");
        add("name", FieldDescriptorProto::TYPE_STRING, "default");
        add("data", FieldDescriptorProto::TYPE_BYTES);
        add("tier", FieldDescriptorProto::TYPE_ENUM, "SEVEN", ".codec.Row.Tier");
        add("child", FieldDescriptorProto::TYPE_MESSAGE, "", ".codec.Row.Child");
        add("time", FieldDescriptorProto::TYPE_MESSAGE, "", ".google.protobuf.Timestamp");
        const auto *built = pool.BuildFile(file);
        ASSERT_NE(built, nullptr);
        message.reset(factory.GetPrototype(built->message_type(0))->New());
    }
    std::vector<SqlValue> EmptyRow(SqlValue value = nullptr) const {
        return std::vector<SqlValue>(message->GetDescriptor()->field_count(), value);
    }
};

TEST_F(CodecParity, NullAndEmptyScalarsHaveDefaultPresenceAndClearMessages) {
    for (const SqlValue empty : {SqlValue(nullptr), SqlValue(std::string{})}) {
        auto *child = message->GetReflection()->MutableMessage(message.get(), F("child"));
        child->GetReflection()->SetInt32(child, child->GetDescriptor()->FindFieldByName("count"),
                                         5);
        message->GetReflection()->MutableMessage(message.get(), F("time"));
        ParseRow(*message, EmptyRow(empty));
        for (int i = 0; i < 10; ++i)
            EXPECT_TRUE(
                message->GetReflection()->HasField(*message, message->GetDescriptor()->field(i)));
        EXPECT_EQ(message->GetReflection()->GetInt32(*message, F("i32")), 42);
        EXPECT_EQ(message->GetReflection()->GetEnumValue(*message, F("tier")), 7);
        EXPECT_EQ(message->GetReflection()->GetString(*message, F("name")), "default");
        EXPECT_FALSE(message->GetReflection()->HasField(*message, F("child")));
        EXPECT_FALSE(message->GetReflection()->HasField(*message, F("time")));
        EXPECT_EQ(SQLBuilder(*message).InsertSetFields(*message).args.size(), 10u);
    }
}

TEST_F(CodecParity, BooleanAliasesAndSignedPositiveTextMatchGoParser) {
    for (const auto &text :
         {"1", "t", "T", "true", "TRUE", "True", "0", "f", "F", "false", "FALSE", "False"}) {
        auto values = EmptyRow();
        values[F("flag")->index()] = std::string(text);
        ParseRow(*message, values);
        EXPECT_EQ(message->GetReflection()->GetBool(*message, F("flag")),
                  text[0] == '1' || text[0] == 't' || text[0] == 'T');
    }
    auto values = EmptyRow();
    values[0] = std::string("+17");
    values[4] = std::string("+1.25");
    ParseRow(*message, values);
    EXPECT_EQ(message->GetReflection()->GetInt32(*message, F("i32")), 17);
    EXPECT_EQ(message->GetReflection()->GetFloat(*message, F("ratio")), 1.25f);
    values[0] = std::string("+");
    EXPECT_THROW(ParseRow(*message, values), std::invalid_argument);
}

TEST_F(CodecParity, TimestampAcceptsDateNativeAndRFC3339Offsets) {
    for (const auto &input : {"2026-01-02", "2026-01-02 00:00:00", "2026-01-02T00:00:00Z",
                              "2026-01-02T01:00:00+01:00"}) {
        auto values = EmptyRow();
        values[F("time")->index()] = std::string(input);
        ParseRow(*message, values);
        EXPECT_EQ(std::get<std::string>(FieldValue(*message, F("time"))),
                  "2026-01-02 00:00:00.000000");
    }
    auto values = EmptyRow();
    values[F("time")->index()] = std::string("2026-01-02T00:00:00.123456789Z");
    ParseRow(*message, values);
    EXPECT_EQ(std::get<std::string>(FieldValue(*message, F("time"))), "2026-01-02 00:00:00.123456");
    values[F("time")->index()] = std::string("invalid");
    const auto previous = message->SerializeAsString();
    EXPECT_THROW(ParseRow(*message, values), std::invalid_argument);
    EXPECT_EQ(message->SerializeAsString(), previous);
    auto *timestamp = message->GetReflection()->MutableMessage(message.get(), F("time"));
    timestamp->GetReflection()->SetInt64(
        timestamp, timestamp->GetDescriptor()->FindFieldByName("seconds"), -62135596800LL);
    timestamp->GetReflection()->SetInt32(timestamp,
                                         timestamp->GetDescriptor()->FindFieldByName("nanos"), 0);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(FieldValue(*message, F("time"))));
}

TEST_F(CodecParity, IntegerBoundariesAreExactAndFailuresLeaveMessageUnchanged) {
    auto values = EmptyRow();
    values[F("i32")->index()] = std::string("-2147483648");
    values[F("i64")->index()] = std::string("-9223372036854775808");
    values[F("u32")->index()] = std::string("4294967295");
    values[F("u64")->index()] = std::string("18446744073709551615");
    ParseRow(*message, values);
    EXPECT_EQ(std::get<int64_t>(FieldValue(*message, F("i64"))), INT64_MIN);
    EXPECT_EQ(std::get<uint64_t>(FieldValue(*message, F("u64"))), UINT64_MAX);
    const auto expected = message->SerializeAsString();
    for (const auto &invalid :
         std::vector<std::pair<std::string, std::string>>{{"i32", "2147483648"},
                                                          {"i64", "9223372036854775808"},
                                                          {"u32", "4294967296"},
                                                          {"u64", "18446744073709551616"},
                                                          {"u32", "-1"}}) {
        auto bad = values;
        bad[F(invalid.first)->index()] = invalid.second;
        EXPECT_THROW(ParseRow(*message, bad), std::invalid_argument);
        EXPECT_EQ(message->SerializeAsString(), expected);
    }
}

TEST(CodecPreflight, SyntheticOptionalZeroAndNullRetainPresence) {
    google::protobuf::DescriptorPool pool;
    google::protobuf::FileDescriptorProto file;
    file.set_name("synthetic_codec.proto");
    file.set_syntax("proto3");
    auto *row = file.add_message_type();
    row->set_name("Optional");
    row->add_oneof_decl()->set_name("_value");
    auto *field = row->add_field();
    field->set_name("value");
    field->set_number(1);
    field->set_type(FieldDescriptorProto::TYPE_INT32);
    field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    field->set_oneof_index(0);
    field->set_proto3_optional(true);
    const auto *built = pool.BuildFile(file);
    ASSERT_NE(built, nullptr);
    google::protobuf::DynamicMessageFactory factory(&pool);
    std::unique_ptr<Message> message(factory.GetPrototype(built->message_type(0))->New());
    ParseRow(*message, {nullptr});
    EXPECT_TRUE(message->GetReflection()->HasField(*message, message->GetDescriptor()->field(0)));
    EXPECT_EQ(SQLBuilder(*message).InsertSetFields(*message).args.size(), 1u);
    EXPECT_EQ(std::get<int64_t>(FieldValue(*message, message->GetDescriptor()->field(0))), 0);
}

TEST(CodecPreflight, UnsupportedIntegerWireKindsFailAtSchemaAndCodec) {
    for (auto kind : {FieldDescriptorProto::TYPE_SINT32, FieldDescriptorProto::TYPE_SINT64,
                      FieldDescriptorProto::TYPE_FIXED32, FieldDescriptorProto::TYPE_FIXED64,
                      FieldDescriptorProto::TYPE_SFIXED32, FieldDescriptorProto::TYPE_SFIXED64}) {
        google::protobuf::DescriptorPool pool;
        google::protobuf::FileDescriptorProto file;
        file.set_name("unsupported_codec.proto");
        file.set_syntax("proto3");
        auto *row = file.add_message_type();
        row->set_name("Unsupported");
        auto *field = row->add_field();
        field->set_name("value");
        field->set_number(1);
        field->set_type(kind);
        field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
        const auto *built = pool.BuildFile(file);
        ASSERT_NE(built, nullptr);
        google::protobuf::DynamicMessageFactory factory(&pool);
        std::unique_ptr<Message> message(factory.GetPrototype(built->message_type(0))->New());
        EXPECT_THROW(MessageTable table(*message), std::invalid_argument);
        EXPECT_THROW(FieldValue(*message, message->GetDescriptor()->field(0)),
                     std::invalid_argument);
        EXPECT_THROW(ParseRow(*message, {nullptr}), std::invalid_argument);
    }
}

TEST(CodecPreflight, RealOneofAndUnsupportedKindsRejectWithoutMutatingCaller) {
    google::protobuf::DescriptorPool pool;
    google::protobuf::FileDescriptorProto file;
    file.set_name("oneof_codec.proto");
    file.set_syntax("proto3");
    auto *row = file.add_message_type();
    row->set_name("Oneof");
    row->add_oneof_decl()->set_name("choice");
    auto *field = row->add_field();
    field->set_name("id");
    field->set_number(1);
    field->set_type(FieldDescriptorProto::TYPE_INT64);
    field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    field = row->add_field();
    field->set_name("name");
    field->set_number(2);
    field->set_type(FieldDescriptorProto::TYPE_STRING);
    field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    field->set_oneof_index(0);
    const auto *built = pool.BuildFile(file);
    ASSERT_NE(built, nullptr);
    google::protobuf::DynamicMessageFactory factory(&pool);
    std::unique_ptr<Message> message(factory.GetPrototype(built->message_type(0))->New());
    message->GetReflection()->SetInt64(message.get(), message->GetDescriptor()->field(0), 99);
    EXPECT_THROW(ParseRow(*message, {int64_t(1), std::string("x")}), std::invalid_argument);
    EXPECT_EQ(message->GetReflection()->GetInt64(*message, message->GetDescriptor()->field(0)), 99);
    EXPECT_THROW(FieldValue(*message, message->GetDescriptor()->field(1)), std::invalid_argument);
}
} // namespace
