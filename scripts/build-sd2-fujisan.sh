#!/usr/bin/env bash
# build-sd2-fujisan.sh — Dual-Format SD Card Slot 2 (BANBANINJ) Packager for Fujisan OS
#
# Exports BOTH formats by default (--format both):
#   1. build/sd2-fujisan.img        Rootless MBR + FAT32 (OEM FF-DSC01) image for raw SD2 / dd
#   2. build/sd-card-slot-2/        Ready-to-drop host folder for spx4-emulator
#                                   directory-mode (audited: 0 symlinks, legal FAT32 names,
#                                   0 case-insensitive collisions)
#
# Strictly conforms to BANBANINJ (B:\BANBANINJ) staging budgets:
#   - zImage                   <= 5.00 MiB (5,242,880 B)
#   - fujisan-spx4-mf.dtb <= 256 KiB  (262,144 B)
#   - initramfs.cpio.gz        <= 5.25 MiB (5,505,024 B)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KERNEL_OUT="${REPO_ROOT}/build/kernel-out"
OUT_DIR="${REPO_ROOT}/build/out"
SD2_DIR="${REPO_ROOT}/build/sd-card-slot-2"
IMG_PATH="${REPO_ROOT}/build/sd2-fujisan.img"

ZIMAGE=""
DTB=""
INITRAMFS="${OUT_DIR}/initramfs.cpio.gz"
SQUASHFS="${OUT_DIR}/rootfs.squashfs"
BANBANINJ_BIN=""
FORMAT="both"
MINIMAL_MODE=0
ALLOW_DUMMY_ZIMAGE=0
IMG_SIZE_MB=""
PART_START_LBA=2048

MAX_ZIMAGE_BYTES=5242880
MAX_DTB_BYTES=262144
MAX_INITRD_BYTES=5505024
DTB_NAME="fujisan-spx4-mf.dtb"

usage() {
    cat <<EOF
Usage: $0 [options]
  --kernel-out <dir>   Kernel handoff dir (default: build/kernel-out)
  --zimage <path>      Override zImage path
  --dtb <path>         Override DTB path
  --initramfs <path>   Override initramfs.cpio.gz (default: build/out/initramfs.cpio.gz)
  --squashfs <path>    Override rootfs.squashfs (default: build/out/rootfs.squashfs)
  --banbaninj <path>   Optional BANBANINJ second-stage loader binary
  --format <mode>      Output format: both | img | dir (default: both)
  --output <path>      Output MBR+FAT32 image (default: build/sd2-fujisan.img)
  --sd2-dir <dir>      Output directory-mode tree (default: build/sd-card-slot-2)
  --size-mb <N>        Override FAT32 image size in MiB (auto-sized if omitted)
  --minimal            Skip rootfs.squashfs (initramfs-only fast smoke image)
  --dummy-zimage       In --minimal mode, generate a 4 KiB placeholder zImage if absent (CI smoke)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --kernel-out)    KERNEL_OUT="$2"; shift 2 ;;
        --zimage)        ZIMAGE="$2"; shift 2 ;;
        --dtb)           DTB="$2"; shift 2 ;;
        --initramfs)     INITRAMFS="$2"; shift 2 ;;
        --squashfs)      SQUASHFS="$2"; shift 2 ;;
        --banbaninj)     BANBANINJ_BIN="$2"; shift 2 ;;
        --format)        FORMAT="$2"; shift 2 ;;
        --output)        IMG_PATH="$2"; shift 2 ;;
        --sd2-dir)       SD2_DIR="$2"; shift 2 ;;
        --size-mb)       IMG_SIZE_MB="$2"; shift 2 ;;
        --minimal)       MINIMAL_MODE=1; shift ;;
        --dummy-zimage)  ALLOW_DUMMY_ZIMAGE=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

case "$FORMAT" in
    both|img|dir) ;;
    *) echo "ERROR: --format must be both, img, or dir" >&2; exit 1 ;;
esac

if [[ -f "${KERNEL_OUT}/kernel.env" ]]; then
    # shellcheck disable=SC1091
    . "${KERNEL_OUT}/kernel.env"
fi

