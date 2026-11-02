#include "src/mysql_wrapper/value_codec.h"

#include <charconv>
#include <cmath>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/util/time_util.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace pbmysql {
namespace {
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
void ValidateField(const Message &message, const FieldDescriptor *field) {
    if (!field || field->containing_type() != message.GetDescriptor())
        throw std::invalid_argument("field does not belong to message");
    if (field->real_containing_oneof() != nullptr)
        throw std::invalid_argument("real oneof fields are not supported: " +
                                    std::string(field->name()));
}
bool IsTimestamp(const FieldDescriptor *f) {
    return !f->is_repeated() && f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE &&
           f->message_type()->full_name() == "google.protobuf.Timestamp";
}
template <class T> T Integer(const std::string &text) {
    T value{};
    const char *start = text.data();
    if constexpr (std::is_signed_v<T>)
        if (!text.empty() && text.front() == '+')
            ++start;
    auto result = std::from_chars(start, text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid or out-of-range integer value");
    return value;
}
double Floating(const std::string &text) {
    double value{};
    const char *start = text.data();
    if (!text.empty() && text.front() == '+')
        ++start;
    auto result = std::from_chars(start, text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(value))
        throw std::invalid_argument("invalid or non-finite floating-point value");
    return value;
}
bool Boolean(const std::string &text) {
    if (text == "1" || text == "t" || text == "T" || text == "true" || text == "TRUE" ||
        text == "True")
        return true;
    if (text == "0" || text == "f" || text == "F" || text == "false" || text == "FALSE" ||
        text == "False")
        return false;
    throw std::invalid_argument("invalid bool value");
}
void SetDefault(Message &message, const FieldDescriptor *field) {
    const auto *r = message.GetReflection();
    if (field->is_repeated() || field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
        r->ClearField(&message, field);
        return;
    }
    switch (field->type()) {
    case FieldDescriptor::TYPE_INT32:
        r->SetInt32(&message, field, field->default_value_int32());
        break;
    case FieldDescriptor::TYPE_INT64:
        r->SetInt64(&message, field, field->default_value_int64());
        break;
    case FieldDescriptor::TYPE_UINT32:
        r->SetUInt32(&message, field, field->default_value_uint32());
        break;
    case FieldDescriptor::TYPE_UINT64:
        r->SetUInt64(&message, field, field->default_value_uint64());
        break;
    case FieldDescriptor::TYPE_FLOAT:
        r->SetFloat(&message, field, field->default_value_float());
        break;
    case FieldDescriptor::TYPE_DOUBLE:
        r->SetDouble(&message, field, field->default_value_double());
        break;
    case FieldDescriptor::TYPE_BOOL:
        r->SetBool(&message, field, field->default_value_bool());
        break;
    case FieldDescriptor::TYPE_ENUM:
        r->SetEnumValue(&message, field, field->default_value_enum()->number());
        break;
    case FieldDescriptor::TYPE_STRING:
    case FieldDescriptor::TYPE_BYTES:
        r->SetString(&message, field, std::string(field->default_value_string()));
        break;
    default:
        throw std::invalid_argument("unsupported protobuf field kind: " +
                                    std::string(field->name()));
    }
}
std::string SerializeChecked(const Message &message) {
    std::string data;
    {
        google::protobuf::io::StringOutputStream output(&data);
        google::protobuf::io::CodedOutputStream coded(&output);
        // Maps have unspecified iteration order, including in copied holders.
        // UPDATE values and unchanged-row predicates must use identical bytes
        // for the same logical value. This also canonicalizes nested maps.
        coded.SetSerializationDeterministic(true);
        if (!message.SerializeToCodedStream(&coded) || coded.HadError())
            throw std::invalid_argument("cannot serialize protobuf message");
    }
    return data;
}
} // namespace

bool IsNumericField(const FieldDescriptor *f) {
    if (!f || f->is_repeated())
        return false;
    switch (f->type()) {
    case FieldDescriptor::TYPE_INT32:
    case FieldDescriptor::TYPE_INT64:
    case FieldDescriptor::TYPE_UINT32:
    case FieldDescriptor::TYPE_UINT64:
    case FieldDescriptor::TYPE_FLOAT:
    case FieldDescriptor::TYPE_DOUBLE:
        return true;
    default:
        return false;
    }
}

std::string ValueText(const SqlValue &value) {
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
                return {};
            else if constexpr (std::is_same_v<T, std::string>)
                return v;
            else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(v))
                    throw std::invalid_argument("non-finite floating-point value");
                char text[128];
                const auto result = std::to_chars(text, text + sizeof(text), v);
                if (result.ec != std::errc{})
                    throw std::invalid_argument("cannot format floating-point value");
                return std::string(text, result.ptr);
            } else
                return std::to_string(v);
        },
        value);
}

