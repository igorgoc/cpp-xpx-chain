#!/bin/bash
set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )"
cd "$DIR"

echo "=========================================================="
echo " Building ProximaX Sirius Core Native Engine for macOS ARM64"
echo "=========================================================="

BOOST_PREFIX="${BOOST_ROOT:-$HOME/boost-build-1.81.0}"
CLANG_PATH="${CMAKE_CXX_COMPILER:-/opt/homebrew/opt/llvm@17/bin/clang++}"
CLANG_C_PATH="${CMAKE_C_COMPILER:-/opt/homebrew/opt/llvm@17/bin/clang}"

if [ ! -d "$BOOST_PREFIX" ]; then
    echo "Error: Boost directory not found at $BOOST_PREFIX"
    echo "Please build Boost 1.81.0 or export BOOST_ROOT=/path/to/boost-build-1.81.0"
    exit 1
fi

if [ ! -f "$CLANG_PATH" ]; then
    echo "Warning: LLVM 17 clang++ not found at $CLANG_PATH, falling back to system clang++"
    CLANG_PATH="$(which clang++)"
    CLANG_C_PATH="$(which clang)"
fi

echo "-> Applying modern Boost compatibility adjustments across all submodules..."
find . -type f \( -name "*.h" -o -name "*.hpp" -o -name "*.cpp" \) -not -path "*/_build/*" | while read -r file; do
    sed -i '' 's/boost::asio::mutable_buffers_1/boost::asio::mutable_buffer/g' "$file" 2>/dev/null || true
done

mkdir -p _build && cd _build

echo "-> Running CMake configuration..."
cmake \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CLANG_PATH" \
  -DCMAKE_C_COMPILER="$CLANG_C_PATH" \
  -DBOOST_ROOT="$BOOST_PREFIX" \
  -DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR:-/opt/homebrew/opt/openssl@3}" \
  -DENABLE_MONGO=OFF \
  -DENABLE_ZEROMQ=OFF \
  -DXPX_STORAGE_SDK_NOT_BUILD_EXAMPLES=ON \
  -DCMAKE_CXX_FLAGS="-pthread" \
  ..

echo "-> Generating headers (make publish)..."
make publish

NUM_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "-> Compiling Sirius Core engine and plugins with $NUM_CORES cores..."
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
  -j"$NUM_CORES"

echo "=========================================================="
echo " Compilation Complete! Binaries are in _build/bin/"
echo "=========================================================="
