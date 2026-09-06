#define _CRT_SECURE_NO_WARNINGS

#include <cassert>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <string_view>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <array>
#include <cstdlib>
#include <cstdarg>
#include <utility>
#include <variant>
#include <thread>
#include <charconv>

namespace fs = std::filesystem;

void print_and_exit_impl(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::exit(1);
}

// print an error message and exit if the condition is true
#define print_and_exit_if(condition, ...)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (condition)                                                                                                 \
        {                                                                                                              \
            print_and_exit_impl(__VA_ARGS__);                                                                          \
        }                                                                                                              \
    } while (0)

class cfile
{
  public:
    cfile(const fs::path &path, const char *mode)
    {
        file_ = std::fopen(path.string().c_str(), mode);
        print_and_exit_if(!file_, "Failed to open file: %s\n", path.string().c_str());
    }
    cfile(const cfile &) = delete;
    cfile &operator=(const cfile &) = delete;
    ~cfile()
    {
        if (file_)
            std::fclose(file_);
    }
    // read the entire file content into a string
    std::string read()
    {
        print_and_exit_if(std::fseek(file_, 0, SEEK_END) != 0, "fseek failed\n");
        long size = std::ftell(file_);
        print_and_exit_if(size < 0, "ftell failed or empty file\n");
        std::rewind(file_);
        std::string content(size, '\0');
        std::size_t bytes_read = std::fread(&content[0], 1, size, file_);
        print_and_exit_if(bytes_read != static_cast<std::size_t>(size), "fread failed: expected %ld, got %zu\n", size,
                          bytes_read);
        return content;
    }
    void write(std::string_view data)
    {
        if (data.empty())
        {
            return;
        }
        std::size_t written = std::fwrite(data.data(), 1, data.size(), file_);
        print_and_exit_if(written != data.size(), "fwrite failed: expected %zu, wrote %zu\n", data.size(), written);
    }

  private:
    FILE *file_;
};

struct vfile
{
    enum class file_type
    {
        file,
        dir
    };
    enum class copy_mode
    {
        normal,
        create_symlink,
        normalize_text,   // normalize line endings to LF
        lowercase_include // lowercase #include directives in source files with normalize line endings to LF
    };
    struct file_info
    {
        fs::path source{};
        copy_mode mode{};
    };
    struct dir_info
    {
        std::vector<vfile> subfiles{};
    };
    file_type type() const
    {
        return std::holds_alternative<file_info>(info) ? file_type::file : file_type::dir;
    }
    file_info &file()
    {
        return std::get<file_info>(info);
    }
    dir_info &dir()
    {
        return std::get<dir_info>(info);
    }
    std::string name;
    std::variant<file_info, dir_info> info;
};

static vfile make_file(std::string &&name, vfile::file_info info = {})
{
    vfile v;
    v.name = std::move(name);
    v.info = std::move(info);
    return v;
}
static vfile make_dir(std::string &&name, vfile::dir_info info = {})
{
    vfile v;
    v.name = std::move(name);
    v.info = std::move(info);
    return v;
}

void to_lower_ascii_impl(char *begin, char *end)
{
    for (char *p = begin; p != end; ++p)
    {
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    }
}

std::string to_lower_ascii(std::string s)
{
    to_lower_ascii_impl(s.data(), s.data() + s.size());
    return s;
}

bool is_c_cxx_source(const fs::path &p)
{
    std::string ext = p.extension().string();
    using namespace std::string_view_literals;
    return ext.empty() || ext == ".h"sv || ext == ".c"sv || ext == ".hpp"sv || ext == ".cpp"sv || ext == ".inl"sv ||
           ext == ".ixx"sv;
}

std::string_view skip_space(std::string_view str)
{
    std::size_t i = 0;
    while (i != str.size() && (str[i] == ' ' || str[i] == '\t'))
    {
        ++i;
    }
    return str.substr(i);
}

