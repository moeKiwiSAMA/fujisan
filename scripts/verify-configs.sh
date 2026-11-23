#!/usr/bin/env bash
# verify-configs.sh — Static validation, script syntax, RASC extractor selftest & hygiene audit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$REPO_ROOT"

echo "[verify] 1. Checking bash / POSIX sh syntax..."
for f in \
    scripts/build-initramfs.sh \
    scripts/build-rootfs.sh \
    scripts/verify-configs.sh \
    scripts/build-sd2-fujisan.sh; do
    if [[ -f "$f" ]]; then
        bash -n "$f"
    fi
done

for f in \
    rootfs/initramfs/init \
    rootfs/overlay/etc/init.d/fujisan-usbd \
    rootfs/overlay/etc/init.d/fujisan-shoulderd \
    rootfs/overlay/etc/init.d/fujisan-powerd \
    rootfs/overlay/usr/local/bin/fujisan-reboot-stock \
    rootfs/overlay/usr/local/bin/fujisan-apkovl-save; do
    sh -n "$f"
done
echo "[verify]    Shell syntax OK."

echo "[verify] 2. Running clean-room Mali-V61 RASC extractor selftest..."
PYTHONDONTWRITEBYTECODE=1 python3 tools/extract-rasc-fw.py --selftest

echo "[verify] 3. Checking UTF-8 / no-BOM / LF / trailing-LF / brand-neutrality / zero-host-paths / zero-proprietary-binaries..."
python3 - <<'PYEOF'
import pathlib, re

root = pathlib.Path(".")
forbidden_host = "/home/" + "coder"
forbidden_brands = [
    "fuji" + "film",
    "gfx" + "100s",
    "gxup" + "0004",
]
forbidden_exts = {".dat", ".bin", ".img", ".squashfs", ".ko", ".o", ".so", ".dll", ".exe"}

checked = 0
for top in ["apps", "docs", "drivers", "dts", "kernel", "rootfs", "scripts", "tests", "tools", ".github"]:
    base = root / top
    if not base.exists():
        continue
    for p in sorted(base.rglob("*")):
        if not p.is_file() or "__pycache__" in p.parts:
            continue
        assert p.suffix.lower() not in forbidden_exts, f"Binary artifact tracked/placed in source tree: {p}"
        raw = p.read_bytes()
        assert not raw.startswith(b"\xef\xbb\xbf"), f"BOM found in {p}"
        assert b"\r" not in raw, f"CR found in {p}"
        assert raw.endswith(b"\n") and not raw.endswith(b"\n\n"), f"Trailing LF violation in {p}"
        txt = raw.decode("utf-8")
        assert forbidden_host not in txt, f"Hardcoded host path found in {p}"
        low = txt.lower()
        for tok in forbidden_brands:
            assert tok not in low, f"Brand token '{tok}' found in {p}"
        checked += 1

for f in [".gitignore", "LICENSE", "Makefile", "README.md"]:
    p = root / f
    if not p.is_file():
        continue
    raw = p.read_bytes()
    assert not raw.startswith(b"\xef\xbb\xbf"), f"BOM found in {p}"
    assert b"\r" not in raw, f"CR found in {p}"
    assert raw.endswith(b"\n") and not raw.endswith(b"\n\n"), f"Trailing LF violation in {p}"
    txt = raw.decode("utf-8")
    assert forbidden_host not in txt, f"Hardcoded host path found in {p}"
    low = txt.lower()
    for tok in forbidden_brands:
        assert tok not in low, f"Brand token '{tok}' found in {p}"
    checked += 1

# Verify zero relative broken markdown links across README.md and docs/*.md
link_re = re.compile(r'\[[^\]]+\]\(([^)#]+)(?:#[^)]*)?\)')
md_files = [root / "README.md"] + sorted((root / "docs").glob("*.md"))
links_checked = 0
for md in md_files:
    if not md.is_file():
        continue
    text = md.read_text(encoding="utf-8")
    for m in link_re.finditer(text):
        target = m.group(1).strip()
        if target.startswith(("http://", "https://", "mailto:", "git@")):
            continue
        resolved = (md.parent / target).resolve()
        assert resolved.exists(), f"Broken local link in {md}: {target}"
        links_checked += 1

print(f"  HYGIENE OK: {checked} files verified (UTF-8, no BOM, LF, brand-neutral, 0 host paths, 0 binaries, {links_checked} local md links OK).")
PYEOF

echo "[verify] 4. Checking git diff --check..."
git diff --check
echo "[verify] ALL CHECKS PASSED!"
