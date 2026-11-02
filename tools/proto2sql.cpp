#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/message_table.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {
std::string ReadOptionString(const google::protobuf::MessageOptions &options, int number) {
    const auto *reflection = options.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor *> fields;
    reflection->ListFields(options, &fields);
    for (const auto *field : fields)
        if (field->number() == number &&
            field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING)
            return reflection->GetString(options, field);
    const auto &unknown = reflection->GetUnknownFields(options);
    for (int i = 0; i < unknown.field_count(); ++i)
        if (unknown.field(i).number() == number &&
            unknown.field(i).type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED)
            return std::string(unknown.field(i).length_delimited());
    return {};
}
bool FileDB(const google::protobuf::FileOptions &options) {
    if (options.HasExtension(proto2mysql::db))
        return options.GetExtension(proto2mysql::db);
    const auto &unknown = options.GetReflection()->GetUnknownFields(options);
    for (int i = 0; i < unknown.field_count(); ++i)
        if (unknown.field(i).number() == 500000 &&
            unknown.field(i).type() == google::protobuf::UnknownField::TYPE_VARINT)
            return unknown.field(i).varint() != 0;
    return false;
}
void Collect(const google::protobuf::Descriptor *descriptor, bool db_file,
             std::vector<const google::protobuf::Descriptor *> &output) {
    if (!descriptor->options().map_entry() &&
        ((db_file && !ReadOptionString(descriptor->options(), 500001).empty()) ||
         !ReadOptionString(descriptor->options(), 50001).empty()))
        output.push_back(descriptor);
    for (int i = 0; i < descriptor->nested_type_count(); ++i)
        Collect(descriptor->nested_type(i), db_file, output);
}
std::string Generate(const std::string &descriptor_path) {
    std::ifstream input(descriptor_path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open descriptor set: " + descriptor_path);
    google::protobuf::FileDescriptorSet files;
    if (!files.ParseFromIstream(&input))
        throw std::runtime_error("invalid FileDescriptorSet: " + descriptor_path);
    if (files.file_size() == 0)
        throw std::runtime_error("descriptor set contains no files");
    // Touch generated descriptors to register both supported option schemas.
    (void)proto2mysql::db;
    (void)OptionTableName;
    google::protobuf::DescriptorPool pool(google::protobuf::DescriptorPool::generated_pool());
    std::vector<int> pending;
    std::unordered_set<std::string> names;
    for (int i = 0; i < files.file_size(); ++i) {
        const auto &file = files.file(i);
        if (file.name().empty() || !names.insert(file.name()).second)
            throw std::runtime_error("empty or duplicate descriptor file name");
        if (!pool.FindFileByName(file.name()))
            pending.push_back(i);
    }
    while (!pending.empty()) {
        bool progress = false;
        for (auto iter = pending.begin(); iter != pending.end();) {
            const auto &file = files.file(*iter);
            bool ready = true;
            for (const auto &dependency : file.dependency())
                if (!pool.FindFileByName(dependency)) {
                    ready = false;
                    break;
                }
            if (!ready) {
                ++iter;
                continue;
            }
            if (!pool.BuildFile(file))
                throw std::runtime_error("invalid protobuf descriptor: " + file.name());
            iter = pending.erase(iter);
            progress = true;
        }
        if (!progress) {
            std::string unresolved;
            for (int i : pending) {
                if (!unresolved.empty())
                    unresolved += ", ";
                unresolved += files.file(i).name();
            }
            throw std::runtime_error("unresolved or cyclic descriptor imports: " + unresolved +
                                     "; generate the set with protoc --include_imports");
        }
    }
    std::vector<const google::protobuf::Descriptor *> descriptors;
    for (const auto &file_proto : files.file()) {
        const auto *file = pool.FindFileByName(file_proto.name());
        for (int i = 0; i < file->message_type_count(); ++i)
            Collect(file->message_type(i), FileDB(file->options()), descriptors);
    }
    std::sort(descriptors.begin(), descriptors.end(),
              [](const auto *a, const auto *b) { return a->full_name() < b->full_name(); });
    google::protobuf::DynamicMessageFactory factory(&pool);
    std::set<std::string> physical_names;
    std::ostringstream output;
    for (const auto *descriptor : descriptors) {
        const auto *prototype = factory.GetPrototype(descriptor);
        if (!prototype)
            throw std::runtime_error("cannot construct message: " +
                                     std::string(descriptor->full_name()));
        pbmysql::MessageTable table(*prototype);
        auto normalized = table.TableName();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!physical_names.insert(normalized).second)
            throw std::runtime_error("multiple messages map to table: " + table.TableName());
        output << table.CreateTableSql() << "\n\n";
    }
    return output.str();
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::string descriptors, out;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: proto2sql --descriptor-set descriptors.pb [--out schema.sql]\n"
                             "Generate descriptors with protoc --include_imports "
                             "--descriptor_set_out=descriptors.pb.\n";
                return 0;
            }
            if ((arg == "--descriptor-set" || arg == "--out") && i + 1 < argc) {
                auto &destination = arg == "--out" ? out : descriptors;
                if (!destination.empty())
                    throw std::invalid_argument("duplicate argument: " + arg);
                destination = argv[++i];
            } else
                throw std::invalid_argument("unknown argument or missing value: " + arg);
        }
        if (descriptors.empty())
            throw std::invalid_argument("--descriptor-set is required");
        const auto sql = Generate(descriptors); // Validate everything before opening the output.
        if (out.empty()) {
            std::cout << sql;
            if (!std::cout)
                throw std::runtime_error("cannot write SQL to stdout");
        } else {
            std::ofstream output(out, std::ios::binary | std::ios::trunc);
            if (!output || !(output << sql))
                throw std::runtime_error("cannot write SQL file: " + out);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "proto2sql: " << error.what() << '\n';
        return 1;
    }
}
