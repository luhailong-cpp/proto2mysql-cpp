#include "db_base.pb.h"
#include "proto2mysql_option.pb.h"
#include "src/mysql_wrapper/descriptor_registry.h"
#include "src/mysql_wrapper/identifier_utils.h"
#include "src/mysql_wrapper/message_table.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {
namespace fs = std::filesystem;
constexpr int kTemporaryDirectoryAttempts = 100;
constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr std::string_view kSerialDevicePrefix = "COM";
constexpr std::string_view kPrinterDevicePrefix = "LPT";
constexpr std::string_view kShortImportFlag = "-I";
#ifdef _WIN32
constexpr std::size_t kInitialExecutablePathCapacity = 512;
// Win32 extended paths allow 32767 UTF-16 code units plus the terminating NUL.
constexpr std::size_t kMaxWindowsPathCapacity = 32768;
constexpr std::size_t kPathBufferGrowthFactor = 2;
constexpr std::size_t kWindowsBackslashEscapeMultiplier = 2;
#else
constexpr int kExecFailureExitCode = 127;
#endif
std::string PathUtf8(const fs::path &path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}
struct Config {
    std::vector<std::string> sources, imports;
    std::string descriptors, out, single, protoc = "protoc";
    bool drop = false, require_db = false, output_directory = false;
    fs::path executable_directory;
};
struct Table {
    std::string name, sql;
};
const char *kDropBanner = "-- WARNING: destructive DROP mode deletes each table and all its "
                          "data.\n-- Use only for empty/test database initialization.\n";
std::string OptionString(const google::protobuf::MessageOptions &options, int number) {
    std::vector<const google::protobuf::FieldDescriptor *> fields;
    options.GetReflection()->ListFields(options, &fields);
    for (const auto *field : fields)
        if (field->number() == number &&
            field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING)
            return options.GetReflection()->GetString(options, field);
    const auto &unknown = options.GetReflection()->GetUnknownFields(options);
    for (int i = 0; i < unknown.field_count(); ++i)
        if (unknown.field(i).number() == number &&
            unknown.field(i).type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED)
            return std::string(unknown.field(i).length_delimited());
    return {};
}
void Collect(const google::protobuf::Descriptor *message,
             std::vector<const google::protobuf::Descriptor *> &output) {
    if (!message->options().map_entry() &&
        (!OptionString(message->options(), proto2mysql::kTableNameFieldNumber).empty() ||
         !OptionString(message->options(), ::kOptionTableNameFieldNumber).empty()))
        output.push_back(message);
    for (int i = 0; i < message->nested_type_count(); ++i)
        Collect(message->nested_type(i), output);
}
std::vector<Table> Generate(const Config &cfg, const std::set<std::string> &roots) {
    auto descriptors = pbmysql::DescriptorSet::Load(cfg.descriptors);
    if (descriptors->Files().empty())
        throw std::runtime_error("descriptor set contains no files");
    std::vector<const google::protobuf::Descriptor *> messages;
    for (const auto *file : descriptors->Files()) {
        if ((!roots.empty() && !roots.count(std::string(file->name()))) ||
            (cfg.require_db && !pbmysql::FileHasDBOption(file)))
            continue;
        for (int i = 0; i < file->message_type_count(); ++i)
            Collect(file->message_type(i), messages);
    }
    std::set<std::string> names;
    std::vector<Table> tables;
    for (const auto *message : messages) {
        pbmysql::MessageTable table(*descriptors->Prototype(message));
        if (!names.insert(pbmysql::FoldIdentifier(table.TableName())).second)
            throw std::runtime_error("multiple messages map to table: " + table.TableName());
        tables.push_back(
            {table.TableName(), (cfg.drop ? std::string(kDropBanner) + "DROP TABLE IF EXISTS " +
                                                pbmysql::QuoteIdentifier(table.TableName()) + ";\n"
                                          : "") +
                                    table.CreateTableSql()});
    }
    std::sort(tables.begin(), tables.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return tables;
}
void ValidateFileName(const std::string &name) {
    if (name.empty() || name == "." || name.rfind("..", 0) == 0 || name.back() == '.' ||
        name.back() == ' ')
        throw std::invalid_argument("unsafe output filename: " + name);
    for (unsigned char c : name)
        if (c < kFirstPrintableAscii || std::string("<>:\"/\\|?*").find(c) != std::string::npos)
            throw std::invalid_argument("unsafe output filename: " + name);
    auto base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const std::set<std::string> devices{"CON",    "PRN",     "AUX",  "NUL",  "CLOCK$",
                                        "CONIN$", "CONOUT$", "COM¹", "COM²", "COM³",
                                        "LPT¹",   "LPT²",    "LPT³"};
    const std::string_view device(base);
    constexpr auto number_position = kSerialDevicePrefix.size();
    if (devices.count(base) || (device.size() == number_position + 1 &&
                                (device.substr(0, number_position) == kSerialDevicePrefix ||
                                 device.substr(0, number_position) == kPrinterDevicePrefix) &&
                                device[number_position] >= '1' && device[number_position] <= '9'))
        throw std::invalid_argument("reserved output filename: " + name);
    (void)pbmysql::FoldIdentifier(name);
}
fs::path UniqueDirectory(const fs::path &parent) {
    std::random_device random;
    for (int i = 0; i < kTemporaryDirectoryAttempts; ++i) {
        auto candidate =
            parent / (".proto2sql-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (fs::create_directory(candidate))
            return candidate;
    }
    throw std::runtime_error("cannot allocate temporary output directory");
}
struct TemporaryDirectory {
    fs::path path;
    ~TemporaryDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove(path / "descriptors.pb", ignored);
            fs::remove(path, ignored);
        }
    }
};
fs::path ExecutableDirectory(const char *argument) {
#ifdef _WIN32
    std::vector<wchar_t> buffer(kInitialExecutablePathCapacity);
    while (buffer.size() <= kMaxWindowsPathCapacity) {
        const auto size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size)
            break;
        if (size < buffer.size())
            return fs::path(std::wstring(buffer.data(), size)).parent_path();
        buffer.resize(buffer.size() * kPathBufferGrowthFactor);
    }
