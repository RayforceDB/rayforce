# RayforceDB

![RayforceDB Cover](docs/assets/images/cover.png)

[![Tests](https://img.shields.io/badge/Tests-passing-success?logo=github&style=flat)](https://singaraiona.github.io/rayforce/tests_report/) [![Coverage](https://img.shields.io/badge/Coverage-passing-brightgreen?style=flat&logo=github)](https://singaraiona.github.io/rayforce/coverage_report/) [![Release](https://img.shields.io/badge/Release-latest-blue?logo=github&style=flat)](https://github.com/singaraiona/rayforce/releases) [![Documentation](https://img.shields.io/badge/Documentation-latest-blue?logo=github&style=flat)](https://singaraiona.github.io/rayforce/)

A high-performance columnar vector database written in pure C. RayforceDB combines the power of columnar storage with SIMD vectorization to deliver fast analytics on time-series and big data workloads.

## Features

- **Columnar storage** with vectorized operations for analytical workloads
- **Minimal footprint**: <1Mb binary, zero dependencies
- **Cross-platform**: Linux, macOS, Windows, WebAssembly
- **Simple query language**: Lisp-like Rayfall syntax, no complex SQL
- **Custom memory management**: Parallel lockfree buddy allocator optimized for analytical workloads

## Quick Start

```bash
git clone https://github.com/singaraiona/rayforce.git
cd rayforce
make release
./rayforce
```

## Use Cases

- Financial analytics and high-frequency trading data
- IoT sensor data and time-series monitoring
- Real-time analytics and streaming data
- Embedded systems and edge computing
- Data science and exploratory analysis
- LLMs and semantic retrieval

## Building

```bash
make debug      # Debug build with sanitizers
make release    # Optimized production build
make tests      # Run test suite
make bench      # Run benchmark suite
```

## Documentation

- [Full Documentation](https://singaraiona.github.io/rayforce/)
- [Test Reports](https://singaraiona.github.io/rayforce/tests_report/)
- [Coverage Reports](https://singaraiona.github.io/rayforce/coverage_report/)

## Contributing

Contributions are welcome! You can help by:

- Reporting bugs and requesting features via [GitHub Issues](https://github.com/singaraiona/rayforce/issues)
- Submitting pull requests
- Creating example scripts and use cases
- Improving documentation

## Development Partnership

RayforceDB is jointly developed with and sponsored by **[Lynx Capital](https://www.lynxcapitalsecurities.com/)**.

This partnership has been instrumental in making RayforceDB a mature, production-ready database system. Lynx Capital's active involvement in development and their commitment to innovative open-source technologies in the financial sector has enabled RayforceDB to reach its full potential.
