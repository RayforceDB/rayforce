# :material-package-variant-closed: Install

There are two ways to install RayforceDB:

- [Download a pre-built binary](#download-a-pre-built-binary)
- [Build from source](#building-from-source)

# :material-download: Download a pre-built binary

You can download the latest pre-built `rayforce` binary for your platform from the [release page](https://github.com/singaraiona/rayforce/releases) or directly here:

- [Download Linux binary](../../assets/rayforce_linux_x86_64)
- [Download MacOS binary](../../assets/rayforce_mac_arm64)

# :material-source-repository-multiple: Building from source

These OSes are supported (for now):

- [Linux](#linux)
- [MacOS](#macos)
- [Windows](#windows)

# :material-linux: Linux

## Requirements

- [Git](https://git-scm.com/)
- [GNU Make](https://www.gnu.org/software/make/)
- [GCC](https://gcc.gnu.org/) or [Clang](https://clang.llvm.org/)

``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
make release
rlwrap ./rayforce -f examples/table.rfl
```

# :material-microsoft-windows: Windows

## Requirements

- [Git](https://git-scm.com/) (includes Git Bash)
- [MSYS2](https://www.msys2.org/) - Provides MinGW-w64 toolchain and make
- **Choose one:**
  - [GCC](https://gcc.gnu.org/) (default, included with MSYS2)
  - [Clang](https://clang.llvm.org/) (recommended to install via MSYS2: `pacman -S mingw-w64-clang-x86_64-clang`)

## Build with GCC (Default)

**Using Git Bash or MSYS2:**
``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
mingw32-make release          # Uses GCC by default, creates rayforce.exe
./rayforce.exe -f examples/table.rfl
```

## Build with Clang (Alternative)

**Using Git Bash or MSYS2:**
``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
mingw32-make release CC=clang # Uses Clang with auto-detected lld and compiler-rt
./rayforce.exe -f examples/table.rfl
```

**Important Notes:**
- Use `mingw32-make` instead of `make` on Windows
- The Makefile automatically detects the compiler and configures appropriate linker flags
- For Clang builds, lld linker and compiler-rt are auto-detected
- Build creates `rayforce.exe` automatically on Windows
- **Recommended:** Install Clang via MSYS2 for better MinGW compatibility

# :simple-macos: MacOS

- [Git](https://git-scm.com/)
- [GNU Make](https://www.gnu.org/software/make/)
- [GCC](https://gcc.gnu.org/) or [Clang](https://clang.llvm.org/)

``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
make release
rlwrap ./rayforce -f examples/table.rfl
```

# :material-checkbox-multiple-blank: Tests

Tests are under tests/ directory. To run tests:

``` sh
make clean && make tests
```
