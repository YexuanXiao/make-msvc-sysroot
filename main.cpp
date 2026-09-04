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
    file_type type{};
    copy_mode mode{};
    std::string name{};
    fs::path source{};
    std::vector<vfile> subfiles{};
};

std::string to_lower_ascii(std::string s)
{
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
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
    if (close_pos == std::string_view::npos)
    {
        print_and_exit_if(true, "Invalid #include directive: %.*s\n", static_cast<int>(line.size()), line.data());
    }
    return rem.substr(0, close_pos);
}

void lowercase_path_inplace(std::string &content, std::string_view path)
{
    if (path.empty())
        return;

    char *p = content.data() + (path.data() - content.data());
    char *end = p + path.size();
    for (; p != end; ++p)
    {
        if (*p >= 'A' && *p <= 'Z')
            *p += 'a' - 'A';
    }
}

void copy(const fs::path &src, const fs::path &dst, vfile::copy_mode mode)
{
    assert(mode == vfile::copy_mode::normalize_text || mode == vfile::copy_mode::lowercase_include);

    cfile in(src, "rb");
    std::string content = in.read();
    std::string_view remaining(content);

    cfile out(dst, "wb");

    while (!remaining.empty())
    {
        std::size_t end = remaining.find_first_of("\r\n");
        std::string_view line = remaining.substr(0, end);

        if (mode == vfile::copy_mode::lowercase_include)
        {
            lowercase_path_inplace(content, find_include_path(line));
        }

        // the line is modified in place
        out.write(line);
        out.write("\n");

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
}

bool is_stl_header(const fs::path &file_path)
{
    if (!file_path.has_extension())
        return true;
    cfile file(file_path, "rb");
    std::string content = file.read();
    // all MSSTL headers have the following copyright block at the top of the file
    // and all other headers do not have it
    std::string_view copyright_block{"// Copyright (c) Microsoft Corporation.\r\n"
                                     "// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception"};
    return content.find(copyright_block) != std::string::npos;
}

void write_sysroot(const vfile &node, const fs::path &current_path)
{
    std::error_code ec;
    if (node.type == vfile::file_type::dir)
    {
        fs::create_directory(current_path, ec);
        print_and_exit_if(ec, "Failed to create directory %s: %s\n", current_path.string().c_str(),
                          ec.message().c_str());
        for (const auto &child : node.subfiles)
        {
            write_sysroot(child, current_path / child.name);
        }
        return;
    }
    switch (node.mode)
    {
    case vfile::copy_mode::normal:
        fs::copy_file(node.source, current_path, fs::copy_options::overwrite_existing, ec);
        print_and_exit_if(ec, "Failed to copy file from %s to %s: %s\n", node.source.string().c_str(),
                          current_path.string().c_str(), ec.message().c_str());
        break;
    case vfile::copy_mode::create_symlink:
        fs::create_symlink(fs::absolute(node.source), current_path, ec);
        print_and_exit_if(ec, "Failed to create symlink from %s to %s: %s\n", node.source.string().c_str(),
                          current_path.string().c_str(), ec.message().c_str());
        break;
    case vfile::copy_mode::normalize_text:
        copy(node.source, current_path, node.mode);
        break;
    case vfile::copy_mode::lowercase_include:
        copy(node.source, current_path, node.mode);
        break;
    }
}

vfile &get_or_create_subdir(vfile &parent, std::string_view name)
{
    assert(parent.type == vfile::file_type::dir);
    for (auto &child : parent.subfiles)
    {
        if (child.type == vfile::file_type::dir && child.name == name)
            return child;
    }
    vfile new_dir;
    new_dir.type = vfile::file_type::dir;
    new_dir.name = name;
    new_dir.mode = vfile::copy_mode::normal;
    parent.subfiles.push_back(std::move(new_dir));
    return parent.subfiles.back();
}

vfile &get_or_create_subdir_path(vfile &root, const std::vector<std::string> &parts)
{
    vfile *current = &root;
    for (const auto &part : parts)
    {
        current = &get_or_create_subdir(*current, part);
    }
    return *current;
}

void add_file_to_tree(vfile &root, fs::path source_file, std::vector<std::string> path, vfile::copy_mode mode)
{
    vfile file_node;
    file_node.type = vfile::file_type::file;
    file_node.name = std::move(path.back());
    path.pop_back();
    file_node.source = std::move(source_file);
    file_node.mode = mode;
    vfile &parent = get_or_create_subdir_path(root, path);
    parent.subfiles.push_back(std::move(file_node));
}

void add_file_to_tree(vfile &root, fs::path source_file, const fs::path &src_dir, vfile::copy_mode mode)
{
    fs::path rel = source_file.lexically_relative(src_dir);
    std::vector<std::string> parts;
    for (const auto &p : rel)
    {
        parts.push_back(to_lower_ascii(p.filename().string()));
    }
    add_file_to_tree(root, std::move(source_file), std::move(parts), mode);
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
            add_file_to_tree(root, entry.path(), src_dir, mode);
        }
        print_and_exit_if(ec, "Error accessing file %s: %s\n", entry.path().string().c_str(), ec.message().c_str());
    }
}