SqlValue FieldValue(const Message &message, const FieldDescriptor *field) {
    ValidateField(message, field);
    const auto *reflection = message.GetReflection();
    if (field->is_repeated()) {
        // Identical to Go serializeContainer: the original message type, with
        // only this field present, encoded with its original protobuf tag.
        std::unique_ptr<Message> holder(message.New());
        holder->CopyFrom(message);
        const auto *descriptor = message.GetDescriptor();
        for (int i = 0; i < descriptor->field_count(); ++i)
            if (descriptor->field(i) != field)
                holder->GetReflection()->ClearField(holder.get(), descriptor->field(i));
        holder->GetReflection()->MutableUnknownFields(holder.get())->Clear();
        return SerializeChecked(*holder);
    }
    if (IsTimestamp(field)) {
        if (!reflection->HasField(message, field))
            return nullptr;
        const auto &stamp = reflection->GetMessage(message, field);
        const auto *d = stamp.GetDescriptor();
        const auto *r = stamp.GetReflection();
        const auto seconds = r->GetInt64(stamp, d->FindFieldByName("seconds"));
        const auto nanos = r->GetInt32(stamp, d->FindFieldByName("nanos"));
        if (seconds == -62135596800LL && nanos == 0)
            return nullptr;
        if (seconds < -30610224000LL || seconds > 253402300799LL || nanos < 0 || nanos > 999999999)
            throw std::invalid_argument("Timestamp is outside MySQL DATETIME range");
        google::protobuf::Timestamp ts;
        ts.set_seconds(seconds);
        ts.set_nanos(nanos / 1000 * 1000);
        std::string text = google::protobuf::util::TimeUtil::ToString(ts);
        text[10] = ' ';
        text.pop_back();
        if (text.size() == 19)
            text += ".000000";
        else
            text.append(26 - text.size(), '0');
        return text;
    }
    switch (field->type()) {
    case FieldDescriptor::TYPE_INT32:
        return int64_t(reflection->GetInt32(message, field));
    case FieldDescriptor::TYPE_INT64:
        return reflection->GetInt64(message, field);
    case FieldDescriptor::TYPE_UINT32:
        return uint64_t(reflection->GetUInt32(message, field));
    case FieldDescriptor::TYPE_UINT64:
        return reflection->GetUInt64(message, field);
    case FieldDescriptor::TYPE_BOOL:
        return int64_t(reflection->GetBool(message, field));
    case FieldDescriptor::TYPE_ENUM:
        return int64_t(reflection->GetEnumValue(message, field));
    case FieldDescriptor::TYPE_FLOAT: {
        const auto value = reflection->GetFloat(message, field);
        if (!std::isfinite(value))
            throw std::invalid_argument("non-finite float field: " + std::string(field->name()));
        // A FLOAT parameter must preserve the shortest float32 decimal value,
        // rather than introducing the binary64 expansion of that float32.
        char text[64];
        const auto formatted = std::to_chars(text, text + sizeof(text), value);
        return std::string(text, formatted.ptr);
    }
    case FieldDescriptor::TYPE_DOUBLE: {
        const auto value = reflection->GetDouble(message, field);
        if (!std::isfinite(value))
            throw std::invalid_argument("non-finite double field: " + std::string(field->name()));
        return value;
    }
    case FieldDescriptor::TYPE_STRING:
    case FieldDescriptor::TYPE_BYTES:
        return reflection->GetString(message, field);
    case FieldDescriptor::TYPE_MESSAGE:
        return reflection->HasField(message, field)
                   ? SerializeChecked(reflection->GetMessage(message, field))
                   : std::string{};
    default:
        throw std::invalid_argument("unsupported protobuf field kind: " +
                                    std::string(field->name()));
    }
}

SqlValue FieldComparisonValue(const Message &message, const FieldDescriptor *field) {
    auto value = FieldValue(message, field);
    if (!field->is_repeated() && field->type() == FieldDescriptor::TYPE_FLOAT)
        return static_cast<double>(message.GetReflection()->GetFloat(message, field));
    return value;
}