[[ -n "$ZIMAGE" ]] || ZIMAGE="${KERNEL_OUT}/zImage"
if [[ -z "$DTB" ]]; then
    if [[ -f "${KERNEL_OUT}/${DTB_NAME}" ]]; then
        DTB="${KERNEL_OUT}/${DTB_NAME}"
    else
        DTB="${REPO_ROOT}/build/ci/${DTB_NAME}"
    fi
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Auto-compile DTB if missing
if [[ ! -f "$DTB" ]]; then
    mkdir -p "$(dirname "$DTB")"
    dtc -Wno-unit_address_vs_reg -I dts -O dtb -o "$DTB" "${REPO_ROOT}/dts/fujisan-spx4-mf.dts"
fi

# Auto-build offline initramfs if missing and script exists
if [[ ! -f "$INITRAMFS" && -x "${SCRIPT_DIR}/build-initramfs.sh" ]]; then
    "${SCRIPT_DIR}/build-initramfs.sh" --offline --output "$INITRAMFS"
fi

# In --minimal --dummy-zimage CI mode, synthesize a tiny valid-header placeholder zImage
if [[ ! -f "$ZIMAGE" ]]; then
    if (( MINIMAL_MODE == 1 && ALLOW_DUMMY_ZIMAGE == 1 )); then
        ZIMAGE="${TMP_DIR}/zImage"
        python3 - "$ZIMAGE" <<'PYEOF'
import struct, sys
buf = bytearray(4096)
# Standard ARM zImage magic 0x016F2818 at offset 0x24
struct.pack_into("<III", buf, 0x24, 0x016F2818, 0, len(buf))
with open(sys.argv[1], "wb") as f:
    f.write(buf)
PYEOF
        echo "[sd2] Synthesized 4096 B CI placeholder zImage (--minimal --dummy-zimage)"
    else
        echo "ERROR: missing zImage at ${ZIMAGE}. Run kernel/build-kernel.sh first." >&2
        exit 1
    fi
fi

for req in "$ZIMAGE" "$DTB" "$INITRAMFS"; do
    if [[ ! -f "$req" ]]; then
        echo "ERROR: missing required artifact: $req" >&2
        exit 1
    fi
done

if (( MINIMAL_MODE == 0 )) && [[ ! -f "$SQUASHFS" ]]; then
    echo "ERROR: missing ${SQUASHFS}. Run scripts/build-rootfs.sh first (or pass --minimal)." >&2
    exit 1
fi

ZSIZE="$(wc -c < "$ZIMAGE")"
DSIZE="$(wc -c < "$DTB")"
ISIZE="$(wc -c < "$INITRAMFS")"
SSIZE=0
(( MINIMAL_MODE == 0 )) && SSIZE="$(wc -c < "$SQUASHFS")"

echo "[sd2] Fujisan OS artifact check against BANBANINJ staging limits:"
echo "  zImage                   : ${ZSIZE} B (limit ${MAX_ZIMAGE_BYTES} B)"
echo "  ${DTB_NAME} : ${DSIZE} B (limit ${MAX_DTB_BYTES} B)"
echo "  initramfs.cpio.gz        : ${ISIZE} B (limit ${MAX_INITRD_BYTES} B)"
(( MINIMAL_MODE == 0 )) && echo "  rootfs.squashfs          : ${SSIZE} B (loaded from SD2 FAT32)"

(( ZSIZE <= MAX_ZIMAGE_BYTES )) || { echo "ERROR: zImage exceeds 5.00 MiB!" >&2; exit 1; }
(( DSIZE <= MAX_DTB_BYTES ))    || { echo "ERROR: DTB exceeds 256 KiB!" >&2; exit 1; }
(( ISIZE <= MAX_INITRD_BYTES )) || { echo "ERROR: initramfs exceeds 5.25 MiB!" >&2; exit 1; }

# 1. Populate SD2 directory tree (B:\ layout)
STAGE_DIR="${TMP_DIR}/sd2-stage"
mkdir -p "${STAGE_DIR}/FFDB" \
         "${STAGE_DIR}/DCIM/100_FJOS" \
         "${STAGE_DIR}/banban/fujisan" \
         "${STAGE_DIR}/banban/plugins" \
         "${STAGE_DIR}/banban/themes"

