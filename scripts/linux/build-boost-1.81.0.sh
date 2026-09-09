#!/usr/bin/env bash
set -euo pipefail

BOOST_VERSION="1.81.0"
BOOST_UNDERSCORE="1_81_0"
BOOST_INSTALL_DIR="${BOOST_ROOT:-$HOME/boost-build-${BOOST_VERSION}}"
BUILD_TMP_DIR="/tmp/boost_build_${BOOST_UNDERSCORE}"
CORES=$(nproc || echo 4)

echo "=========================================================="
echo " Building Pinned Boost ${BOOST_VERSION} for Linux (x86_64)"
echo " Install Destination: ${BOOST_INSTALL_DIR}"
echo " Cores: ${CORES}"
echo "=========================================================="

if [ -f "${BOOST_INSTALL_DIR}/include/boost/version.hpp" ]; then
    echo "✓ Boost ${BOOST_VERSION} already installed at ${BOOST_INSTALL_DIR}"
    exit 0
fi

rm -rf "${BUILD_TMP_DIR}"
mkdir -p "${BUILD_TMP_DIR}"
cd "${BUILD_TMP_DIR}"

TARBALL="boost_${BOOST_UNDERSCORE}.tar.gz"
echo "-> Downloading Boost ${BOOST_VERSION} source..."
if ! curl -sSL -f -o "${TARBALL}" "https://archives.boost.io/release/${BOOST_VERSION}/source/${TARBALL}"; then
    echo "Fallback to jfrog..."
    curl -sSL -f -o "${TARBALL}" "https://boostorg.jfrog.io/artifactory/main/release/${BOOST_VERSION}/source/${TARBALL}"
fi

echo "-> Unpacking Boost source archive..."
tar -xzf "${TARBALL}"
cd "boost_${BOOST_UNDERSCORE}"

echo "-> Bootstrapping Boost build engine..."
./bootstrap.sh --prefix="${BOOST_INSTALL_DIR}"

echo "-> Compiling and installing Boost libraries..."
./b2 --prefix="${BOOST_INSTALL_DIR}" \
    --without-python \
    link=shared,static \
    threading=multi \
    variant=release \
    cxxflags="-fPIC -march=x86-64 -mtune=generic" \
    cflags="-fPIC -march=x86-64 -mtune=generic" \
    -j"${CORES}" \
    install

echo "-> Cleaning temporary build files..."
rm -rf "${BUILD_TMP_DIR}"

if [ -f "${BOOST_INSTALL_DIR}/include/boost/version.hpp" ]; then
    echo "=========================================================="
    echo "✓ Boost ${BOOST_VERSION} successfully installed to ${BOOST_INSTALL_DIR}"
    echo "=========================================================="
else
    echo "::error:: Boost installation failed!"
    exit 1
fi