SqlValue NormalizeComparisonValue(const FieldDescriptor *field, const SqlValue &value) {
    if (!field)
        throw std::invalid_argument("null field descriptor");
    if (std::holds_alternative<std::nullptr_t>(value) || field->is_repeated())
        return value;
    const auto *text = std::get_if<std::string>(&value);
    switch (field->type()) {
    case FieldDescriptor::TYPE_INT32:
        if (text)
            return int64_t(Integer<int32_t>(*text));
        break;
    case FieldDescriptor::TYPE_INT64:
        if (text)
            return Integer<int64_t>(*text);
        break;
    case FieldDescriptor::TYPE_UINT32:
        if (text)
            return uint64_t(Integer<uint32_t>(*text));
        break;
    case FieldDescriptor::TYPE_UINT64:
        if (text)
            return Integer<uint64_t>(*text);
        break;
    case FieldDescriptor::TYPE_ENUM:
        if (text)
            return int64_t(Integer<int32_t>(*text));
        break;
    case FieldDescriptor::TYPE_BOOL:
        if (text)
            return int64_t(Boolean(*text));
        break;
    case FieldDescriptor::TYPE_FLOAT: {
        const double number =
            text ? Floating(*text)
                 : std::visit(
                       [](const auto &v) -> double {
                           using T = std::decay_t<decltype(v)>;
                           if constexpr (std::is_arithmetic_v<T>)
                               return static_cast<double>(v);
                           else
                               throw std::invalid_argument("invalid float comparison value");
                       },
                       value);
        const float rounded = static_cast<float>(number);
        if (!std::isfinite(rounded))
            throw std::invalid_argument("non-finite float comparison value");
        return static_cast<double>(rounded);
    }
    case FieldDescriptor::TYPE_DOUBLE:
        if (text)
            return Floating(*text);
        break;
    default:
        return value; // Opaque BLOB and DATETIME predicates preserve exact supplied bytes.
    }
    if (const auto *number = std::get_if<double>(&value); number && !std::isfinite(*number))
        throw std::invalid_argument("non-finite comparison value");
    return value;
}

void ParseRow(Message &message, const std::vector<SqlValue> &row) {
    const auto *descriptor = message.GetDescriptor();
    if (row.size() != static_cast<size_t>(descriptor->field_count()))
        throw std::invalid_argument("result column count differs from message field count");
    std::unique_ptr<Message> parsed(message.New());
    auto *reflection = parsed->GetReflection();
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto *field = descriptor->field(i);
        ValidateField(*parsed, field);
        if (std::holds_alternative<std::nullptr_t>(row[i])) {
            SetDefault(*parsed, field);
            continue;
        }
        const std::string text = ValueText(row[i]);
        if (text.empty()) {
            SetDefault(*parsed, field);
            continue;
        }
        if (field->is_repeated()) {
            std::unique_ptr<Message> holder(message.New());
            if (!holder->ParseFromString(text))
                throw std::invalid_argument("invalid collection protobuf: " +
                                            std::string(field->name()));
            reflection->SwapFields(parsed.get(), holder.get(), {field});
        } else if (IsTimestamp(field)) {
            std::string iso = text;
            if (iso.size() == 10)
                iso += "T00:00:00Z";
            else if (iso.size() >= 19 && iso[10] == ' ') {
                iso[10] = 'T';
                iso += 'Z';
            }
            google::protobuf::Timestamp ts;
            if (!google::protobuf::util::TimeUtil::FromString(iso, &ts))
                throw std::invalid_argument("invalid DATETIME value");
            auto *target = reflection->MutableMessage(parsed.get(), field);
            const auto *target_desc = target->GetDescriptor();
            target->GetReflection()->SetInt64(target, target_desc->FindFieldByName("seconds"),
                                              ts.seconds());
            target->GetReflection()->SetInt32(target, target_desc->FindFieldByName("nanos"),
                                              ts.nanos());
        } else {
            switch (field->type()) {
            case FieldDescriptor::TYPE_INT32:
                reflection->SetInt32(parsed.get(), field, Integer<int32_t>(text));
                break;
            case FieldDescriptor::TYPE_INT64:
                reflection->SetInt64(parsed.get(), field, Integer<int64_t>(text));
                break;
            case FieldDescriptor::TYPE_UINT32:
                reflection->SetUInt32(parsed.get(), field, Integer<uint32_t>(text));
                break;
            case FieldDescriptor::TYPE_UINT64:
                reflection->SetUInt64(parsed.get(), field, Integer<uint64_t>(text));
                break;
            case FieldDescriptor::TYPE_BOOL:
                reflection->SetBool(parsed.get(), field, Boolean(text));
                break;
            case FieldDescriptor::TYPE_ENUM:
                reflection->SetEnumValue(parsed.get(), field, Integer<int32_t>(text));
                break;
            case FieldDescriptor::TYPE_FLOAT: {
                const double value = Floating(text);
                if (std::abs(value) > std::numeric_limits<float>::max())
                    throw std::invalid_argument("float overflow");
                reflection->SetFloat(parsed.get(), field, static_cast<float>(value));
                break;
            }
            case FieldDescriptor::TYPE_DOUBLE:
                reflection->SetDouble(parsed.get(), field, Floating(text));
                break;
            case FieldDescriptor::TYPE_STRING:
            case FieldDescriptor::TYPE_BYTES:
                reflection->SetString(parsed.get(), field, text);
                break;
            case FieldDescriptor::TYPE_MESSAGE:
                if (!reflection->MutableMessage(parsed.get(), field)->ParseFromString(text))
                    throw std::invalid_argument("invalid message protobuf: " +
                                                std::string(field->name()));
                break;
            default:
                throw std::invalid_argument("unsupported protobuf field kind: " +
                                            std::string(field->name()));
            }
        }
    }
    message.CopyFrom(*parsed);
}
} // namespace pbmysql
