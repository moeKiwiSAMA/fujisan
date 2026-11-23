#!/usr/bin/env bash
# build-rootfs.sh — Build Fujisan OS Alpine armv7 rootfs.squashfs + apkovl / overlay.img
#
# Modes:
#   Default (online): Downloads Alpine 3.21.10 armv7 minirootfs, overlays
#                     rootfs/overlay/ (OpenRC services fujisan-usbd, fujisan-shoulderd,
#                     fujisan-powerd) + freestanding pure-ARM32 apps, and packs
#                     build/out/rootfs.squashfs + optional fujisan.apkovl.tar.gz.
#   --offline-apkovl: Packs deterministic rootfs/overlay/ into
#                     build/out/fujisan.apkovl.tar.gz with zero network access.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DL_DIR="${REPO_ROOT}/.tools/downloads"
STAGE_DIR="${REPO_ROOT}/build/rootfs-stage"
OUT_DIR="${REPO_ROOT}/build/out"
OUT_SQUASHFS="${OUT_DIR}/rootfs.squashfs"
OUT_APKOVL="${OUT_DIR}/fujisan.apkovl.tar.gz"
OFFLINE_APKOVL=0

ALPINE_VER="3.21.10"
ALPINE_TAR="alpine-minirootfs-${ALPINE_VER}-armv7.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/${ALPINE_TAR}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) OUT_SQUASHFS="$2"; shift 2 ;;
        --apkovl) OUT_APKOVL="$2"; shift 2 ;;
        --offline-apkovl) OFFLINE_APKOVL=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--offline-apkovl] [--output build/out/rootfs.squashfs] [--apkovl build/out/fujisan.apkovl.tar.gz]"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "$OUT_DIR"

if (( OFFLINE_APKOVL == 1 )); then
    rm -rf "$STAGE_DIR"
    mkdir -p "$STAGE_DIR"
    cp -a "${REPO_ROOT}/rootfs/overlay/." "$STAGE_DIR/"
    chmod 0755 "${STAGE_DIR}/etc/init.d/fujisan-usbd" \
               "${STAGE_DIR}/etc/init.d/fujisan-shoulderd" \
               "${STAGE_DIR}/etc/init.d/fujisan-powerd" \
               "${STAGE_DIR}/usr/local/bin/fujisan-reboot-stock" \
               "${STAGE_DIR}/usr/local/bin/fujisan-apkovl-save"
    mkdir -p "${STAGE_DIR}/etc/runlevels/default"
    ln -sf /etc/init.d/fujisan-usbd "${STAGE_DIR}/etc/runlevels/default/fujisan-usbd"
    ln -sf /etc/init.d/fujisan-shoulderd "${STAGE_DIR}/etc/runlevels/default/fujisan-shoulderd"
    ln -sf /etc/init.d/fujisan-powerd "${STAGE_DIR}/etc/runlevels/default/fujisan-powerd"
    tar --sort=name --mtime='UTC 1970-01-01' --owner=0 --group=0 --numeric-owner \
        -cf - -C "$STAGE_DIR" . | gzip -9n > "$OUT_APKOVL"
    echo "[rootfs] Packed offline apkovl: ${OUT_APKOVL} ($(wc -c < "$OUT_APKOVL") B)"
    exit 0
fi

for tool in curl tar mksquashfs; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: missing required tool '$tool'" >&2
        exit 1
    fi
done

mkdir -p "$DL_DIR" "$(dirname "$OUT_SQUASHFS")"
if [[ ! -f "${DL_DIR}/${ALPINE_TAR}" ]]; then
    echo "[rootfs] Downloading ${ALPINE_TAR}..."
    curl -fsSL --connect-timeout 15 --max-time 120 --retry 5 --retry-delay 2 \
        -o "${DL_DIR}/${ALPINE_TAR}" "${ALPINE_URL}"
fi

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
tar -xzf "${DL_DIR}/${ALPINE_TAR}" -C "$STAGE_DIR"
cp -a "${REPO_ROOT}/rootfs/overlay/." "$STAGE_DIR/"

chmod 0755 "${STAGE_DIR}/etc/init.d/fujisan-usbd" \
           "${STAGE_DIR}/etc/init.d/fujisan-shoulderd" \
           "${STAGE_DIR}/etc/init.d/fujisan-powerd" \
           "${STAGE_DIR}/usr/local/bin/fujisan-reboot-stock" \
           "${STAGE_DIR}/usr/local/bin/fujisan-apkovl-save"

mkdir -p "${STAGE_DIR}/etc/runlevels/default" "${STAGE_DIR}/lib/firmware/fujisan/mali-v61"
ln -sf /etc/init.d/fujisan-usbd "${STAGE_DIR}/etc/runlevels/default/fujisan-usbd"
ln -sf /etc/init.d/fujisan-shoulderd "${STAGE_DIR}/etc/runlevels/default/fujisan-shoulderd"
ln -sf /etc/init.d/fujisan-powerd "${STAGE_DIR}/etc/runlevels/default/fujisan-powerd"

printf 'fujisan\n' > "${STAGE_DIR}/etc/hostname"
printf '127.0.0.1\tlocalhost fujisan\n' > "${STAGE_DIR}/etc/hosts"
cat > "${STAGE_DIR}/etc/os-release" <<'EOF'
NAME="Fujisan OS"
ID=fujisan
ID_LIKE=alpine
PRETTY_NAME="Fujisan OS (Alpine armv7 for SPX4-MF)"
EOF

for app in fujisan-home fujisan-cam fujisan-term; do
    if [[ -f "${REPO_ROOT}/build/ci/${app}" ]]; then
        install -m 0755 "${REPO_ROOT}/build/ci/${app}" "${STAGE_DIR}/usr/local/bin/${app}"
    fi
done

mksquashfs "$STAGE_DIR" "$OUT_SQUASHFS" -comp gzip -noappend -all-root
echo "[rootfs] Built ${OUT_SQUASHFS}: $(wc -c < "$OUT_SQUASHFS") B"