# Standard 1024-byte Fujisan FF-DSC01 transfer marker
python3 - "${STAGE_DIR}/FFDB/FFXFER.DAT" <<'PYEOF'
import sys
buf = bytearray(1024)
buf[0:8] = b"FF-DSC01"
with open(sys.argv[1], "wb") as f:
    f.write(buf)
PYEOF

cat > "${STAGE_DIR}/banban/boot.cfg" <<EOF
# BANBANINJ Multi-Boot Configuration (B:\banban\boot.cfg)
# Generated by Fujisan OS scripts/build-sd2-fujisan.sh
menu=1
default=fujisan
splash_ms=1800
uart=0

[entry:fujisan]
title=Fujisan OS (SPX4-MF Linux)
mode=alpine
kernel=B:\banban\fujisan\zImage
dtb=B:\banban\fujisan\\${DTB_NAME}
initrd=B:\banban\fujisan\initramfs.cpio.gz
cmdline=console=tty0 nosmp loglevel=7 rdinit=/init

[entry:stock]
title=Stock Fujisan 2.13
mode=stock
EOF

cp -f "$ZIMAGE"    "${STAGE_DIR}/banban/fujisan/zImage"
cp -f "$DTB"       "${STAGE_DIR}/banban/fujisan/${DTB_NAME}"
cp -f "$INITRAMFS" "${STAGE_DIR}/banban/fujisan/initramfs.cpio.gz"
if (( MINIMAL_MODE == 0 )); then
    cp -f "$SQUASHFS" "${STAGE_DIR}/banban/fujisan/rootfs.squashfs"
fi
if [[ -n "$BANBANINJ_BIN" ]]; then
    [[ -f "$BANBANINJ_BIN" ]] || { echo "ERROR: --banbaninj not found: $BANBANINJ_BIN" >&2; exit 1; }
    cp -f "$BANBANINJ_BIN" "${STAGE_DIR}/BANBANINJ"
fi

# Generate MANIFEST.TXT with SHA-256 checksums
{
    echo "# Fujisan OS SD2 Manifest (BANBANINJ compatible)"
    echo "entry=fujisan"
    echo "minimal=${MINIMAL_MODE}"
    (cd "$STAGE_DIR" && sha256sum banban/boot.cfg banban/fujisan/zImage "banban/fujisan/${DTB_NAME}" banban/fujisan/initramfs.cpio.gz $( (( MINIMAL_MODE == 0 )) && echo banban/fujisan/rootfs.squashfs ))
} > "${STAGE_DIR}/banban/MANIFEST.TXT"

# Audit FAT32 directory tree (0 symlinks, legal FAT32 names, 0 case-insensitive collisions)
python3 - "$STAGE_DIR" <<'PYEOF'
import os, sys
root = sys.argv[1]
illegal = set('<>:"/\\|?*')
for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
    seen = {}
    for name in dirnames + filenames:
        full = os.path.join(dirpath, name)
        if os.path.islink(full):
            raise SystemExit(f"ERROR: symlink forbidden in SD2 tree: {full}")
        if any(c in illegal for c in name):
            raise SystemExit(f"ERROR: illegal FAT32 character in name: {full}")
        low = name.lower()
        if low in seen:
            raise SystemExit(f"ERROR: case-insensitive FAT32 collision: {seen[low]} vs {name}")
        seen[low] = name
print("[sd2] Directory tree FAT32 audit PASSED (0 symlinks, 0 collisions)")
PYEOF

if [[ "$FORMAT" == "both" || "$FORMAT" == "dir" ]]; then
    rm -rf "$SD2_DIR"
    mkdir -p "$(dirname "$SD2_DIR")"
    cp -a "$STAGE_DIR" "$SD2_DIR"
    echo "[sd2] Exported directory-mode tree: ${SD2_DIR}"
fi

