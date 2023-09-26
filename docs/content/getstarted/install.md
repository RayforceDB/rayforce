# :material-package-variant-closed: Install

There are two ways to install RayforceDB:

- [Download a pre-built binary](#download-a-pre-built-binary)
- [Build from source](#building-from-source)

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
rlwrap ./rayforce -f tests/table.rfl
```

# :material-microsoft-windows: Windows

## Requirements

- [Git](https://git-scm.com/)
- [MinGW](http://www.mingw.org/)
- [TDM-GCC](https://jmeubank.github.io/tdm-gcc/)

``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
mingw32-make.exe CC="Path/To/TDM/GCC/bin/gcc.exe" release 
./rayforce.exe -f tests/table.rfl
```

# :simple-macos: MacOS

- [Git](https://git-scm.com/)
- [GNU Make](https://www.gnu.org/software/make/)
- [GCC](https://gcc.gnu.org/) or [Clang](https://clang.llvm.org/)

``` sh
git clone https://github.com/singaraiona/rayforce
cd rayforce
make release
rlwrap ./rayforce -f tests/table.rfl
```
