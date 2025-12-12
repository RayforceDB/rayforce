# :material-package-variant-closed: Install

Distribution of RayforceDB is currently happening via native installation from <a href="https://github.com/RayforceDB/rayforce">source</a>.

| Platform                             | Is supported?    | 
| ------------------------------------ | ---------------- | 
| :simple-linux: Linux x86_64          | :material-check: | 
| :simple-apple: MacOS arm64           | :material-check: | 
| :material-microsoft-windows: Windows | :material-close: `coming soon` |


!!! note ""

    For :simple-linux: Linux installation, you will need to install <a href="https://gcc.gnu.org/">GCC</a>.
    
    For :simple-apple: MacOS installation, you will need to install <a href="https://clang.llvm.org/">Clang</a>

### Installation process

1. Clone the library from https://github.com/RayforceDB/rayforce
```bash
~ $ git clone https://github.com/RayforceDB/rayforce && cd rayforce
```
2. Compile the latest version
```bash
~ $ make release
```
3. Make your first query
```clj
~ $ ./rayforce
↪ (sum [2 3 4 5 6 7 8 9])
44
```