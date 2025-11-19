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
- **Choose one:**
  - [Clang/LLVM](https://llvm.org/) - **Recommended** (install via [Scoop](https://scoop.sh/): `scoop install llvm make`)
  - [MinGW](http://www.mingw.org/) or [TDM-GCC](https://jmeubank.github.io/tdm-gcc/)

## Build with Clang (Recommended)

**Using Git Bash:**
``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
make release          # Uses clang by default, creates rayforce.exe
./rayforce.exe -f examples/table.rfl
```

## Build with GCC/MinGW

**Using Git Bash:**
``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
make release CC=gcc   # Explicitly use GCC
./rayforce.exe -f examples/table.rfl
```

**Important Notes:**
- The Makefile automatically detects Clang with MSVC target and uses appropriate linker flags
- Build creates `rayforce.exe` automatically on Windows
- **Use Git Bash** for best compatibility (includes Unix tools like `rm`, `cp`)
- In cmd.exe, some make commands may fail without Git's bin directory in PATH

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