std::string_view find_include_path(std::string_view line)
{
    std::string_view rem = skip_space(line);
    if (rem.empty() || rem[0] != '#')
    {
        return {};
    }
    rem.remove_prefix(1);
    rem = skip_space(rem);
    std::string_view kw = "include";
    if (rem.substr(0, kw.size()) != kw)
    {
        return {};
    }
    rem.remove_prefix(kw.size());
    rem = skip_space(rem);
    if (rem.empty() || (rem[0] != '<' && rem[0] != '"'))
    {
        // some MSVC headers uses #include _STL_INTRIN_HEADER
        std::string_view s = "_STL_INTRIN_HEADER";
        if (rem.substr(0, s.size()) == s)
        {
            return {};
        }
        print_and_exit_if(true, "Invalid #include directive: %.*s\n", static_cast<int>(line.size()), line.data());
    }
    char close = rem[0] == '<' ? '>' : '"';
    rem.remove_prefix(1);
    std::size_t close_pos = rem.find(close);
    print_and_exit_if(close_pos == std::string_view::npos, "Invalid #include directive: %.*s\n",
                      static_cast<int>(line.size()), line.data());
    return rem.substr(0, close_pos);
}

void lowercase_path_inplace(std::string &content, std::string_view path)
{
    if (path.empty())
        return;

    char *p = content.data() + (path.data() - content.data());
    char *end = p + path.size();
    to_lower_ascii_impl(p, end);
}

