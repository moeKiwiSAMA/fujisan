#!/usr/bin/env bash
# build-initramfs.sh — Build Fujisan OS Stage-1 initramfs.cpio.gz (<= 5.25 MiB BANBANINJ budget)
#
# Modes:
#   --offline (recommended for CI / air-gapped builds):
#       Compiles freestanding pure-ARM32 (0-Thumb) /init plus fujisan-home,
#       fujisan-cam, and fujisan-term with zero libc/libgcc, verifies 0 Thumb ($t)
#       symbols, and packs a bit-for-bit deterministic initramfs.cpio.gz.
#   Default (online):
#       Uses Alpine Linux 3.21.10 armv7 minirootfs + rootfs/initramfs/init and
#       freestanding Fujisan apps (supports rootfs.squashfs + overlay.img/apkovl).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DL_DIR="${REPO_ROOT}/.tools/downloads"
STAGE_DIR="${REPO_ROOT}/build/initramfs-stage"
OUT_DIR="${REPO_ROOT}/build/out"
OUT_FILE="${OUT_DIR}/initramfs.cpio.gz"
OFFLINE=0

ALPINE_VER="3.21.10"
ALPINE_TAR="alpine-minirootfs-${ALPINE_VER}-armv7.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/${ALPINE_TAR}"

# Hard BANBANINJ staging budget (5.25 MiB)
MAX_INITRD_BYTES=5505024

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) OUT_FILE="$2"; shift 2 ;;
        --offline) OFFLINE=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--offline] [--output build/out/initramfs.cpio.gz]"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

pick_arm32_prefix() {
    if [[ -n "${ARM32_CROSS_COMPILE:-}" ]]; then
        echo "${ARM32_CROSS_COMPILE}"
        return 0
    fi
    if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
        echo "arm-linux-gnueabihf-"
        return 0
    fi
    if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
        echo "arm-none-eabi-"
        return 0
    fi
    echo "ERROR: neither arm-linux-gnueabihf-gcc nor arm-none-eabi-gcc found" >&2
    return 1
}

verify_zero_thumb() {
    local prefix="$1"
    local elf="$2"
    local thumb_syms

    thumb_syms="$("${prefix}readelf" -sW "$elf" | awk '$8 == "$t" || $8 ~ /^\$t\./ { print }' || true)"
    if [[ -n "$thumb_syms" ]]; then
        echo "ERROR: ${elf} contains Thumb (\$t) symbols!" >&2
        echo "$thumb_syms" >&2
        exit 1
    fi
    if ! "${prefix}readelf" -h "$elf" | grep -q 'Machine:[[:space:]]*ARM'; then
        echo "ERROR: ${elf} is not an ARM32 ELF binary!" >&2
        exit 1
    fi
    echo "[initramfs] Verified pure ARM32 (0 Thumb \$t symbols): ${elf}"
}

compile_fujisan_apps() {
    local prefix="$1"
    local dest_bin="$2"
    local cc="${prefix}gcc"
    local cflags=(
        -mcpu=cortex-a7 -marm -mno-thumb-interwork -mno-unaligned-access
        -mgeneral-regs-only -nostdlib -ffreestanding -static -Os -Wall -Wextra -Werror
        -fno-stack-protector -fno-asynchronous-unwind-tables
        -fno-tree-loop-distribute-patterns
        -Wl,--build-id=none -Wl,-static -Wl,-z,noexecstack
    )

    mkdir -p "$dest_bin"
    for app_spec in \
        "fujisan-home:apps/fujisan-home/fujisan_home.c:apps/fujisan-home" \
        "fujisan-cam:apps/fujisan-cam/fujisan_cam.c:apps/fujisan-cam" \
        "fujisan-term:apps/fujisan-term/fujisan_term.c:apps/fujisan-term"; do
        IFS=':' read -r bin_name src_rel inc_rel <<< "$app_spec"
        echo "[initramfs] Compiling pure-ARM32 ${dest_bin}/${bin_name}..."
        "$cc" "${cflags[@]}" \
            -I"${REPO_ROOT}/apps/common" \
            -I"${REPO_ROOT}/${inc_rel}" \
            -o "${dest_bin}/${bin_name}" \
            "${REPO_ROOT}/${src_rel}"
        chmod 0755 "${dest_bin}/${bin_name}"
        verify_zero_thumb "$prefix" "${dest_bin}/${bin_name}"
    done
}

