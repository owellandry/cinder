# Cinder

> A fast, lightweight JavaScript runtime and package manager written in pure C.

Cinder is built on [QuickJS](https://bellard.org/quickjs/) — a small, embeddable JS engine — and aims to be lighter than Node.js while matching Bun's speed for everyday tasks.

## Features

- ⚡ Fast startup (no JIT warmup)
- 📦 npm-compatible package manager
- 🧩 Built-in modules: `fs`, `path`, `process`
- 🔒 Lock file (`cinder.lock`) for reproducible installs
- 🌐 Cross-platform: Linux, macOS, Windows

## Building

### Prerequisites

- CMake ≥ 3.16
- C11 compiler (GCC, Clang, or MSVC)
- libcurl
- zlib

```bash
# Linux / macOS
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

```powershell
# Windows (MSVC + vcpkg)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Usage

```bash
# Run a JS file
cinder app.js

# Package manager
cinder init                    # create package.json
cinder add express             # install and save dependency
cinder add -D typescript       # install dev dependency
cinder install                 # install all from package.json
cinder remove express          # remove a package
cinder run build               # run a script from package.json
```

## Architecture

```
src/
├── main.c                  Entry point
├── cli/cli.c               Command-line dispatcher
├── runtime/
│   ├── runtime.c           QuickJS setup + module loader
│   └── modules/
│       ├── mod_fs.c        Node-compatible fs module
│       ├── mod_path.c      Node-compatible path module
│       └── mod_process.c   process global object
└── pm/
    ├── pm.c                Package manager orchestrator
    ├── registry.c          npm registry HTTP client
    ├── resolver.c          Semver range resolver
    ├── installer.c         .tgz download + extraction
    └── lockfile.c          cinder.lock read/write
```

## License

MIT
