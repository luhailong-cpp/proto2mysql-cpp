#include "src/mysql_wrapper/descriptor_registry.h"
#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace pbmysql {
namespace {
struct Registry {
    std::mutex mutex;
    std::map<std::string, const google::protobuf::FileDescriptor *> files;
    std::set<std::string> names;
};
Registry &GlobalRegistry() {
    // Generated descriptors have process lifetime. Avoid static-destruction
    // ordering between this registry and protobuf's generated descriptor pool.
    static Registry *registry = new Registry;
    return *registry;
}
std::optional<std::string> StringOption(const google::protobuf::Message &options, int number) {
    std::vector<const google::protobuf::FieldDescriptor *> fields;
    options.GetReflection()->ListFields(options, &fields);
    for (const auto *field : fields) {
        if (field->number() == number &&
            field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING)
            return options.GetReflection()->GetString(options, field);
    }
    const auto &unknown = options.GetReflection()->GetUnknownFields(options);
    for (int i = unknown.field_count() - 1; i >= 0; --i) {
        if (unknown.field(i).number() == number &&
            unknown.field(i).type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED)
            return std::string(unknown.field(i).length_delimited());
    }
    return std::nullopt;
}
} // namespace

bool FileHasDBOption(const google::protobuf::FileDescriptor *file) {
    if (!file)
        throw std::invalid_argument("null file descriptor");
    const auto &options = file->options();
    if (options.HasExtension(proto2mysql::db))
        return options.GetExtension(proto2mysql::db);
    const auto &unknown = options.GetReflection()->GetUnknownFields(options);
    for (int i = unknown.field_count() - 1; i >= 0; --i)
        if (unknown.field(i).number() == 500000 &&
            unknown.field(i).type() == google::protobuf::UnknownField::TYPE_VARINT)
            return unknown.field(i).varint() != 0;
    return false;
}
std::optional<std::string> TableNameFromDescriptor(const google::protobuf::Descriptor *descriptor) {
    if (!descriptor)
        throw std::invalid_argument("null message descriptor");
    auto name = StringOption(descriptor->options(), 500001);
    return name && !name->empty() ? name : std::nullopt;
}
void RegisterProtoFile(const google::protobuf::FileDescriptor *file) {
    if (!file)
        throw std::invalid_argument("null file descriptor");
    if (file->pool() != google::protobuf::DescriptorPool::generated_pool())
        throw std::invalid_argument(
            "dynamic proto files require RegisterAllTables(shared DescriptorSet)");
    auto &registry = GlobalRegistry();
    std::lock_guard<std::mutex> guard(registry.mutex);
    auto candidate = registry.files;
    std::function<void(const google::protobuf::FileDescriptor *)> add = [&](const auto *current) {
        std::string name(current->name());
        auto found = candidate.find(name);
        if (found != candidate.end()) {
            if (found->second != current)
                throw std::invalid_argument("conflicting proto file: " + name);
            return;
        }
        candidate.emplace(name, current);
        for (int i = 0; i < current->dependency_count(); ++i)
            add(current->dependency(i));
    };
    add(file);
    registry.files = std::move(candidate);
}
std::vector<const google::protobuf::FileDescriptor *> RegisteredProtoFiles() {
    auto &registry = GlobalRegistry();
    std::set<std::string> names;
    {
        std::lock_guard<std::mutex> guard(registry.mutex);
        names = registry.names;
    }
    for (const auto &name : names) {
        const auto *file = google::protobuf::DescriptorPool::generated_pool()->FindFileByName(name);
        if (!file)
            throw std::invalid_argument("registered proto file is not linked: " + name);
        RegisterProtoFile(file);
    }
    std::lock_guard<std::mutex> guard(registry.mutex);
    std::vector<const google::protobuf::FileDescriptor *> result;
    for (const auto &pair : registry.files)
        result.push_back(pair.second);
    return result;
}
void RegisterProtoFileName(const std::string &name) {
    if (name.empty() || name.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid proto file name");
    auto &registry = GlobalRegistry();
    std::lock_guard<std::mutex> guard(registry.mutex);
    registry.names.insert(name);
}

DescriptorSet::DescriptorSet(const google::protobuf::FileDescriptorSet &descriptors)
    : factory_(&pool_) {
    if (!descriptors.file_size())
        throw std::invalid_argument("descriptor set contains no files");
    std::map<std::string, google::protobuf::FileDescriptorProto> pending;
    std::set<std::string> requested;
    for (const auto &file : descriptors.file()) {
        if (file.name().empty() || !pending.emplace(file.name(), file).second)
            throw std::invalid_argument("empty or duplicate descriptor file name");
        requested.insert(file.name());
    }
    // Input descriptors win over linked application descriptors. Only missing
    // dependencies are copied from the generated pool, avoiding stale linked
    // schemas silently replacing an explicitly supplied descriptor set.
    std::function<void(const std::string &)> dependency = [&](const std::string &name) {
        if (pending.count(name))
            return;
        const auto *generated =
            google::protobuf::DescriptorPool::generated_pool()->FindFileByName(name);
        if (!generated)
            throw std::invalid_argument("unresolved proto import: " + name +
                                        "; use protoc --include_imports");
        google::protobuf::FileDescriptorProto file;
        generated->CopyTo(&file);
        pending.emplace(name, file);
        for (const auto &import : file.dependency())
            dependency(import);
    };
    std::vector<std::string> imports;
    for (const auto &item : pending)
        for (const auto &import : item.second.dependency())
            imports.push_back(import);
    for (const auto &import : imports)
        dependency(import);
    while (!pending.empty()) {
        bool progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            bool ready = true;
            for (const auto &import : it->second.dependency())
                if (!pool_.FindFileByName(import))
                    ready = false;
            if (!ready) {
                ++it;
                continue;
            }
            const auto *built = pool_.BuildFile(it->second);
            if (!built)
                throw std::invalid_argument("invalid proto descriptor: " + it->first);
            if (requested.count(it->first))
                files_.push_back(built);
            it = pending.erase(it);
            progress = true;
        }
        if (!progress)
            throw std::invalid_argument("cyclic or unresolved descriptor imports");
    }
    std::sort(files_.begin(), files_.end(),
              [](const auto *a, const auto *b) { return a->name() < b->name(); });
}
std::shared_ptr<DescriptorSet> DescriptorSet::Load(const std::string &path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    google::protobuf::FileDescriptorSet descriptors;
    if (!input || !descriptors.ParseFromIstream(&input))
        throw std::invalid_argument("cannot read descriptor set: " + path);
    return std::make_shared<DescriptorSet>(descriptors);
}
const google::protobuf::Message *
DescriptorSet::Prototype(const google::protobuf::Descriptor *descriptor) {
    if (!descriptor || descriptor->file()->pool() != &pool_)
        throw std::invalid_argument("descriptor belongs to another source");
    const auto *prototype = factory_.GetPrototype(descriptor);
    if (!prototype)
        throw std::invalid_argument("cannot construct message: " +
                                    std::string(descriptor->full_name()));
    return prototype;
}
std::unique_ptr<google::protobuf::Message> DescriptorSet::NewMessage(const std::string &name) {
    const auto *descriptor = pool_.FindMessageTypeByName(name);
    if (!descriptor)
        throw std::invalid_argument("message not found in descriptor set: " + name);
    return std::unique_ptr<google::protobuf::Message>(Prototype(descriptor)->New());
}
} // namespace pbmysql
