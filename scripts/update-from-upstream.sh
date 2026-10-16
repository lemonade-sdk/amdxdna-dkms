#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Update amdxdna-dkms from upstream kernel and firmware repositories
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

KERNEL_REPO="https://gitlab.freedesktop.org/drm/misc/kernel.git"
KERNEL_BRANCH="drm-misc-fixes"
FIRMWARE_REPO="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git"

echo "=== Updating amdxdna-dkms from upstream ==="

# Check if we have existing directories or need to clone
if [ -d "$PROJECT_ROOT/drm-kernel" ] && [ -d "$PROJECT_ROOT/linux-firmware" ]; then
    echo ">>> Using existing upstream directories..."
    KERNEL_DIR="$PROJECT_ROOT/drm-kernel"
    FIRMWARE_DIR="$PROJECT_ROOT/linux-firmware"
    TEMP_CLEANUP="false"
else
    echo ">>> Cloning repositories to temporary directory..."
    TEMP_DIR=$(mktemp -d)
    trap 'rm -rf "$TEMP_DIR"' EXIT

    echo "  Cloning kernel repository (shallow)..."
    git clone --depth=1 --branch="$KERNEL_BRANCH" "$KERNEL_REPO" "$TEMP_DIR/kernel"

    echo "  Cloning firmware repository (shallow)..."
    git clone --depth=1 "$FIRMWARE_REPO" "$TEMP_DIR/firmware"

    KERNEL_DIR="$TEMP_DIR/kernel"
    FIRMWARE_DIR="$TEMP_DIR/firmware"
    TEMP_CLEANUP="true"
fi

cd "$KERNEL_DIR"

# Get version information
KERNEL_VERSION=$(make -s kernelversion 2>/dev/null || echo "unknown")
GIT_COMMIT=$(git rev-parse --short HEAD)
GIT_DATE=$(git log -1 --format=%cd --date=format:%Y%m%d)

echo ""
echo ">>> Source information:"
echo "  Kernel version: $KERNEL_VERSION"
echo "  Git commit: $GIT_COMMIT"
echo "  Git date: $GIT_DATE"

# Create source directories
echo ""
echo ">>> Creating source directories..."
mkdir -p "$PROJECT_ROOT/src/amdxdna"
mkdir -p "$PROJECT_ROOT/src/include/uapi/drm"
mkdir -p "$PROJECT_ROOT/src/trace/events"

# Copy driver sources
echo ""
echo ">>> Copying driver sources..."
DRIVER_SRC="drivers/accel/amdxdna"
cp -v "$DRIVER_SRC"/*.c "$PROJECT_ROOT/src/amdxdna/"
cp -v "$DRIVER_SRC"/*.h "$PROJECT_ROOT/src/amdxdna/"

# Copy our compatibility header (not from upstream)
echo ""
echo ">>> Installing compatibility header..."
cp -v "$PROJECT_ROOT/compat/amdxdna_compat.h" "$PROJECT_ROOT/src/amdxdna/"

# Copy UAPI header and fix include path
echo ""
echo ">>> Copying UAPI header..."
cp -v include/uapi/drm/amdxdna_accel.h "$PROJECT_ROOT/src/include/uapi/drm/"
# Fix the drm.h include to use angle brackets for system header
sed -i 's|#include "drm.h"|#include <drm/drm.h>|' "$PROJECT_ROOT/src/include/uapi/drm/amdxdna_accel.h"

# Copy tracepoint header
echo ""
echo ">>> Copying tracepoint header..."
cp -v include/trace/events/amdxdna.h "$PROJECT_ROOT/src/trace/events/"

# Apply compatibility patches
echo ""
echo ">>> Applying compatibility patches..."

# Add GENMASK_U64 compatibility to amdxdna_error.h
sed -i '/^#include <linux\/bits.h>/a\\n/* Compatibility shim for older kernels that don'\''t have GENMASK_U64 */\n#ifndef GENMASK_U64\n#define GENMASK_U64(h, l) GENMASK_ULL(h, l)\n#endif' "$PROJECT_ROOT/src/amdxdna/amdxdna_error.h"

# Add compat header include to source files that need it
# These files use kzalloc_obj, kzalloc_flex, etc.
for file in aie2_ctx.c aie2_error.c aie2_pci.c aie2_solver.c \
            amdxdna_ctx.c amdxdna_gem.c amdxdna_mailbox.c \
            amdxdna_pci_drv.c amdxdna_ubuf.c \
            npu1_regs.c npu4_regs.c npu5_regs.c npu6_regs.c; do
  # Find the first local include line and add compat header before it
  if [ -f "$PROJECT_ROOT/src/amdxdna/$file" ]; then
    sed -i '0,/^#include "/{s|^#include "|#include "amdxdna_compat.h"\n#include "|;}' "$PROJECT_ROOT/src/amdxdna/$file"
    echo "  Added compat header to $file"
  fi
done

# Generate DKMS Makefile from upstream Makefile
echo ""
echo ">>> Generating Makefile.dkms..."
cat > "$PROJECT_ROOT/src/amdxdna/Makefile" << 'EOF'
# SPDX-License-Identifier: GPL-2.0-only

