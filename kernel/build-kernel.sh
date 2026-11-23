#!/usr/bin/env bash
# build-kernel.sh — Reproducible pure-ARM32 Linux 6.6.63 kernel + DTB + modules builder
# for Fujisan OS on SPX4-MF Medium-Format Camera (SPX4 Quad-Core Cortex-A7).
#
# Outputs (under build/kernel-out/, git-ignored):
#   - zImage                         (pure ARM32 -marm, 0 Thumb, <= 5.00 MiB BANBANINJ budget)
#   - fujisan-spx4-mf.dtb       (compiled Device Tree Blob, <= 256 KiB)
#   - modules/lib/modules/<kver>/    (loadable kernel modules tree)
#   - kernel.env                     (KERNEL_RELEASE, DTB_NAME, EXTRA_CMDLINE)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLS_DIR="${REPO_ROOT}/.tools"
DL_DIR="${TOOLS_DIR}/downloads"
SRC_DIR="${TOOLS_DIR}/src"
BUILD_DIR="${REPO_ROOT}/build"
KBUILD_OUT="${BUILD_DIR}/kbuild"
OUT_DIR="${BUILD_DIR}/kernel-out"

LINUX_VER="${LINUX_VER:-6.6.63}"
LINUX_TAR="linux-${LINUX_VER}.tar.xz"
LINUX_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${LINUX_TAR}"

CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
JOBS="${JOBS:-$(nproc)}"

for tool in "${CROSS_COMPILE}gcc" "${CROSS_COMPILE}objdump" dtc make tar patch xz; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: missing required build tool '$tool'" >&2
        exit 1
    fi
done

mkdir -p "$DL_DIR" "$SRC_DIR" "$KBUILD_OUT" "$OUT_DIR"

TARBALL_PATH="${LINUX_TARBALL:-${DL_DIR}/${LINUX_TAR}}"
if [[ ! -f "$TARBALL_PATH" ]]; then
    for candidate in \
        "${REPO_ROOT}/../banban-injector/.tools/downloads/${LINUX_TAR}" \
        "${REPO_ROOT}/../fujisan-os/.tools/downloads/${LINUX_TAR}"; do
        if [[ -f "$candidate" ]]; then
            echo "[kernel] Reusing local tarball cache: ${candidate}"
            ln -sf "$(realpath "$candidate")" "${DL_DIR}/${LINUX_TAR}"
            TARBALL_PATH="${DL_DIR}/${LINUX_TAR}"
            break
        fi
    done
    if [[ ! -f "$TARBALL_PATH" ]]; then
        echo "[kernel] Downloading ${LINUX_URL}..."
        curl -fsSL -o "${DL_DIR}/${LINUX_TAR}" "${LINUX_URL}"
        TARBALL_PATH="${DL_DIR}/${LINUX_TAR}"
    fi
fi

KERNEL_SRC="${SRC_DIR}/linux-${LINUX_VER}"
PATCH_STAMP="${KERNEL_SRC}/.fujisan_spx4_patched_v1"
if [[ ! -d "$KERNEL_SRC" ]]; then
    echo "[kernel] Extracting ${LINUX_TAR}..."
    tar -xJf "$TARBALL_PATH" -C "$SRC_DIR"
fi

