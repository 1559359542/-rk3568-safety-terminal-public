#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${RK3568_SDK:-/home/lyy/rk3568/SDK}
KERNEL="$SDK/kernel"
FRAGMENT="$PROJECT_ROOT/kernel_config/sht3x.config"
MERGE="$KERNEL/scripts/kconfig/merge_config.sh"

test -d "$KERNEL"
test -r "$KERNEL/.config"
test -r "$FRAGMENT"
test -x "$MERGE"

(
    cd "$KERNEL"
    ARCH=arm64 "$MERGE" .config "$FRAGMENT"
)

grep -qx 'CONFIG_SENSORS_SHT3x=y' "$KERNEL/.config"
grep -qx 'CONFIG_CRC8=y' "$KERNEL/.config"

echo "Applied SHT3x configuration to: $KERNEL/.config"
