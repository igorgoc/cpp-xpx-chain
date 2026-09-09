#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CHAIN_DIR="$( cd "${SCRIPT_DIR}/../.." && pwd )"
cd "${CHAIN_DIR}"

echo "=========================================================="
echo " Building ProximaX Sirius Core Native Engine for Linux (x86_64)"
echo "=========================================================="

BOOST_PREFIX="${BOOST_ROOT:-$HOME/boost-build-1.81.0}"
ROCKSDB_PREFIX="${ROCKSDB_ROOT:-$HOME/rocksdb-8.5.3}"

if [ ! -f "${BOOST_PREFIX}/include/boost/version.hpp" ]; then
    echo "-> Pinned Boost 1.81.0 not found at ${BOOST_PREFIX}, running builder..."
    "${SCRIPT_DIR}/build-boost-1.81.0.sh"
fi

if [ ! -f "${ROCKSDB_PREFIX}/include/rocksdb/db.h" ]; then
    echo "-> Pinned RocksDB 8.5.3 not found at ${ROCKSDB_PREFIX}, running builder..."
    "${SCRIPT_DIR}/build-rocksdb-8.5.3.sh"
fi

mkdir -p _build && cd _build

CCACHE_OPTS=""
if command -v ccache >/dev/null 2>&1; then
    echo "-> Enabling ccache acceleration for rapid builds..."
    CCACHE_OPTS="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache"
fi

echo "-> Running CMake configuration..."
cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBOOST_ROOT="${BOOST_PREFIX}" \
  -DROCKSDB_ROOT_DIR="${ROCKSDB_PREFIX}" \
  -DROCKSDB_LIBRARIES="${ROCKSDB_PREFIX}/lib/librocksdb.so" \
  -DROCKSDB_INCLUDE_DIR="${ROCKSDB_PREFIX}/include" \
  -DENABLE_MONGO=OFF \
  -DENABLE_ZEROMQ=OFF \
  -DXPX_STORAGE_SDK_NOT_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS="-pthread -include cstdint" \
  -DCMAKE_C_FLAGS="-pthread" \
  ${CCACHE_OPTS} \
  ..

echo "-> Generating headers (make publish)..."
make publish

NUM_CORES=$(nproc || echo 4)
echo "-> Compiling Sirius Core engine and plugins (${NUM_CORES} cores)..."
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
  -j"${NUM_CORES}"

echo "=========================================================="
echo " Compilation Complete! Binaries in _build/bin/"
echo "=========================================================="

cd "${CHAIN_DIR}"
echo "-> Bundling standalone relocatable Linux distribution..."
"${SCRIPT_DIR}/bundle-linux-deps.sh" "${CHAIN_DIR}/staging/bin"

NODE_MANAGER_BIN="${CHAIN_DIR}/../proximax-sirius-core/bin"
if [ -d "${NODE_MANAGER_BIN}" ]; then
    echo "-> Auto-updating changed binaries in Node Manager (${NODE_MANAGER_BIN})..."
    "${SCRIPT_DIR}/bundle-linux-deps.sh" "${NODE_MANAGER_BIN}"
    echo "-> Node Manager binaries synchronized successfully."
fi
