#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${RK3568_SDK:-/home/lyy/rk3568/SDK}
DTB=rockchip/rk3568-safety-terminal.dtb

"$PROJECT_ROOT/scripts/sync_project_dts_to_sdk.sh"
make -C "$SDK/kernel" ARCH=arm64 "$DTB"

echo "Built: $SDK/kernel/arch/arm64/boot/dts/$DTB"