build_offline_initramfs() {
    local prefix cc cflags
    for tool in cpio gzip find sort; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: missing required tool '$tool'" >&2
            exit 1
        fi
    done

    prefix="$(pick_arm32_prefix)"
    cc="${prefix}gcc"
    cflags=(
        -mcpu=cortex-a7 -marm -mno-thumb-interwork -mno-unaligned-access
        -mgeneral-regs-only -nostdlib -ffreestanding -static -Os -Wall -Wextra -Werror
        -fno-stack-protector -fno-asynchronous-unwind-tables
        -fno-tree-loop-distribute-patterns
        -Wl,--build-id=none -Wl,-static -Wl,-z,noexecstack
    )

    mkdir -p "$(dirname "$OUT_FILE")"
    rm -rf "$STAGE_DIR"
    mkdir -p "${STAGE_DIR}/proc" "${STAGE_DIR}/sys" "${STAGE_DIR}/dev" \
             "${STAGE_DIR}/dev/pts" "${STAGE_DIR}/dev/dri" "${STAGE_DIR}/run" \
             "${STAGE_DIR}/tmp" "${STAGE_DIR}/etc" "${STAGE_DIR}/mnt/sd1" \
             "${STAGE_DIR}/mnt/sd2" "${STAGE_DIR}/mnt/ro" \
             "${STAGE_DIR}/mnt/rw" "${STAGE_DIR}/newroot" \
             "${STAGE_DIR}/usr/local/bin"
    chmod 1777 "${STAGE_DIR}/tmp"

    echo "[initramfs] Compiling freestanding pure-ARM32 /init (${cc})..."
    "$cc" "${cflags[@]}" \
        -I"${REPO_ROOT}/apps/common" \
        -o "${STAGE_DIR}/init" \
        "${REPO_ROOT}/rootfs/initramfs/init_arm32.c"
    chmod 0755 "${STAGE_DIR}/init"
    verify_zero_thumb "$prefix" "${STAGE_DIR}/init"

    compile_fujisan_apps "$prefix" "${STAGE_DIR}/usr/local/bin"

    printf 'fujisan\n' > "${STAGE_DIR}/etc/hostname"
    printf '127.0.0.1\tlocalhost fujisan\n' > "${STAGE_DIR}/etc/hosts"
    cat > "${STAGE_DIR}/etc/os-release" <<'EOF'
NAME="Fujisan OS"
ID=fujisan
ID_LIKE=alpine
PRETTY_NAME="Fujisan OS (SPX4-MF Pure-ARM32 Stage-1)"
EOF

    find "$STAGE_DIR" -exec touch -h -d '@0' {} +

    echo "[initramfs] Packing deterministic offline ${OUT_FILE}..."
    (
        cd "$STAGE_DIR"
        find . -print0 | LC_ALL=C sort -z | \
            cpio --null -o -H newc -R 0:0 --reproducible --quiet 2>/dev/null || \
        find . -print0 | LC_ALL=C sort -z | \
            cpio --null -o -H newc -R 0:0 --quiet
    ) | gzip -9n > "$OUT_FILE"
}

if (( OFFLINE == 1 )); then
    build_offline_initramfs
else
    for tool in curl tar cpio gzip; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: missing required tool '$tool'" >&2
            exit 1
        fi
    done

    prefix="$(pick_arm32_prefix)"
    mkdir -p "$DL_DIR" "$(dirname "$OUT_FILE")"

    if [[ ! -f "${DL_DIR}/${ALPINE_TAR}" ]]; then
        echo "[initramfs] Downloading ${ALPINE_TAR}..."
        curl -fsSL --connect-timeout 15 --max-time 90 --retry 5 --retry-delay 2 \
            -o "${DL_DIR}/${ALPINE_TAR}" "${ALPINE_URL}"
    fi

    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR"
    tar -xzf "${DL_DIR}/${ALPINE_TAR}" -C "$STAGE_DIR"

    install -m 0755 "${REPO_ROOT}/rootfs/initramfs/init" "${STAGE_DIR}/init"
    compile_fujisan_apps "$prefix" "${STAGE_DIR}/usr/local/bin"

    printf 'fujisan\n' > "${STAGE_DIR}/etc/hostname"
    printf '127.0.0.1\tlocalhost fujisan\n' > "${STAGE_DIR}/etc/hosts"
    mkdir -p "${STAGE_DIR}/proc" "${STAGE_DIR}/sys" "${STAGE_DIR}/dev" \
             "${STAGE_DIR}/dev/pts" "${STAGE_DIR}/run" "${STAGE_DIR}/tmp" \
             "${STAGE_DIR}/mnt/sd1" "${STAGE_DIR}/mnt/sd2" \
             "${STAGE_DIR}/mnt/ro" "${STAGE_DIR}/mnt/rw" "${STAGE_DIR}/newroot"
    chmod 1777 "${STAGE_DIR}/tmp"
    rm -rf "${STAGE_DIR}/usr/share/doc" "${STAGE_DIR}/usr/share/man" "${STAGE_DIR}/var/cache/apk"/*

    echo "[initramfs] Packing ${OUT_FILE}..."
    (
        cd "$STAGE_DIR"
        find . -print0 | LC_ALL=C sort -z | \
            cpio --null -o -H newc -R 0:0 --quiet | \
            gzip -9n > "$OUT_FILE"
    )
fi

ISIZE="$(wc -c < "$OUT_FILE")"
echo "[initramfs] Built ${OUT_FILE}: ${ISIZE} bytes (BANBANINJ ceiling: ${MAX_INITRD_BYTES} B)"
if (( ISIZE > MAX_INITRD_BYTES )); then
    echo "ERROR: initramfs exceeds BANBANINJ 5.25 MiB budget!" >&2
    exit 1
fi