void add_include_files(const fs::path &src_dir, vfile &include_root, std::size_t cxx_pos, std::size_t msstl_pos,
                       std::size_t intrin_pos, bool msvc_header, bool cppwinrt)
{
    for (const auto &entry : fs::recursive_directory_iterator(src_dir))
    {
        std::error_code ec;
        if (entry.is_regular_file(ec) && !ec)
        {
            auto path = entry.path();
            fs::path rel = path.lexically_relative(src_dir);
            // split the relative path into parts and lowercase them
            std::vector<std::string> parts;
            for (const auto &p : rel)
            {
                parts.push_back(to_lower_ascii(p.filename().string()));
            }

            // these files is unsupport for clang
            static constexpr std::array<std::string_view, 19> msvc_intrinsics = {
                "ammintrin.h", "arm64intr.h",   "arm64_neon.h", "arm_intr.h" /* no longer exists in newer versions */,
                "arm_neon.h",  "emmintrin.h",   "immintrin.h",  "intrin.h",
                "intrin0.h",   "intrin0.inl.h", "mm3dnow.h",    "mmintrin.h",
                "nmmintrin.h", "pmmintrin.h",   "smmintrin.h",  "tmmintrin.h",
                "wmmintrin.h", "xmmintrin.h",   "zmmintrin.h"};

            auto &filename = parts.back();

            // determining intrinsic headers doesn't require opening files, so check that before STL
            bool is_intrin = msvc_header && std::find(msvc_intrinsics.begin(), msvc_intrinsics.end(), filename) !=
                                                msvc_intrinsics.end();
            is_intrin = is_intrin || (!msvc_header && filename == "softintrin.h"); // part of the Windows SDK

            // MSSTL headers exist only in the immediate (top-level) directory of the include path
            bool is_stl = !is_intrin && msvc_header && rel.parent_path().empty() && is_stl_header(path);

            if (is_stl)
            {
                auto &target_root = include_root.subfiles[cxx_pos].subfiles[msstl_pos];
                // MSSTL do not need to lowercase #include directives, but only normalize line endings to LF.
                add_file_to_tree(target_root, std::move(path), std::move(parts), vfile::copy_mode::normalize_text);
            }
            else if (is_intrin)
            {
                // intrins headers are conflicting with clang, so we put them in a separate directory
                auto &target_root = include_root.subfiles[intrin_pos];
                add_file_to_tree(target_root, std::move(path), std::move(parts), vfile::copy_mode::normalize_text);
            }
            else
            {
                auto &target_root = include_root;
                // cppwinrt headers are known files that do not need to be processed.
                // They are typically 60 MiB or 80 MiB depending on the Windows SDK version
                auto need_normalize = (!cppwinrt) && is_c_cxx_source(path);
                add_file_to_tree(target_root, std::move(path), std::move(parts),
                                 need_normalize ? vfile::copy_mode::lowercase_include : vfile::copy_mode::normal);
            }
        }
        print_and_exit_if(ec, "Error accessing file %s: %s\n", entry.path().string().c_str(), ec.message().c_str());
    }
}

// check if the input paths are legal windows sdk and msvc paths
void validate_input(const fs::path &windows_sdk_inc, const fs::path &windows_sdk_lib, const fs::path &build_tool)
{
    std::error_code ec;
    if (!fs::exists(windows_sdk_lib, ec) || !fs::exists(windows_sdk_lib / "ucrt", ec) ||
        !fs::exists(windows_sdk_lib / "um", ec))
        print_and_exit_if(true, "Invalid Windows SDK lib path: %s, expected subdirectories ucrt and um\n",
                          windows_sdk_lib.string().c_str());
    if (!fs::exists(windows_sdk_inc, ec) || !fs::exists(windows_sdk_inc / "ucrt", ec) ||
        !fs::exists(windows_sdk_inc / "um", ec) || !fs::exists(windows_sdk_inc / "cppwinrt", ec) ||
        !fs::exists(windows_sdk_inc / "shared", ec) || !fs::exists(windows_sdk_inc / "winrt", ec))
        print_and_exit_if(
            true,
            "Invalid Windows SDK include path: %s, expected subdirectories ucrt, um, cppwinrt, shared, and winrt\n",
            windows_sdk_inc.string().c_str());
    if (!fs::exists(build_tool, ec) || !fs::exists(build_tool / "include", ec) || !fs::exists(build_tool / "lib", ec) ||
        !fs::exists(build_tool / "modules", ec))
        print_and_exit_if(true, "Invalid MSVC path: %s, expected subdirectories include, lib, and modules\n",
                          build_tool.string().c_str());
}

