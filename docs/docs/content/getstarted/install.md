# :material-package-variant-closed: Install

There are two ways to install RayforceDB:

- [Download a pre-built binary](#download-a-pre-built-binary)
- [Build from source](#building-from-source)

# :material-download: Download a pre-built binary

You can download the latest pre-built `rayforce` binary for your platform from the [release page](https://github.com/singaraiona/rayforce/releases) or directly here (linux version):

- [Download Linux binary](../../assets/rayforce_linux_x86_64)

For Linux, you can use the following commands to download and run the binary from this documentation (if you are viewing it locally or from your server):

```sh
curl -LO <YOUR_DOCS_URL>/assets/rayforce
chmod +x rayforce
./rayforce -f examples/table.rfl
```

Replace `<YOUR_DOCS_URL>` with the actual URL where your documentation is hosted, or use the relative link if browsing locally.

# :material-source-repository-multiple: Building from source

These OSes are supported (for now):

- [Linux](#linux)
- [Windows](#windows)
- [MacOS](#macos)

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

- [Git](https://git-scm.com/)
- [MinGW](http://www.mingw.org/)
- [TDM-GCC](https://jmeubank.github.io/tdm-gcc/)

``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
mingw32-make.exe CC="gcc" LIBS="-lm -lws2_32 -lkernel32" release 
./rayforce.exe -f examples/table.rfl
```

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
