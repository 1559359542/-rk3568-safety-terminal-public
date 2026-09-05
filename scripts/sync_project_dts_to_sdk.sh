#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${RK3568_SDK:-/home/lyy/rk3568/SDK}
DST="$SDK/kernel/arch/arm64/boot/dts/rockchip"

install -m 0644 \
  "$PROJECT_ROOT/dts_patches/rk3568-safety-terminal.dts" \
  "$DST/rk3568-safety-terminal.dts"

install -m 0644 \
  "$PROJECT_ROOT/dts_patches/rk3568-safety-terminal.dtsi" \
  "$DST/rk3568-safety-terminal.dtsi"

echo "Synchronized project DTS files to: $DST"