void add_architecture_libs(vfile &lib_ref, const fs::path &msvc, const fs::path &sdklib, vfile::copy_mode lib_mode,
                           std::string_view src_arch, std::string_view dest_arch)
{
    vfile arch_dir;
    arch_dir.type = vfile::file_type::dir;
    std::string arch_dir_name{dest_arch};
    // arch-unknown-windows-msvc
    arch_dir_name += "-unknown-windows-msvc";
    arch_dir.name = std::move(arch_dir_name);
    lib_ref.subfiles.push_back(std::move(arch_dir));
    vfile &arch_node = lib_ref.subfiles.back();

    add_files(msvc / "lib" / src_arch, arch_node, lib_mode);
    add_files(sdklib / "ucrt" / src_arch, arch_node, lib_mode);
    add_files(sdklib / "um" / src_arch, arch_node, lib_mode);

    vfile &onecore_node = get_or_create_subdir(arch_node, "onecore");
    add_files(msvc / "lib" / "onecore" / src_arch, onecore_node, lib_mode);

    vfile &enclave_node = get_or_create_subdir(arch_node, "enclave");
    add_files(sdklib / "ucrt_enclave" / src_arch, enclave_node, lib_mode);
}

vfile make_vfile_tree(const fs::path &out, const fs::path &sdkinc, const fs::path &sdklib, const fs::path &msvc,
                      vfile::copy_mode lib_mode)
{
    // root directory of the sysroot, identical to the output directory
    vfile root;
    root.type = vfile::file_type::dir;
    root.name = out.filename().string();

    // create the unix-style subdirectories
    vfile lib_dir;
    lib_dir.type = vfile::file_type::dir;
    lib_dir.name = "lib";
    root.subfiles.push_back(lib_dir);
    std::size_t lib_pos = 0;

    vfile include_dir;
    include_dir.type = vfile::file_type::dir;
    include_dir.name = "include";
    root.subfiles.push_back(include_dir);
    std::size_t include_pos = 1;

    vfile share_dir;
    share_dir.type = vfile::file_type::dir;
    share_dir.name = "share";
    root.subfiles.push_back(share_dir);
    std::size_t share_pos = 2;

    // C++ standard library headers, including MSSTL and libc++
    vfile cxx_dir;
    cxx_dir.type = vfile::file_type::dir;
    cxx_dir.name = "c++";
    cxx_dir.mode = vfile::copy_mode::normal;
    root.subfiles[include_pos].subfiles.push_back(cxx_dir);
    std::size_t cxx_pos = root.subfiles[include_pos].subfiles.size() - 1;

    // MSSTL headers, including implementation headers, which are not part of the public API
    vfile msstl_dir;
    msstl_dir.type = vfile::file_type::dir;
    msstl_dir.name = "msvcstl";
    msstl_dir.mode = vfile::copy_mode::normal;
    root.subfiles[include_pos].subfiles[cxx_pos].subfiles.push_back(msstl_dir);
    std::size_t msstl_pos = root.subfiles[include_pos].subfiles[cxx_pos].subfiles.size() - 1;

    // these intrinsics headers are confilcting with clang
    vfile intrin_dir;
    intrin_dir.type = vfile::file_type::dir;
    intrin_dir.name = "__msvc_vcruntime_intrinsics";
    intrin_dir.mode = vfile::copy_mode::normal;
    root.subfiles[include_pos].subfiles.push_back(intrin_dir);
    std::size_t intrin_pos = root.subfiles[include_pos].subfiles.size() - 1;

    add_architecture_libs(root.subfiles[lib_pos], msvc, sdklib, lib_mode, "arm64", "aarch64");
    add_architecture_libs(root.subfiles[lib_pos], msvc, sdklib, lib_mode, "arm", "arm");
    add_architecture_libs(root.subfiles[lib_pos], msvc, sdklib, lib_mode, "x64", "x86_64");
    add_architecture_libs(root.subfiles[lib_pos], msvc, sdklib, lib_mode, "x86", "i686");

    vfile &modules_node = get_or_create_subdir(root.subfiles[share_pos], "msvcstl");
    add_files(msvc / "modules", modules_node, vfile::copy_mode::normalize_text);

    add_include_files(msvc / "include", root.subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos, true, false);
    add_include_files(sdkinc / "ucrt", root.subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos, false, false);
    add_include_files(sdkinc / "um", root.subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos, false, false);
    add_include_files(sdkinc / "cppwinrt", root.subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos, false, true);
    add_include_files(sdkinc / "shared", root.subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos, false, false);
    add_include_files(sdkinc / "winrt", root.subfiles[include_pos], cxx_pos, msstl_pos, intrin_pos, false, false);

    return root;
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
)");
    std::exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 5 || argc > 6)
    {
        print_usage_and_exit();
    }

    vfile::copy_mode symlink{};
    if (argc == 6)
    {
        if (std::strcmp(argv[5], "--symlink") != 0)
        {
            print_usage_and_exit();
        }
        symlink = vfile::copy_mode::create_symlink;
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
    write_sysroot(root, out);

    cfile readme(out / "README.md", "wb");
    readme.write("This directory was generated by make-msvc-sysroot.\n"
                 "Do not edit files manually. Any changes will be overwritten.\n");

    return 0;
}
