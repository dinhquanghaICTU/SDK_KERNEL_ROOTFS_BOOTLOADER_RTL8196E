#!/bin/bash
# build_toolchain.sh — Build Lexra MIPS toolchain with crosstool-ng
#
# This script automates the complete toolchain build process including:
#   - Patch deployment to ~/.crosstool-ng/
#   - Toolchain compilation
#
# Output: ${PROJECT_ROOT}/x-tools/mips-lexra-linux-musl/
#
# J. Nilo - November 2025

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Determine project root (parent of 1-Build-Environment)
if [ -n "$PROJECT_ROOT" ]; then
    # Use environment variable if set
    :
else
    # Auto-detect: go up from 10-lexra-toolchain -> 1-Build-Environment -> project root
    PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
fi

TOOLCHAIN_PREFIX="${PROJECT_ROOT}/x-tools/mips-lexra-linux-musl"
TARBALL_CACHE="${PROJECT_ROOT}/downloads"

echo "========================================="
echo "  LEXRA MIPS TOOLCHAIN BUILD"
echo "========================================="
echo ""
echo "Project root: ${PROJECT_ROOT}"
echo "Toolchain will be installed to: ${TOOLCHAIN_PREFIX}"
echo ""

# Check if crosstool-ng is installed
if ! command -v ct-ng >/dev/null 2>&1; then
    echo "ERROR: crosstool-ng not found in PATH"
    echo ""
    echo "Run install_deps.sh first to install all dependencies"
    echo "including crosstool-ng."
    echo ""
    exit 1
fi

echo "crosstool-ng found: $(ct-ng version)"
echo ""

# Keep a project-local source cache so builds remain reproducible when a
# kernel.org CDN edge temporarily returns 404 for an otherwise valid release.
# crosstool-ng accepts either .tar.xz or .tar.gz from this directory.
LINUX_VERSION=$(sed -n 's/^CT_LINUX_VERSION="\([^"]*\)"/\1/p' \
    "${SCRIPT_DIR}/crosstool-ng.config")
LINUX_FALLBACK_SHA256="1f9b8724ecad389b57b50bcb15e538ba752e1f508a87d382c084d574776a90a6"
mkdir -p "$TARBALL_CACHE"
if [ ! -f "${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.xz" ] \
   && [ ! -f "${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.gz" ]; then
    echo "Linux ${LINUX_VERSION} tarball not cached; downloading GitHub fallback..."
    tmp="${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.gz.part"
    wget --tries=3 --timeout=30 -O "$tmp" \
        "https://github.com/torvalds/linux/archive/refs/tags/v${LINUX_VERSION}.tar.gz"
    tar -tzf "$tmp" >/dev/null
    mv "$tmp" "${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.gz"
    echo "Cached: ${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.gz"
    echo ""
fi
if [ -f "${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.gz" ]; then
    echo "${LINUX_FALLBACK_SHA256}  ${TARBALL_CACHE}/linux-${LINUX_VERSION}.tar.gz" \
        | sha256sum -c -
fi

# Check if toolchain already exists
if [ -d "$TOOLCHAIN_PREFIX" ]; then
    echo "Toolchain already exists at: $TOOLCHAIN_PREFIX"
    echo "Skipping build."
    exit 0
fi

# Deploy Lexra patches to crosstool-ng patches directory
echo "Deploying Lexra patches to ~/.crosstool-ng/..."
mkdir -p ~/.crosstool-ng/
cp -f -a "${SCRIPT_DIR}/patches" ~/.crosstool-ng/
echo "Patches deployed"
echo ""

# Create temporary build directory (kept on failure for inspection)
BUILD_DIR=$(mktemp -d)
cleanup() { [ $? -eq 0 ] && rm -rf "$BUILD_DIR" || echo "Build dir kept for inspection: $BUILD_DIR"; }
trap cleanup EXIT

cd "$BUILD_DIR"
echo "Build directory: $BUILD_DIR"
echo ""

# Generate config with correct prefix path
echo "Generating configuration..."
sed "s|CT_PREFIX_DIR=.*|CT_PREFIX_DIR=\"${TOOLCHAIN_PREFIX}\"|" \
    "${SCRIPT_DIR}/crosstool-ng.config" > .config

# Use the project-local cache populated above. Disable saving because the
# fallback archive is already persistent there.
sed -i 's/CT_SAVE_TARBALLS=y/# CT_SAVE_TARBALLS is not set/' .config
sed -i "s@CT_LOCAL_TARBALLS_DIR=.*@CT_LOCAL_TARBALLS_DIR=\"${TARBALL_CACHE}\"@" .config

echo "Configuration loaded"
echo ""

# Show configuration summary
echo "========================================="
echo "  TOOLCHAIN CONFIGURATION"
echo "========================================="
ct-ng show-config 2>/dev/null | grep -E "(Target|Vendor|OS|Kernel|C library|GCC|Binutils)" || true
echo "========================================="
echo ""

# Build toolchain
echo ""
echo "Building toolchain..."
echo "This will take approximately 30 minutes..."
echo ""

ct-ng build

echo ""
echo "========================================="
echo "  BUILD COMPLETE!"
echo "========================================="
echo ""
echo "Toolchain installed to: $TOOLCHAIN_PREFIX"
echo ""
echo "Add to your ~/.bashrc:"
echo "  export PATH=\"$TOOLCHAIN_PREFIX/bin:\$PATH\""
echo ""
echo "Verify installation:"
echo "  mips-lexra-linux-musl-gcc --version"
echo ""