void copy(const fs::path &src, const fs::path &dst, vfile::copy_mode mode)
{
    assert(mode == vfile::copy_mode::normalize_text || mode == vfile::copy_mode::lowercase_include);

    cfile in(src, "rb");
    std::string content = in.read();
    std::string output;
    output.reserve(content.size());
    std::string_view remaining(content);
    while (!remaining.empty())
    {
        std::size_t end = remaining.find_first_of("\r\n");
        std::string_view line = remaining.substr(0, end);

        if (mode == vfile::copy_mode::lowercase_include)
        {
            lowercase_path_inplace(content, find_include_path(line));
        }

        // the line is modified in place
        output.append(line);
        output.push_back('\n');

        if (end == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(end);
        std::size_t skip = remaining.find_first_not_of("\r\n");
        if (skip == std::string_view::npos)
        {
            break;
        }
        remaining.remove_prefix(skip);
    }
    cfile out(dst, "wb");
    out.write(output);
}

bool is_stl_header(const fs::path &file_path)
{
    cfile file(file_path, "rb");
    std::string content = file.read();
    // all MSSTL headers have the following copyright block at the top of the file
    // and all other headers do not have it
    std::string_view copyright_block{"// Copyright (c) Microsoft Corporation.\r\n"
                                     "// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception"};
    return content.find(copyright_block) != std::string::npos;
}

void process_file(vfile &file_node, const fs::path &current_path)
{
    switch (file_node.file().mode)
    {
    case vfile::copy_mode::normal: {
        std::error_code ec;
        fs::copy_file(file_node.file().source, current_path, fs::copy_options::overwrite_existing, ec);
        print_and_exit_if(ec, "Failed to copy file from %s to %s: %s\n", file_node.file().source.string().c_str(),
                          current_path.string().c_str(), ec.message().c_str());
        break;
    }
    case vfile::copy_mode::create_symlink: {
        std::error_code ec;
        fs::create_symlink(file_node.file().source, current_path, ec);
        print_and_exit_if(ec, "Failed to create symlink from %s to %s: %s\n", file_node.file().source.string().c_str(),
                          current_path.string().c_str(), ec.message().c_str());
        break;
    }
    case vfile::copy_mode::normalize_text:
    case vfile::copy_mode::lowercase_include:
        copy(file_node.file().source, current_path, file_node.file().mode);
        break;
    }
}

vfile::file_type write_sysroot_common(vfile &node, const fs::path &current_path)
{
    if (node.type() != vfile::file_type::dir)
    {
        process_file(node, current_path);
        return node.type();
    }
    std::error_code ec;
    fs::create_directory(current_path, ec);
    print_and_exit_if(ec, "Failed to create directory %s: %s\n", current_path.string().c_str(), ec.message().c_str());
    return node.type();
}

void write_sysroot_seq(vfile &node, const fs::path &current_path)
{
    if (write_sysroot_common(node, current_path) != vfile::file_type::dir)
    {
        return;
    }
    for (auto &child : node.dir().subfiles)
    {
        write_sysroot_seq(child, current_path / child.name);
    }
}

struct jthread
{
    std::thread t;
    jthread() = default;
    jthread(std::thread &&other) noexcept
    {
        t = std::move(other);
    }
    jthread(jthread &&other) noexcept
    {
        t = std::move(other.t);
    }
    ~jthread()
    {
        if (t.joinable())
            t.join();
    }
};

void write_sysroot_par_impl(vfile &node, const fs::path &current_path, std::size_t threads_count,
                            std::vector<jthread> &threads)
{
    if (write_sysroot_common(node, current_path) != vfile::file_type::dir)
    {
        return;
    }

    auto &subfiles = node.dir().subfiles;
    vfile *data = subfiles.data();
    vfile *end = data + subfiles.size();

    while (data != end)
    {
        if (data->type() != vfile::file_type::file)
        {
            write_sysroot_par_impl(*data, current_path / data->name, threads_count, threads);
            ++data;
            continue;
        }

        vfile *seg_end = std::find_if_not(data, end, [](const vfile &v) { return v.type() == vfile::file_type::file; });
        std::size_t seg_len = seg_end - data;
        // limit the number of threads to avoid creating too many threads for small segments
        std::size_t num_threads = std::max(std::size_t(1), std::min(threads_count, seg_len / 8));

        auto process_one = [&current_path](vfile &v) { process_file(v, current_path / v.name); };

        if (num_threads == 1)
        {
            std::for_each(data, seg_end, process_one);
        }
        else
        {
            std::size_t base = seg_len / num_threads;
            std::size_t remainder = seg_len % num_threads;
            vfile *p = data;

            for (std::size_t i = 0; i != num_threads; ++i)
            {
                std::size_t block_size = base + (i < remainder ? 1 : 0);
                vfile *block_end = p + block_size;

                threads.emplace_back(
                    std::thread([p, block_end, &process_one]() { std::for_each(p, block_end, process_one); }));

                p = block_end;
            }

            threads.clear(); // wait for all threads
        }

        data = seg_end;
    }
}

void write_sysroot_par(vfile &node, const fs::path &current_path, std::size_t threads_count)
{
    std::vector<jthread> threads;
    threads.reserve(threads_count);
    write_sysroot_par_impl(node, current_path, threads_count, threads);
}

vfile &get_or_create_subdir(vfile &parent, std::string name)
{
    assert(parent.type() == vfile::file_type::dir);
    for (auto &child : parent.dir().subfiles)
    {
        if (child.type() == vfile::file_type::dir && child.name == name)
            return child;
    }
    vfile new_dir = make_dir(std::move(name));
    parent.dir().subfiles.push_back(std::move(new_dir));
    return parent.dir().subfiles.back();
}

vfile &get_or_create_subdir_path(vfile &root, const fs::path &path)
{
    vfile *current = &root;
    for (const auto &part : path)
    {
        std::string name = to_lower_ascii(part.filename().string());
        current = &get_or_create_subdir(*current, std::move(name));
    }
    return *current;
}

void add_file_to_tree(vfile &root, fs::path source_file, const fs::path &rel_path, vfile::copy_mode mode)
{
    std::string filename = to_lower_ascii(rel_path.filename().string());
    fs::path parent_path = rel_path.parent_path();
    vfile &parent = get_or_create_subdir_path(root, parent_path);

    vfile::file_info info{std::move(source_file), mode};
    vfile file_node = make_file(std::move(filename), std::move(info));
    parent.dir().subfiles.push_back(std::move(file_node));
}

void add_files(const fs::path &src_dir, vfile &root, vfile::copy_mode mode)
{
    // allow some directories to be missing, because libraries for certain architectures/variants are optional
    // in particular, ARM libraries have been removed in recent MSVC/WindowsSDK releases
    if (!fs::exists(src_dir))
    {
        std::fprintf(stdout, "Directory %s does not exist, skipping\n", src_dir.string().c_str());
        return;
    }
    for (const auto &entry : fs::recursive_directory_iterator(src_dir))
    {
        std::error_code ec;
        if (entry.is_regular_file(ec) && !ec)
        {
            fs::path path = entry.path();
            fs::path rel = path.lexically_relative(src_dir);
            add_file_to_tree(root, std::move(path), rel, mode);
        }
        print_and_exit_if(ec, "Error accessing file %s: %s\n", entry.path().string().c_str(), ec.message().c_str());
    }
}

struct add_include_files_args
{
    vfile &include_root;
    std::size_t cxx_pos;
    std::size_t msstl_pos;
    std::size_t intrin_pos;
};

void add_include_files(const fs::path &src_dir, add_include_files_args args, bool msvc_header, bool cppwinrt)
{
    for (const auto &entry : fs::recursive_directory_iterator(src_dir))
    {
        std::error_code ec;
        if (entry.is_regular_file(ec) && !ec)
        {
            auto path = entry.path();
            fs::path rel = path.lexically_relative(src_dir);
            std::string filename_lower = to_lower_ascii(rel.filename().string());

            // these files is unsupport for clang
            static constexpr std::array<std::string_view, 19> msvc_intrinsics = {
                "ammintrin.h", "arm64intr.h",   "arm64_neon.h", "arm_intr.h" /* no longer exists in newer versions */,
                "arm_neon.h",  "emmintrin.h",   "immintrin.h",  "intrin.h",
                "intrin0.h",   "intrin0.inl.h", "mm3dnow.h",    "mmintrin.h",
                "nmmintrin.h", "pmmintrin.h",   "smmintrin.h",  "tmmintrin.h",
                "wmmintrin.h", "xmmintrin.h",   "zmmintrin.h"};

            // determining intrinsic headers doesn't require opening files, so check that before STL
            bool is_intrin = msvc_header && std::find(msvc_intrinsics.begin(), msvc_intrinsics.end(), filename_lower) !=
                                                msvc_intrinsics.end();
            is_intrin = is_intrin || (!msvc_header && filename_lower == "softintrin.h"); // part of the Windows SDK

            // MSSTL headers exist only in the immediate (top-level) directory of the include path
            bool is_stl = !is_intrin && msvc_header && rel.parent_path().empty() &&
                          (!path.has_extension() || is_stl_header(path));

            if (is_stl)
            {
                auto &target_root = args.include_root.dir().subfiles[args.cxx_pos].dir().subfiles[args.msstl_pos];
                // MSSTL do not need to lowercase #include directives, but only normalize line endings to LF.
                add_file_to_tree(target_root, std::move(path), rel, vfile::copy_mode::normalize_text);
            }
            else if (is_intrin)
            {
                // intrins headers are conflicting with clang, so we put them in a separate directory
                auto &target_root = args.include_root.dir().subfiles[args.intrin_pos];
                add_file_to_tree(target_root, std::move(path), rel, vfile::copy_mode::normalize_text);
            }
            else
            {
                auto &target_root = args.include_root;
                // cppwinrt headers are known files that do not need to be processed.
                // They are typically 60 MiB or 80 MiB depending on the Windows SDK version
                auto mode = ((!cppwinrt) && is_c_cxx_source(path)) ? vfile::copy_mode::lowercase_include
                                                                   : vfile::copy_mode::normal;
                add_file_to_tree(target_root, std::move(path), rel, mode);
            }
        }
        print_and_exit_if(ec, "Error accessing file %s: %s\n", entry.path().string().c_str(), ec.message().c_str());
    }
}

struct add_architecture_libs_args
{
    vfile &lib_root;
    const fs::path &msvc;
    const fs::path &sdklib;
    vfile::copy_mode lib_mode;
};

void add_architecture_libs(add_architecture_libs_args args, std::string_view src_arch, std::string_view dest_arch)
{
    vfile arch_dir = make_dir(std::string(dest_arch) + "-unknown-windows-msvc");
    args.lib_root.dir().subfiles.push_back(std::move(arch_dir));
    vfile &arch_node = args.lib_root.dir().subfiles.back();

    add_files(args.msvc / "lib" / src_arch, arch_node, args.lib_mode);
    add_files(args.sdklib / "ucrt" / src_arch, arch_node, args.lib_mode);
    add_files(args.sdklib / "um" / src_arch, arch_node, args.lib_mode);

    vfile &onecore_node = get_or_create_subdir(arch_node, "onecore");
    add_files(args.msvc / "lib" / "onecore" / src_arch, onecore_node, args.lib_mode);

    vfile &enclave_node = get_or_create_subdir(arch_node, "enclave");
    add_files(args.sdklib / "ucrt_enclave" / src_arch, enclave_node, args.lib_mode);
}

vfile make_vfile_tree(const fs::path &out, const fs::path &sdkinc, const fs::path &sdklib, const fs::path &msvc,
                      vfile::copy_mode lib_mode)
{
    // root directory of the sysroot, identical to the output directory
    vfile root = make_dir(out.filename().string());

    // create the unix-style subdirectories
    vfile lib_dir = make_dir("lib");
    root.dir().subfiles.push_back(std::move(lib_dir));
    std::size_t lib_pos = 0;

    vfile include_dir = make_dir("include");
    root.dir().subfiles.push_back(std::move(include_dir));
    std::size_t include_pos = 1;

    vfile share_dir = make_dir("share");
    root.dir().subfiles.push_back(std::move(share_dir));
    std::size_t share_pos = 2;

    // C++ standard library headers, including MSSTL and libc++
    vfile cxx_dir = make_dir("c++");
    root.dir().subfiles[include_pos].dir().subfiles.push_back(std::move(cxx_dir));
    std::size_t cxx_pos = root.dir().subfiles[include_pos].dir().subfiles.size() - 1;

    // MSSTL headers, including implementation headers, which are not part of the public API
    vfile msstl_dir = make_dir("msvcstl");
    root.dir().subfiles[include_pos].dir().subfiles[cxx_pos].dir().subfiles.push_back(std::move(msstl_dir));
    std::size_t msstl_pos = root.dir().subfiles[include_pos].dir().subfiles[cxx_pos].dir().subfiles.size() - 1;

    // these intrinsics headers are confilcting with clang
    vfile intrin_dir = make_dir("__msvc_vcruntime_intrinsics");
    root.dir().subfiles[include_pos].dir().subfiles.push_back(std::move(intrin_dir));
    std::size_t intrin_pos = root.dir().subfiles[include_pos].dir().subfiles.size() - 1;

    add_architecture_libs_args arch_args{root.dir().subfiles[lib_pos], msvc, sdklib, lib_mode};

    add_architecture_libs(arch_args, "arm64", "aarch64");
    add_architecture_libs(arch_args, "arm", "arm");
    add_architecture_libs(arch_args, "x64", "x86_64");
    add_architecture_libs(arch_args, "x86", "i686");

    vfile &modules_node = get_or_create_subdir(root.dir().subfiles[share_pos], "msvcstl");
    add_files(msvc / "modules", modules_node, vfile::copy_mode::normal);

    add_include_files_args include_args{root.dir().subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos};

    add_include_files(msvc / "include", include_args, true, false);
    add_include_files(sdkinc / "ucrt", include_args, false, false);
    add_include_files(sdkinc / "um", include_args, false, false);
    add_include_files(sdkinc / "cppwinrt", include_args, false, true);
    add_include_files(sdkinc / "shared", include_args, false, false);
    add_include_files(sdkinc / "winrt", include_args, false, false);

    return root;
}

// check if the input paths are legal windows sdk and msvc paths
void validate_input(const fs::path &windows_sdk_inc, const fs::path &windows_sdk_lib, const fs::path &build_tool)
{
    std::error_code ec;
    print_and_exit_if((!fs::exists(windows_sdk_lib, ec) || !fs::exists(windows_sdk_lib / "ucrt", ec) ||
                       !fs::exists(windows_sdk_lib / "um", ec)),
                      "Invalid Windows SDK lib path: %s, expected subdirectories ucrt and um\n",
                      windows_sdk_lib.string().c_str());
    print_and_exit_if(
        (!fs::exists(windows_sdk_inc, ec) || !fs::exists(windows_sdk_inc / "ucrt", ec) ||
         !fs::exists(windows_sdk_inc / "um", ec) || !fs::exists(windows_sdk_inc / "cppwinrt", ec) ||
         !fs::exists(windows_sdk_inc / "shared", ec) || !fs::exists(windows_sdk_inc / "winrt", ec)),
        "Invalid Windows SDK include path: %s, expected subdirectories ucrt, um, cppwinrt, shared, and winrt\n",
        windows_sdk_inc.string().c_str());
    print_and_exit_if((!fs::exists(build_tool, ec) || !fs::exists(build_tool / "include", ec) ||
                       !fs::exists(build_tool / "lib", ec) || !fs::exists(build_tool / "modules", ec)),
                      "Invalid MSVC path: %s, expected subdirectories include, lib, and modules\n",
                      build_tool.string().c_str());
}

void print_usage_and_exit()
{
    std::fprintf(stderr, R"(
Usage: make-msvc-sysroot <windows-sdk-inc> <windows-sdk-lib> <build-tool> <output-path> [options]

Arguments:
  <windows-sdk-inc>   Path to the Include directory of Windows SDK.
  <windows-sdk-lib>   Path to the Lib directory of Windows SDK.
  <build-tool>        Path to the MSVC build tools.
  <output-path>       Output path. If exists, its contents will be removed.

Options:
  --symlink           Use symbolic links for library files.
  --threads <count>   Number of threads for processing and writing files.
)");
    std::exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 5)
    {
        print_usage_and_exit();
    }

    vfile::copy_mode symlink{};
    std::size_t threads_count = 1;

    for (auto i = 5; i != argc; ++i)
    {
        if (std::strcmp(argv[i], "--symlink") == 0)
        {
            symlink = vfile::copy_mode::create_symlink;
        }
        else if (std::strcmp(argv[i], "--threads") == 0)
        {
            print_and_exit_if(i + 1 == argc, "Missing argument for --threads\n");
            auto res = std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), threads_count);
            print_and_exit_if(res.ec != std::errc() || threads_count == 0, "Invalid argument for --threads: %s\n",
                              argv[i + 1]);
            ++i;
        }
        else
        {
            print_and_exit_if(true, "Unknown option: %s\n", argv[i]);
        }
    }

    std::error_code ec;
    auto sdkinc = fs::absolute(fs::path(argv[1]), ec);
    print_and_exit_if(ec, "Failed to get absolute path for %s: %s\n", argv[1], ec.message().c_str());
    auto sdklib = fs::absolute(fs::path(argv[2]), ec);
    print_and_exit_if(ec, "Failed to get absolute path for %s: %s\n", argv[2], ec.message().c_str());
    auto msvc = fs::absolute(fs::path(argv[3]), ec);
    print_and_exit_if(ec, "Failed to get absolute path for %s: %s\n", argv[3], ec.message().c_str());
    auto out = fs::absolute(fs::path(argv[4]), ec);
    print_and_exit_if(ec, "Failed to get absolute path for %s: %s\n", argv[4], ec.message().c_str());

    validate_input(sdkinc, sdklib, msvc);

    vfile root = make_vfile_tree(out, sdkinc, sdklib, msvc, symlink);

    fs::remove_all(out, ec);
    print_and_exit_if(ec, "Failed to remove existing output directory %s: %s\n", out.string().c_str(),
                      ec.message().c_str());
    if (threads_count == 1)
    {
        write_sysroot_seq(root, out);
    }
    else
    {
        write_sysroot_par(root, out, threads_count);
    }

    cfile readme(out / "README.md", "wb");
    readme.write("This directory was generated by make-msvc-sysroot.\n"
                 "Do not edit files manually. Any changes will be overwritten.\n");

    return 0;
}
