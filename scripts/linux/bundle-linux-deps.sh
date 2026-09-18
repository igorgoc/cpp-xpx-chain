#!/usr/bin/env bash
set -euo pipefail

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../.." && pwd )"
BUILD_BIN="${DIR}/_build/bin"
TARGET_DIR="${1:-${DIR}/staging/bin}"
BOOST_LIB_DIR="${BOOST_ROOT:-$HOME/boost-build-1.81.0}/lib"
ROCKSDB_LIB_DIR="${ROCKSDB_ROOT:-$HOME/rocksdb-8.5.3}/lib"

echo "=========================================================="
echo " Packaging Standalone Portable Linux Sirius Engine Bundle"
echo " Source:      ${BUILD_BIN}"
echo " Destination: ${TARGET_DIR}"
echo "=========================================================="

mkdir -p "${TARGET_DIR}"

echo "-> Staging core executables..."
cp -f "${BUILD_BIN}"/sirius.bc "${TARGET_DIR}/"
[ -f "${BUILD_BIN}/catapult.recovery" ] && cp -f "${BUILD_BIN}/catapult.recovery" "${TARGET_DIR}/"

echo "-> Staging dynamic plugins and shared libraries..."
cp -d "${BUILD_BIN}"/*.so* "${TARGET_DIR}/" 2>/dev/null || true

echo "-> Bundling pinned Boost 1.81.0 shared libraries..."
if [ -d "${BOOST_LIB_DIR}" ]; then
    cp -d "${BOOST_LIB_DIR}"/libboost_*.so* "${TARGET_DIR}/"
else
    echo "::warning:: Boost library dir ${BOOST_LIB_DIR} not found!"
fi

echo "-> Bundling pinned RocksDB 8.5.3 shared libraries..."
if [ -d "${ROCKSDB_LIB_DIR}" ]; then
    cp -d "${ROCKSDB_LIB_DIR}"/librocksdb.so* "${TARGET_DIR}/"
elif [ -d "${ROCKSDB_ROOT:-$HOME/rocksdb-8.5.3}/lib64" ]; then
    cp -d "${ROCKSDB_ROOT:-$HOME/rocksdb-8.5.3}/lib64"/librocksdb.so* "${TARGET_DIR}/"
fi

echo "-> Bundling runtime compression & OpenSSL shared libraries..."
MULTIARCH=$(gcc -print-multiarch 2>/dev/null || echo "x86_64-linux-gnu")
for lib in libsnappy.so* libzstd.so* libcrypto.so* libssl.so*; do
    for path in "/usr/lib/${MULTIARCH}" "/lib/${MULTIARCH}" /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/local/lib /usr/lib /lib; do
        if compgen -G "${path}/${lib}" > /dev/null; then
            cp -d ${path}/${lib} "${TARGET_DIR}/" 2>/dev/null || true
            break
        fi
    done
done

echo "-> Applying relocatable DT_RUNPATH (\$ORIGIN:\$ORIGIN/../lib) using patchelf..."
chmod -R u+w "${TARGET_DIR}"

for elf in "${TARGET_DIR}"/*; do
    if [ -f "${elf}" ] && [ ! -L "${elf}" ]; then
        if file "${elf}" | grep -q "ELF"; then
            patchelf --set-rpath '$ORIGIN:$ORIGIN/../lib' "${elf}" 2>/dev/null || true
        fi
    fi
done

echo "=========================================================="
echo " Standalone Linux Engine Bundle Assembly Complete!"
echo " Total files in ${TARGET_DIR}: $(ls -1 "${TARGET_DIR}" | wc -l)"
echo "=========================================================="
