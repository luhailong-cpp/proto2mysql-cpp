#pragma once

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbmysql {

bool FileHasDBOption(const google::protobuf::FileDescriptor *file);
std::optional<std::string> TableNameFromDescriptor(const google::protobuf::Descriptor *descriptor);

// Portable generated-proto discovery: register a root once, then scan every
// annotated message in its file and dependencies without per-table calls.
void RegisterProtoFile(const google::protobuf::FileDescriptor *file);
void RegisterProtoFileName(const std::string &file_name);
std::vector<const google::protobuf::FileDescriptor *> RegisteredProtoFiles();
class ProtoFileRegistration {
  public:
    explicit ProtoFileRegistration(const google::protobuf::FileDescriptor *file) {
        RegisterProtoFile(file);
    }
    explicit ProtoFileRegistration(const std::string &name) { RegisterProtoFileName(name); }
};

// Owns descriptors and dynamic-message prototypes. Keep this object alive for
// as long as messages created from it; Database retains shared ownership.
class DescriptorSet {
  public:
    explicit DescriptorSet(const google::protobuf::FileDescriptorSet &descriptors);
    static std::shared_ptr<DescriptorSet> Load(const std::string &path);
    const std::vector<const google::protobuf::FileDescriptor *> &Files() const { return files_; }
    const google::protobuf::Message *Prototype(const google::protobuf::Descriptor *descriptor);
    std::unique_ptr<google::protobuf::Message> NewMessage(const std::string &full_name);

  private:
    google::protobuf::DescriptorPool pool_;
    google::protobuf::DynamicMessageFactory factory_;
    std::vector<const google::protobuf::FileDescriptor *> files_;
};

} // namespace pbmysql

#define PBMYSQL_DETAIL_JOIN_IMPL(a, b) a##b
#define PBMYSQL_DETAIL_JOIN(a, b) PBMYSQL_DETAIL_JOIN_IMPL(a, b)
#define PBMYSQL_REGISTER_PROTO_FILE(MessageType)                                                   \
    namespace {                                                                                    \
    const ::pbmysql::ProtoFileRegistration PBMYSQL_DETAIL_JOIN(pbmysql_proto_file_, __COUNTER__){  \
        MessageType::descriptor()->file()};                                                        \
    }