if [[ ! -f "$PATCH_STAMP" ]]; then
    for p in "${SCRIPT_DIR}"/patches/*.patch; do
        echo "[kernel] Applying $(basename "$p")..."
        patch -d "$KERNEL_SRC" -p1 < "$p"
    done
    touch "$PATCH_STAMP"
fi

rm -rf "${KERNEL_SRC}/drivers/fujisan-spx4"
cp -a "${REPO_ROOT}/drivers" "${KERNEL_SRC}/drivers/fujisan-spx4"

cp -f "${SCRIPT_DIR}/fujisan_spx4_mf_defconfig" "${KBUILD_OUT}/.config"

echo "[kernel] Configuring Linux ${LINUX_VER} (ARCH=arm CROSS_COMPILE=${CROSS_COMPILE})..."
make -C "$KERNEL_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" olddefconfig >/dev/null

# Verify pure-ARM32 & NOR-Flash safety invariants
if grep -q '^CONFIG_THUMB2_KERNEL=y' "${KBUILD_OUT}/.config"; then
    echo "ERROR: CONFIG_THUMB2_KERNEL must remain disabled (pure ARM32 required)!" >&2
    exit 1
fi
if grep -q '^CONFIG_ARM_THUMB=y' "${KBUILD_OUT}/.config"; then
    echo "ERROR: CONFIG_ARM_THUMB must remain disabled!" >&2
    exit 1
fi
if grep -q '^CONFIG_MTD=y' "${KBUILD_OUT}/.config"; then
    echo "ERROR: CONFIG_MTD must remain disabled to protect NOR calibration/BKUP!" >&2
    exit 1
fi

echo "[kernel] Building zImage and modules (-j${JOBS})..."
make -C "$KERNEL_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" zImage modules

echo "[kernel] Verifying pure ARM32 (0 Thumb \$t symbols in vmlinux & fujisan-spx4 .ko)..."
if "${CROSS_COMPILE}objdump" -t "${KBUILD_OUT}/vmlinux" | grep -q ' \$t$'; then
    echo "ERROR: vmlinux contains Thumb (\$t) symbols!" >&2
    exit 1
fi
while IFS= read -r ko; do
    if "${CROSS_COMPILE}objdump" -t "$ko" | grep -q ' \$t$'; then
        echo "ERROR: module ${ko} contains Thumb (\$t) symbols!" >&2
        exit 1
    fi
done < <(find "${KBUILD_OUT}/drivers/fujisan-spx4" -name '*.ko' 2>/dev/null)

cp -f "${KBUILD_OUT}/arch/arm/boot/zImage" "${OUT_DIR}/zImage"

rm -rf "${OUT_DIR}/modules"
mkdir -p "${OUT_DIR}/modules"
make -C "$KERNEL_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
    INSTALL_MOD_PATH="${OUT_DIR}/modules" INSTALL_MOD_STRIP=1 modules_install >/dev/null

KREL="$(make -s -C "$KERNEL_SRC" O="$KBUILD_OUT" ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" kernelrelease)"

echo "[kernel] Compiling dts/fujisan-spx4-mf.dts..."
dtc -Wno-unit_address_vs_reg -I dts -O dtb \
    -o "${OUT_DIR}/fujisan-spx4-mf.dtb" \
    "${REPO_ROOT}/dts/fujisan-spx4-mf.dts"

cat > "${OUT_DIR}/kernel.env" <<EOF
KERNEL_RELEASE=${KREL}
DTB_NAME=fujisan-spx4-mf.dtb
EXTRA_CMDLINE=console=tty0 nosmp loglevel=7 rdinit=/init
EOF

ZSIZE="$(stat -c %s "${OUT_DIR}/zImage")"
DSIZE="$(stat -c %s "${OUT_DIR}/fujisan-spx4-mf.dtb")"
MODCOUNT="$(find "${OUT_DIR}/modules" -name '*.ko' | wc -l)"

echo ""
echo "[kernel] Fujisan OS KERNEL BUILD SUCCESSFUL!"
echo "  KERNEL_RELEASE   : ${KREL}"
echo "  zImage           : ${ZSIZE} B (BANBANINJ staging limit: 5,242,880 B)"
echo "  DTB              : ${DSIZE} B (BANBANINJ staging limit: 262,144 B)"
echo "  Loadable modules : ${MODCOUNT} .ko files in build/kernel-out/modules/"

if (( ZSIZE > 5242880 )); then
    echo "ERROR: zImage (${ZSIZE} B) exceeds 5.00 MiB BANBANINJ staging budget!" >&2
    exit 1
fi
if (( DSIZE > 262144 )); then
    echo "ERROR: DTB (${DSIZE} B) exceeds 256 KiB BANBANINJ staging budget!" >&2
    exit 1
fi
echo "[kernel] All pure-ARM32, NOR-safety, and size-budget checks PASSED!"