#elif defined(__linux__)
    std::error_code error;
    const auto executable = fs::read_symlink("/proc/self/exe", error);
    if (!error)
        return executable.parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return fs::weakly_canonical(fs::u8path(buffer.data())).parent_path();
#endif
    std::error_code fallback_error;
    const auto fallback_executable = fs::canonical(fs::u8path(argument), fallback_error);
    return fallback_error ? fs::path{} : fallback_executable.parent_path();
}
#ifdef _WIN32
std::string Utf8(const wchar_t *text) {
    const std::wstring_view input(text);
    if (input.empty())
        return {};
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("UTF-16 process argument exceeds conversion limit");
    const auto length = static_cast<int>(input.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), length,
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        throw std::invalid_argument("invalid UTF-16 process argument");
    std::string value(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), length, value.data(),
                             size, nullptr, nullptr))
        throw std::invalid_argument("cannot convert process argument to UTF-8");
    return value;
}
std::wstring Wide(const std::string &text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("UTF-8 process argument exceeds conversion limit");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size == 0 && !text.empty())
        throw std::invalid_argument("invalid UTF-8 process argument");
    std::wstring value(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        value.data(), size);
    return value;
}
std::wstring QuoteArg(const std::wstring &text) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(slashes * (c == L'"' ? kWindowsBackslashEscapeMultiplier : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            quoted += L'\\';
        quoted += c;
    }
    quoted.append(slashes * kWindowsBackslashEscapeMultiplier, L'\\');
    return quoted + L'"';
}
#endif
void Run(const std::vector<std::string> &arguments) {
#ifdef _WIN32
    std::wstring command;
    for (const auto &argument : arguments) {
        if (!command.empty())
            command += L' ';
        command += QuoteArg(Wide(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process))
        throw std::runtime_error("cannot start protoc");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = EXIT_FAILURE;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code != 0)
        throw std::runtime_error("protoc failed with exit code " + std::to_string(code));
#else
    auto child = fork();
    if (child < 0)
        throw std::runtime_error("cannot fork protoc");
    if (child == 0) {
        std::vector<char *> argv;
        for (const auto &argument : arguments)
            argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(kExecFailureExitCode);
    }
    int status = 0;
    while (waitpid(child, &status, 0) == -1)
        if (errno != EINTR)
            throw std::runtime_error("cannot wait for protoc");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("protoc failed");
#endif
}
std::set<std::string> Compile(Config &cfg, const fs::path &descriptor_path) {
    std::vector<fs::path> sources, imports;
    std::set<std::string> roots;
    for (const auto &source : cfg.sources) {
        auto absolute = fs::canonical(fs::u8path(source));
        if (!fs::is_regular_file(absolute))
            throw std::invalid_argument("proto input is not a regular file");
        if (std::find(sources.begin(), sources.end(), absolute) != sources.end())
            continue;
        sources.push_back(absolute);
        imports.push_back(absolute.parent_path());
        if (!roots.insert(PathUtf8(absolute.filename())).second)
            throw std::invalid_argument("input proto files have colliding basenames");
    }
    for (const auto &directory : cfg.imports)
        imports.push_back(fs::canonical(fs::u8path(directory)));
    imports.push_back(fs::current_path());
    if (!cfg.executable_directory.empty()) {
        const auto bundled =
            cfg.executable_directory.parent_path() / "share" / "proto2mysql" / "proto";
        if (fs::is_directory(bundled))
            imports.push_back(bundled);
        const auto includes = cfg.executable_directory.parent_path() / "include";
        if (fs::is_regular_file(includes / "google" / "protobuf" / "descriptor.proto"))
            imports.push_back(includes);
    }
#ifdef PBMYSQL_BUNDLED_PROTO_DIR
    if (const auto directory = fs::u8path(PBMYSQL_BUNDLED_PROTO_DIR); fs::is_directory(directory))
        imports.push_back(directory);
#endif
#ifdef PBMYSQL_PROTOBUF_INCLUDE_DIR
    if (const auto directory = fs::u8path(PBMYSQL_PROTOBUF_INCLUDE_DIR);
        fs::is_directory(directory))
        imports.push_back(directory);
#endif
    std::vector<std::string> command{cfg.protoc, "--include_imports",
                                     "--descriptor_set_out=" + PathUtf8(descriptor_path)};
    std::set<fs::path> seen;
    for (const auto &directory : imports)
        if (seen.insert(directory).second)
            command.push_back("--proto_path=" + PathUtf8(directory));
    for (const auto &source : sources) {
        for (const auto &directory : imports) {
            const auto candidate = directory / source.filename();
            if (fs::exists(candidate)) {
                if (!fs::equivalent(candidate, source))
                    throw std::invalid_argument("proto input shadowed by another import path: " +
                                                PathUtf8(source));
                break;
            }
        }
        // Absolute paths also prevent a source basename beginning with '-' from
        // being interpreted as another compiler flag.
        command.push_back(PathUtf8(source));
    }
    Run(command);
    cfg.descriptors = PathUtf8(descriptor_path);
    return roots;
}
// Stage and close every file before replacing any target. Failures before the
// commit point restore old targets. Cleanup failures after commit are warnings.
void WriteFiles(const fs::path &directory,
                const std::vector<std::pair<std::string, std::string>> &files) {
    std::set<std::string> names;
    for (const auto &file : files) {
        ValidateFileName(file.first);
        if (!names.insert(pbmysql::FoldIdentifier(file.first)).second)
            throw std::invalid_argument("colliding output filenames");
    }
    if (files.empty())
        return;
    fs::create_directories(directory);
    const auto parent = fs::canonical(directory), stage = UniqueDirectory(parent);
    struct Item {
        fs::path target, fresh, backup;
        bool installed = false, saved = false;
    };
    std::vector<Item> items;
    try {
        for (size_t i = 0; i < files.size(); ++i) {
            items.push_back({parent / fs::u8path(files[i].first),
                             stage / ("new-" + std::to_string(i)),
                             stage / ("old-" + std::to_string(i))});
            auto &item = items.back();
            if (fs::exists(item.target) && !fs::is_regular_file(item.target))
                throw std::runtime_error("output target is not a regular file: " +
                                         PathUtf8(item.target));
            std::ofstream output(item.fresh, std::ios::binary);
            if (!output || !(output << files[i].second))
                throw std::runtime_error("cannot stage SQL output");
            output.close();
            if (!output)
                throw std::runtime_error("cannot close staged SQL output");
        }
        for (auto &item : items) {
            if (fs::exists(item.target)) {
                fs::rename(item.target, item.backup);
                item.saved = true;
            }
            fs::rename(item.fresh, item.target);
            item.installed = true;
        }
    } catch (...) {
        const auto failure = std::current_exception();
        std::string recovery;
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            std::error_code error;
            if (it->installed) {
                fs::remove(it->target, error);
                if (error)
                    recovery += " remove " + PathUtf8(it->target);
            }
            if (it->saved) {
                error.clear();
                fs::rename(it->backup, it->target, error);
                if (error)
                    recovery += " restore " + PathUtf8(it->target);
            }
        }
        for (const auto &item : items) {
            std::error_code error;
            fs::remove(item.fresh, error);
        }
        std::error_code error;
        fs::remove(stage, error);
        if (!recovery.empty())
            throw std::runtime_error("SQL export failed; manual recovery required:" + recovery +
                                     "; backups: " + PathUtf8(stage));
        std::rethrow_exception(failure);
    }
    for (const auto &item : items) {
        std::error_code error;
        if (item.saved)
            fs::remove(item.backup, error);
        if (error)
            std::cerr << "warning: SQL files committed; cannot remove backup " << item.backup
                      << '\n';
    }
    std::error_code error;
    fs::remove(stage, error);
    if (error)
        std::cerr << "warning: SQL files committed; temporary directory remains: " << stage << '\n';
}
std::string Combined(const std::vector<Table> &tables) {
    std::string output;
    for (const auto &table : tables)
        output += table.sql + "\n\n";
    return output;
}
void CSV(std::vector<std::string> &output, const std::string &value) {
    size_t start = 0;
    do {
        auto end = value.find(',', start);
        auto part = value.substr(start, end - start);
        auto first = part.find_first_not_of(" \t\r\n");
        if (first != std::string::npos)
            output.push_back(part.substr(first, part.find_last_not_of(" \t\r\n") - first + 1));
        if (end == std::string::npos)
            break;
        start = end + 1;
    } while (true);
}
Config Parse(int argc, char **argv) {
    Config cfg;
#ifdef _WIN32
    if (const wchar_t *executable = _wgetenv(L"PROTO2SQL_PROTOC"))
        cfg.protoc = Utf8(executable);
#else
    if (const char *executable = std::getenv("PROTO2SQL_PROTOC"))
        cfg.protoc = executable;
#endif
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i], value;
        const auto equal = arg.find('=');
        bool inline_value = equal != std::string::npos;
        if (inline_value) {
            value = arg.substr(equal + 1);
            arg.resize(equal);
        }
        if (arg == "-drop" || arg == "--drop" || arg == "--require-db") {
            bool flag = true;
            if (inline_value) {
                if (value == "1" || value == "t" || value == "T" || value == "true" ||
                    value == "TRUE" || value == "True")
                    flag = true;
                else if (value == "0" || value == "f" || value == "F" || value == "false" ||
                         value == "FALSE" || value == "False")
                    flag = false;
                else
                    throw std::invalid_argument("invalid boolean flag: " + arg);
            }
            if (arg == "--require-db")
                cfg.require_db = flag;
            else
                cfg.drop = flag;
            continue;
        }
        if (arg.rfind(kShortImportFlag.data(), 0) == 0 && arg.size() > kShortImportFlag.size() &&
            !inline_value) {
            value = arg.substr(kShortImportFlag.size());
            arg = kShortImportFlag;
            inline_value = true;
        }
        if (!inline_value) {
            if (i + 1 >= argc)
                throw std::invalid_argument("missing flag value: " + arg);
            value = argv[++i];
        }
        if (value.empty())
            throw std::invalid_argument("empty flag value: " + arg);
        if (arg == "-proto" || arg == "--proto" || arg == "-proto-file" || arg == "--proto-file")
            CSV(cfg.sources, value);
        else if (arg == "-I" || arg == "--import-path")
            cfg.imports.push_back(value);
        else if (arg == "--descriptor-set") {
            if (!cfg.descriptors.empty())
                throw std::invalid_argument("duplicate descriptor set");
            cfg.descriptors = value;
        } else if (arg == "--out" || arg == "-out") {
            cfg.out = value;
            cfg.output_directory = arg == "-out";
        } else if (arg == "--single" || arg == "-single") {
            ValidateFileName(value);
            cfg.single = value;
        } else if (arg == "--protoc")
            cfg.protoc = value;
        else
            throw std::invalid_argument("unknown flag: " + arg);
    }
    if ((cfg.sources.empty() && cfg.descriptors.empty()) ||
        (!cfg.sources.empty() && !cfg.descriptors.empty()))
        throw std::invalid_argument("provide either -proto source files or --descriptor-set");
    return cfg;
}
} // namespace
int RunCLI(int argc, char **argv) {
    try {
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
                std::cout
                    << "Usage: proto2sql -proto a.proto[,b.proto] [-proto-file c.proto] [-I "
                       "imports] [-out directory] [-single schema.sql] [-drop]\n"
                       "       proto2sql --descriptor-set descriptors.pb [--out schema.sql]\n"
                       "Source mode invokes protoc (override with --protoc or PROTO2SQL_PROTOC).\n"
                       "Descriptor mode retains stdout/single-file compatibility; -out selects a "
                       "directory.\n"
                       "--require-db filters tables to files declaring db=true.\n"
                       "WARNING: -drop deletes tables and all data; only use for empty/test "
                       "databases.\n";
                return 0;
            }
        auto cfg = Parse(argc, argv);
        cfg.executable_directory = ExecutableDirectory(argv[0]);
        TemporaryDirectory temporary;
        std::set<std::string> roots;
        if (!cfg.sources.empty()) {
            temporary.path = UniqueDirectory(fs::temp_directory_path());
            roots = Compile(cfg, temporary.path / "descriptors.pb");
        }
        const auto tables = Generate(cfg, roots);
        if (tables.empty() &&
            (!cfg.sources.empty() || cfg.output_directory || !cfg.single.empty())) {
            std::cerr << "warning: no annotated table messages found; no files generated\n";
            return 0;
        }
        if (!cfg.sources.empty() || cfg.output_directory || !cfg.single.empty()) {
            std::vector<std::pair<std::string, std::string>> files;
            if (!cfg.single.empty())
                files.push_back({cfg.single, Combined(tables)});
            else
                for (const auto &table : tables)
                    files.push_back({table.name + ".sql", table.sql + "\n"});
            WriteFiles(cfg.out.empty() ? fs::path(".") : fs::u8path(cfg.out), files);
            std::cout << "Generated " << tables.size() << " tables\n";
        } else if (cfg.out.empty()) {
            std::cout << Combined(tables);
            if (!std::cout)
                throw std::runtime_error("cannot write SQL to stdout");
        } else {
            auto output = fs::u8path(cfg.out);
            auto parent = output.parent_path();
            if (parent.empty())
                parent = ".";
            WriteFiles(parent, {{PathUtf8(output.filename()), Combined(tables)}});
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "proto2sql: " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wide_arguments) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i)
            arguments.push_back(Utf8(wide_arguments[i]));
        std::vector<char *> pointers;
        for (auto &argument : arguments)
            pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        return RunCLI(argc, pointers.data());
    } catch (const std::exception &error) {
        std::cerr << "proto2sql: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char **argv) { return RunCLI(argc, argv); }
#endif
