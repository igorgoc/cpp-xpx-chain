# Building ProximaX Sirius C++ Chain on macOS (Apple Silicon ARM64)

This guide documents the exact, tested steps to compile the native **ProximaX Sirius Core C++ engine (`sirius.bc`)**, all **25+ consensus plugins**, and **extensions** natively on Apple Silicon (M1/M2/M3/M4) macOS without Docker.

---

## 1. Prerequisites & Toolchain Setup

### 1.1 Command Line Tools & Homebrew
Install Xcode Command Line Tools and Homebrew if not already present:
```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### 1.2 Toolchain & Dependencies via Homebrew
Install **LLVM 17** (provides modern `clang++`), **CMake**, **OpenSSL**, and **RocksDB**:
```bash
brew install llvm@17 cmake openssl@3 rocksdb
```

Verify the LLVM 17 compiler:
```bash
/opt/homebrew/opt/llvm@17/bin/clang++ --version
```

---

## 2. Build Boost 1.81.0 from Source (Apple Silicon ARM64)

Catapult requires Boost 1.81.0. Build it natively for `darwin-arm64`:

```bash
cd ~
curl -L -O https://boostorg.jfrog.io/artifactory/main/release/1.81.0/source/boost_1_81_0.tar.gz
tar -xzf boost_1_81_0.tar.gz
cd boost_1_81_0

./bootstrap.sh --prefix=$HOME/boost-build-1.81.0

# Compile and install Boost
./b2 --prefix=$HOME/boost-build-1.81.0 --without-python -j$(sysctl -n hw.ncpu) stage release
./b2 --prefix=$HOME/boost-build-1.81.0 --without-python install
```

Verify Boost installation:
```bash
ls -la $HOME/boost-build-1.81.0/include/boost/version.hpp
```

---

## 3. Clone Repository & Initialize Submodules

```bash
cd ~/Projects
git clone https://github.com/proximax-storage/cpp-xpx-chain.git
cd cpp-xpx-chain
git checkout master # or feature branch
git submodule update --init --recursive
```

---

## 4. CMake Configuration

Create the build directory and configure CMake with the dedicated LLVM 17 toolchain and custom Boost 1.81.0:

```bash
mkdir -p _build && cd _build

cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@17/bin/clang++ \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@17/bin/clang \
  -DBOOST_ROOT=$HOME/boost-build-1.81.0 \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DENABLE_MONGO=OFF \
  -DENABLE_ZEROMQ=OFF \
  -DXPX_STORAGE_SDK_NOT_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS="-pthread" \
  ..
```

---

## 5. Parallel Compilation

### 5.1 Generate headers & publish targets
```bash
make publish
```

### 5.2 Build Sirius Core Engine, Recovery Tool, Plugins & Extensions
```bash
make \
  sirius.bc \
  catapult.recovery \
  catapult.plugins.accountlink \
  catapult.plugins.aggregate \
  catapult.plugins.committee \
  catapult.plugins.config \
  catapult.plugins.contract \
  catapult.plugins.dbrb \
  catapult.plugins.exchange \
  catapult.plugins.exchangesda \
  catapult.plugins.hashcache \
  catapult.plugins.hashcache.cache \
  catapult.plugins.liquidityprovider \
  catapult.plugins.lockhash \
  catapult.plugins.locksecret \
  catapult.plugins.metadata \
  catapult.plugins.metadata_v2 \
  catapult.plugins.mosaic \
  catapult.plugins.multisig \
  catapult.plugins.namespace \
  catapult.plugins.operation \
  catapult.plugins.property \
  catapult.plugins.service \
  catapult.plugins.signature \
  catapult.plugins.storage \
  catapult.plugins.streaming \
  catapult.plugins.supercontract \
  catapult.plugins.transfer \
  catapult.plugins.upgrade \
  extension.addressextraction \
  extension.diagnostics \
  extension.eventsource \
  extension.fastfinality \
  extension.filespooling \
  extension.harvesting \
  extension.hashcache \
  extension.networkheight \
  extension.nodediscovery \
  extension.packetserver \
  extension.partialtransaction \
  extension.pluginhandlers \
  extension.storage \
  extension.sync \
  extension.syncsource \
  extension.timesync \
  extension.transactionsink \
  extension.unbondedpruning \
  torrent-sirius \
  api \
  connection \
  drive \
  ionet \
  model \
  net \
  nodediscovery \
  sdk_external \
  storage-crypto \
  utils \
  -j$(sysctl -n hw.ncpu)
```

The output binaries and `.dylib` libraries will be located in:
* `_build/bin/` (`sirius.bc`, `catapult.recovery`, `catapult.tools.*`)
* `_build/lib/` or `_build/bin/` (`libcatapult.*.dylib`, `libextension.*.dylib`, `libboost_*.dylib`)

---

## 6. Standalone Bundling & Relocatable dylib Packaging

To run `sirius.bc` standalone on any macOS machine without requiring Homebrew or system Boost installed:

1. Copy binaries and plugins to `proximax-sirius-core-native/bin/`:
```bash
cp -R _build/bin/* ../proximax-sirius-core-native/bin/
```

2. Run the relocatable dynamic library bundling script:
```bash
cd ../proximax-sirius-core-native
python3 scripts/bundle_dylib_deps.py
```

### What `bundle_dylib_deps.py` Automates:
* Discovers all transitive dylibs via `otool -L` (Boost, RocksDB, OpenSSL, snappy).
* Copies them into `bin/`.
* Rewrites install names to `@rpath/<dylib_name>` using `install_name_tool -id`.
* Re-links dependencies to `@rpath/<dep_name>`.
* Adds `@loader_path` to each binary's rpath list.
* Re-signs binaries with ad-hoc signatures (`codesign --force --sign -`).

---

## 7. Troubleshooting & Common Pitfalls

| Issue | Cause | Solution |
| :--- | :--- | :--- |
| **`std::this_thread::sleep_for(1ns)` CPU spike (~300%)** | Disruptor consumer busy-wait in `ConsumerDispatcher.cpp`. | Patch `ConsumerDispatcher.cpp` with condition variable wait (`m_cv.wait_for(...)`). |
| **Boost Linker Mismatch** | Homebrew Boost installed version differs from Catapult ABI (e.g. 1.84+ vs 1.81.0). | Build Boost 1.81.0 from source and pass `-DBOOST_ROOT=$HOME/boost-build-1.81.0`. |
| **Missing RocksDB Symbols** | Incompatible RocksDB version. | Use RocksDB `v8.5.3` / `v8.x` built with `-DWITH_SNAPPY=1`. |
| **`dyld: Library not loaded: @rpath/...`** | Hardcoded Homebrew paths missing on target machine. | Run `scripts/bundle_dylib_deps.py` to fix all dylib `@rpath` and `@loader_path` entries. |
