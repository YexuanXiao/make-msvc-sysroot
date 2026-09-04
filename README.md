# Unix-style Windows Sysroot Generator for Clang

A Unix-style Windows sysroot generator for Clang. Written in standard C++17.

## Related Discussions

<https://github.com/llvm/llvm-project/pull/96417>

<https://discourse.llvm.org/t/rfc-support-sysroot-for-arch-windows-msvc-targets/91650>

## Usage

```
make-msvc-sysroot <windows-sdk-inc> <windows-sdk-lib> <build-tool> <output-path> [options]
```

## Arguments

| Argument | Description |
|----------|-------------|
| `<windows-sdk-inc>` | Path to the Include directory of Windows SDK. |
| `<windows-sdk-lib>` | Path to the Lib directory of Windows SDK. |
| `<build-tool>`      | Path to the MSVC build tools. |
| `<output-path>`     | Output path. If exists, its contents will be removed. |

## Options

| Option | Description |
|--------|-------------|
| `--symlink` | Use symbolic links for library files. |
