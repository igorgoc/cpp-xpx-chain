#!/usr/bin/env bash
set -euo pipefail

ROCKSDB_VERSION="8.5.3"
ROCKSDB_INSTALL_DIR="${ROCKSDB_ROOT:-$HOME/rocksdb-${ROCKSDB_VERSION}}"
BUILD_TMP_DIR="/tmp/rocksdb_build_${ROCKSDB_VERSION}"
CORES=$(nproc || echo 4)

echo "=========================================================="
echo " Building Pinned RocksDB ${ROCKSDB_VERSION} (with RTTI)"
echo " Install Destination: ${ROCKSDB_INSTALL_DIR}"
echo " Cores: ${CORES}"
echo "=========================================================="

if [ -f "${ROCKSDB_INSTALL_DIR}/include/rocksdb/db.h" ] && [ -f "${ROCKSDB_INSTALL_DIR}/lib/librocksdb.so" ]; then
    echo "✓ RocksDB ${ROCKSDB_VERSION} already installed at ${ROCKSDB_INSTALL_DIR}"
    exit 0
fi

rm -rf "${BUILD_TMP_DIR}"
mkdir -p "${BUILD_TMP_DIR}"
cd "${BUILD_TMP_DIR}"

echo "-> Cloning RocksDB v${ROCKSDB_VERSION}..."
git clone --depth 1 --branch "v${ROCKSDB_VERSION}" https://github.com/facebook/rocksdb.git rocksdb-src
cd rocksdb-src

echo "-> Configuring CMake for RocksDB (USE_RTTI=1, WITH_TESTS=OFF, WITH_SNAPPY=1, WITH_ZSTD=1)..."
mkdir -p _build && cd _build
cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${ROCKSDB_INSTALL_DIR}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DUSE_RTTI=1 \
    -DROCKSDB_BUILD_SHARED=ON \
    -DWITH_TESTS=OFF \
    -DWITH_BENCHMARK_TOOLS=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_SNAPPY=1 \
    -DWITH_ZSTD=1 \
    -DWITH_GFLAGS=OFF \
    ..

echo "-> Compiling RocksDB (${CORES} cores)..."
cmake --build . -j"${CORES}"

echo "-> Installing RocksDB..."
cmake --install .

echo "-> Cleaning temporary build files..."
rm -rf "${BUILD_TMP_DIR}"

if [ -f "${ROCKSDB_INSTALL_DIR}/include/rocksdb/db.h" ]; then
    echo "=========================================================="
    echo "✓ RocksDB ${ROCKSDB_VERSION} successfully installed to ${ROCKSDB_INSTALL_DIR}"
    echo "=========================================================="
else
    echo "::error:: RocksDB installation failed!"
    exit 1
fi