if [[ "$FORMAT" == "both" || "$FORMAT" == "img" ]]; then
    for tool in mkfs.vfat mmd mcopy mdir dd; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "ERROR: missing required FAT32 tool '$tool'" >&2
            exit 1
        fi
    done

    PAYLOAD_BYTES=$(( ZSIZE + DSIZE + ISIZE + SSIZE ))
    AUTO_SIZE_MB=$(( (PAYLOAD_BYTES / 1048576) + 48 ))
    (( AUTO_SIZE_MB < 64 )) && AUTO_SIZE_MB=64
    [[ -n "$IMG_SIZE_MB" ]] || IMG_SIZE_MB="$AUTO_SIZE_MB"
    (( IMG_SIZE_MB >= 40 )) || { echo "ERROR: --size-mb must be >= 40" >&2; exit 1; }

    TOTAL_SECTORS=$(( IMG_SIZE_MB * 2048 ))
    PART_SECTORS=$(( TOTAL_SECTORS - PART_START_LBA ))
    PART_OFFSET_BYTES=$(( PART_START_LBA * 512 ))
    PART_IMG="${TMP_DIR}/part1.fat32"

    truncate -s "$(( PART_SECTORS * 512 ))" "$PART_IMG"
    mkfs.vfat -F 32 -n "FUJISANSD2" "$PART_IMG" >/dev/null

    export MTOOLS_SKIP_CHECK=1
    mmd   -i "$PART_IMG" "::/FFDB"
    mmd   -i "$PART_IMG" "::/DCIM"
    mmd   -i "$PART_IMG" "::/DCIM/100_FJOS"
    mmd   -i "$PART_IMG" "::/banban"
    mmd   -i "$PART_IMG" "::/banban/fujisan"
    mmd   -i "$PART_IMG" "::/banban/plugins"
    mmd   -i "$PART_IMG" "::/banban/themes"

    mcopy -i "$PART_IMG" "${STAGE_DIR}/FFDB/FFXFER.DAT" "::/FFDB/FFXFER.DAT"
    [[ -f "${STAGE_DIR}/BANBANINJ" ]] && mcopy -i "$PART_IMG" "${STAGE_DIR}/BANBANINJ" "::/BANBANINJ"
    mcopy -i "$PART_IMG" "${STAGE_DIR}/banban/boot.cfg" "::/banban/boot.cfg"
    mcopy -i "$PART_IMG" "${STAGE_DIR}/banban/MANIFEST.TXT" "::/banban/MANIFEST.TXT"
    mcopy -i "$PART_IMG" "${STAGE_DIR}/banban/fujisan/zImage" "::/banban/fujisan/zImage"
    mcopy -i "$PART_IMG" "${STAGE_DIR}/banban/fujisan/${DTB_NAME}" "::/banban/fujisan/${DTB_NAME}"
    mcopy -i "$PART_IMG" "${STAGE_DIR}/banban/fujisan/initramfs.cpio.gz" "::/banban/fujisan/initramfs.cpio.gz"
    if (( MINIMAL_MODE == 0 )); then
        mcopy -i "$PART_IMG" "${STAGE_DIR}/banban/fujisan/rootfs.squashfs" "::/banban/fujisan/rootfs.squashfs"
    fi

    mkdir -p "$(dirname "$IMG_PATH")"
    rm -f "$IMG_PATH"
    truncate -s "$(( TOTAL_SECTORS * 512 ))" "$IMG_PATH"
    python3 - "$IMG_PATH" "$PART_START_LBA" "$PART_SECTORS" <<'PYEOF'
import struct, sys
path, start_lba, sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mbr = bytearray(512)
struct.pack_into("<I", mbr, 0x1B8, 0x464A534E)  # "FJSN" disk signature
struct.pack_into("<BBBBBBBBII", mbr, 0x1BE,
                 0x00, 0xFE, 0xFF, 0xFF, 0x0C, 0xFE, 0xFF, 0xFF,
                 start_lba, sectors)
mbr[510] = 0x55
mbr[511] = 0xAA
with open(path, "r+b") as f:
    f.seek(0)
    f.write(mbr)
PYEOF
    dd if="$PART_IMG" of="$IMG_PATH" bs=512 seek="$PART_START_LBA" conv=notrunc status=none
    echo "[sd2] Exported MBR+FAT32 image: ${IMG_PATH} (${IMG_SIZE_MB} MiB)"
    mdir -i "${IMG_PATH}@@${PART_OFFSET_BYTES}" "::/banban/fujisan"
fi

echo "[sd2] Fujisan OS SD2 packaging COMPLETE!"
