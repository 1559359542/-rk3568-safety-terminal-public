#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dts="$project_root/boards/rk3568/dts/rk3568-safety-terminal-mipi-10p1-800x1280.dts"
sdk_root=${RK3568_SDK_DIR:-"$HOME/rk3568/SDK"}
sdk_dts_dir="$sdk_root/kernel/arch/arm64/boot/dts/rockchip"
target_dts="$sdk_dts_dir/rk3568-safety-terminal-mipi-10p1-800x1280.dts"
staged_dts="${target_dts}.new"

test -f "$source_dts" || {
    echo "missing project DTS: $source_dts" >&2
    exit 1
}
test -d "$sdk_dts_dir" || {
    echo "missing SDK DTS directory: $sdk_dts_dir" >&2
    exit 1
}
test ! -e "$target_dts" || {
    echo "refusing to overwrite SDK DTS: $target_dts" >&2
    exit 2
}
test ! -e "$staged_dts" || {
    echo "staged DTS already exists: $staged_dts" >&2
    exit 2
}

install -m 0644 "$source_dts" "$staged_dts"
mv "$staged_dts" "$target_dts"
sha256sum "$source_dts" "$target_dts"
