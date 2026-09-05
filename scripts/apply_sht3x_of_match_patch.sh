#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${RK3568_SDK:-/home/lyy/rk3568/SDK}
KERNEL="$SDK/kernel"
PATCH_FILE="$PROJECT_ROOT/kernel_patches/0001-hwmon-sht3x-add-of-match.patch"
TARGET="$KERNEL/drivers/hwmon/sht3x.c"

if [ ! -r "$PATCH_FILE" ] || [ ! -r "$TARGET" ]; then
    echo "Missing patch or kernel driver source" >&2
    exit 1
fi

if grep -q 'sht3x_of_match' "$TARGET"; then
    echo "SHT3x OF match patch is already applied: $TARGET"
    exit 0
fi

patch -d "$KERNEL" -p1 --dry-run < "$PATCH_FILE"
patch -d "$KERNEL" -p1 < "$PATCH_FILE"

echo "Applied SHT3x OF match patch to: $TARGET"