# Prepend our include paths to ensure bundled headers override system ones
# This is critical because 6.14 kernels may ship with older amdxdna_accel.h
LINUXINCLUDE := -I$(src)/include/uapi -I$(src) $(LINUXINCLUDE)

EOF

# Extract object file list from upstream Makefile
grep -E '^\s+\w+\.o \\' "$DRIVER_SRC/Makefile" | sed 's/^//' >> "$PROJECT_ROOT/src/amdxdna/Makefile.tmp"
# Get the last object file (without backslash)
grep -E '^\s+\w+\.o$' "$DRIVER_SRC/Makefile" | sed 's/^//' >> "$PROJECT_ROOT/src/amdxdna/Makefile.tmp"

# Add the full amdxdna-y section
echo "amdxdna-y := \\" >> "$PROJECT_ROOT/src/amdxdna/Makefile"
cat "$PROJECT_ROOT/src/amdxdna/Makefile.tmp" >> "$PROJECT_ROOT/src/amdxdna/Makefile"
rm "$PROJECT_ROOT/src/amdxdna/Makefile.tmp"

# Add obj-m
echo "" >> "$PROJECT_ROOT/src/amdxdna/Makefile"
echo "obj-m := amdxdna.o" >> "$PROJECT_ROOT/src/amdxdna/Makefile"

echo "Created Makefile"

# Copy firmware
echo ""
echo ">>> Copying firmware..."
cd "$FIRMWARE_DIR"

# Create firmware directory
mkdir -p "$PROJECT_ROOT/firmware/amdnpu"

# Copy firmware files (all .sbin files from each device directory)
for DEVICE_DIR in amdnpu/*/; do
    DEVICE_NAME=$(basename "$DEVICE_DIR")
    mkdir -p "$PROJECT_ROOT/firmware/amdnpu/$DEVICE_NAME"

    echo "  Device: $DEVICE_NAME"

    # Copy all versioned firmware files
    cp -v "$DEVICE_DIR"npu.sbin.* "$PROJECT_ROOT/firmware/amdnpu/$DEVICE_NAME/" || true
done

# Parse WHENCE to create symlinks
echo ""
echo ">>> Creating firmware symlinks from WHENCE..."
grep -A 100 "^File: amdnpu/" WHENCE | while read -r line; do
    if [[ $line =~ ^Link:\ (amdnpu/.+)\ -\>\ (.+)$ ]]; then
        LINK_PATH="${BASH_REMATCH[1]}"
        TARGET="${BASH_REMATCH[2]}"

        FULL_LINK="$PROJECT_ROOT/firmware/$LINK_PATH"
        LINK_DIR=$(dirname "$FULL_LINK")

        mkdir -p "$LINK_DIR"

        # Create symlink relative to the directory
        cd "$LINK_DIR"
        ln -sf "$TARGET" "$(basename "$FULL_LINK")"
        echo "  Created: $LINK_PATH -> $TARGET"
        cd "$FIRMWARE_DIR"
    elif [[ $line =~ ^File: ]] && [[ ! $line =~ amdnpu ]]; then
        # Stop when we hit a different driver section
        break
    fi
done

# Copy firmware license
cp -v LICENSE.amdnpu "$PROJECT_ROOT/firmware/amdnpu/"

# Update debian/changelog
echo ""
echo ">>> Updating debian/changelog..."
cd "$PROJECT_ROOT"

# Create debian directory if it doesn't exist
mkdir -p debian

# Version format: KERNEL_VERSION+gitDATE.COMMIT-1
PKG_VERSION="${KERNEL_VERSION}+git${GIT_DATE}.${GIT_COMMIT}-1"

# Check if debchange (dch) is available
if command -v dch &> /dev/null; then
    # Create or update changelog
    if [ ! -f debian/changelog ]; then
        # Create new changelog
        dch --create --package amdxdna-dkms --newversion "$PKG_VERSION" \
            --distribution unstable \
            "Update to kernel $KERNEL_VERSION (commit $GIT_COMMIT)"
    else
        # Add new entry
        dch --newversion "$PKG_VERSION" \
            --distribution unstable \
            "Update to kernel $KERNEL_VERSION (commit $GIT_COMMIT)"
    fi

    # Set maintainer if DEBFULLNAME and DEBEMAIL are set
    if [ -n "$DEBFULLNAME" ] && [ -n "$DEBEMAIL" ]; then
        echo "Maintainer: $DEBFULLNAME <$DEBEMAIL>"
    else
        echo "Note: Set DEBFULLNAME and DEBEMAIL environment variables for proper maintainer info"
    fi
else
    echo "Warning: debchange (dch) not found. Creating basic changelog manually."
    cat > debian/changelog << CHANGELOG_EOF
amdxdna-dkms ($PKG_VERSION) unstable; urgency=medium

  * Update to kernel $KERNEL_VERSION (commit $GIT_COMMIT)

 -- Builder <builder@localhost>  $(date -R)

CHANGELOG_EOF
fi

echo ""
echo "=== Update complete ==="
echo "Package version: $PKG_VERSION"
echo ""
echo "Next steps:"
echo "  1. Review changes: git diff"
echo "  2. Build package: make build-deb"
echo "  3. Install package: make install"
