#!/usr/bin/env python3
"""Clean-room Mali-V61 4-RASC firmware extractor for Fujisan OS.

Reads a user-supplied SPX4-MF stock firmware image (--firmware-dat),
extracts the 1 MiB bitwise-inverted (^ 0xFF) Mali-V61 RASC firmware bank
at file offset 0x02C60274..0x02D60274 (QSPI Flash 0x02C60000..0x02D60000),
validates ARM MVE v2 RASC headers, and writes the four codec firmware
binaries + manifest.json for the Linux fujisan-mali-v61 driver.

Zero proprietary firmware data is stored in this repository.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import tempfile
from dataclasses import dataclass

RASC_BANK_DAT_OFFSET = 0x02C60274
RASC_BANK_FLASH_OFFSET = 0x02C60000
RASC_BANK_RAM_VA = 0x00CA0000
RASC_BANK_SIZE = 0x00100000  # 8 x 128 KiB = 1 MiB

RASC_MAGIC_0 = 0x0000EB5E
RASC_MAGIC_1 = 0x00000204


@dataclass(frozen=True)
class RascSlot:
    name: str
    filename: str
    codec: str
    direction: str
    rel_offset: int
    slot_size: int
    expected_id_prefix: str
    expected_text_len: int
    expected_bss_start: int
    expected_bss_pages: int
    expected_shared_start: int
    expected_shared_size: int

    @property
    def dat_offset(self) -> int:
        return RASC_BANK_DAT_OFFSET + self.rel_offset

    @property
    def flash_offset(self) -> int:
        return RASC_BANK_FLASH_OFFSET + self.rel_offset

    @property
    def ram_va(self) -> int:
        return RASC_BANK_RAM_VA + self.rel_offset


RASC_SLOTS: tuple[RascSlot, ...] = (
    RascSlot(
        name="hevc_enc",
        filename="mali-v61-hevc-enc.bin",
        codec="hevc",
        direction="encode",
        rel_offset=0x00000,
        slot_size=0x39000,
        expected_id_prefix="56610002H.265",
        expected_text_len=0x38080,
        expected_bss_start=0x3A000,
        expected_bss_pages=18,
        expected_shared_start=0x45000,
        expected_shared_size=0x62D0,
    ),
    RascSlot(
        name="hevc_dec",
        filename="mali-v61-hevc-dec.bin",
        codec="hevc",
        direction="decode",
        rel_offset=0x39000,
        slot_size=0x2F000,
        expected_id_prefix="56610002HEVC",
        expected_text_len=0x2EA00,
        expected_bss_start=0x30000,
        expected_bss_pages=57,
        expected_shared_start=0x3C000,
        expected_shared_size=0x2C638,
    ),
    RascSlot(
        name="h264_enc",
        filename="mali-v61-h264-enc.bin",
        codec="h264",
        direction="encode",
        rel_offset=0x68000,
        slot_size=0x39000,
        expected_id_prefix="56610002H.264",
        expected_text_len=0x38F00,
        expected_bss_start=0x3A000,
        expected_bss_pages=18,
        expected_shared_start=0x46000,
        expected_shared_size=0x5EAC,
    ),
    RascSlot(
        name="h264_dec",
        filename="mali-v61-h264-dec.bin",
        codec="h264",
        direction="decode",
        rel_offset=0xA1000,
        slot_size=0x5F000,
        expected_id_prefix="56610002H.264",
        expected_text_len=0x43200,
        expected_bss_start=0x45000,
        expected_bss_pages=37,
        expected_shared_start=0x50000,
        expected_shared_size=0x19FFC,
    ),
)


def invert_bytes(buf: bytes) -> bytes:
    """Bitwise-invert stock firmware Flash bytes (byte ^ 0xFF)."""
    table = bytes(range(255, -1, -1))
    return buf.translate(table)


def parse_rasc_header(blob: bytes) -> dict[str, object]:
    """Parse key ARM MVE v2 RASC firmware header fields."""
    if len(blob) < 0xC0:
        raise ValueError(f"RASC blob too small ({len(blob)} bytes)")
    m0, m1 = struct.unpack_from("<II", blob, 0x00)
    if m0 != RASC_MAGIC_0 or m1 != RASC_MAGIC_1:
        raise ValueError(
            f"Invalid RASC magic: got 0x{m0:08X} 0x{m1:08X}, "
            f"expected 0x{RASC_MAGIC_0:08X} 0x{RASC_MAGIC_1:08X}"
        )

    ascii_run = bytearray()
    for b in blob[0x08:0x60]:
        if 0x20 <= b <= 0x7E:
            ascii_run.append(b)
        elif ascii_run:
            break
    fw_id = ascii_run.decode("ascii", errors="replace").strip()

    text_len = struct.unpack_from("<I", blob, 0x60)[0]
    bss_start = struct.unpack_from("<I", blob, 0x64)[0]
    bss_pages = struct.unpack_from("<I", blob, 0x68)[0]
    bss_bitmap = struct.unpack_from("<I", blob, 0x6C)[0]
    shared_start = struct.unpack_from("<I", blob, 0xAC)[0]
    shared_size = struct.unpack_from("<I", blob, 0xB0)[0]

    return {
        "magic": [f"0x{m0:08X}", f"0x{m1:08X}"],
        "firmware_id": fw_id,
        "text_len": text_len,
        "bss_start": bss_start,
        "bss_pages": bss_pages,
        "bss_bitmap": f"0x{bss_bitmap:08X}",
        "shared_start": shared_start,
        "shared_size": shared_size,
        "size_bytes": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest(),
    }


def extract_from_dat(
    dat_path: pathlib.Path,
    out_dir: pathlib.Path,
    *,
    strict_id: bool = True,
    dry_run: bool = False,
) -> dict[str, object]:
    file_size = dat_path.stat().st_size
    min_size = RASC_BANK_DAT_OFFSET + RASC_BANK_SIZE
    if file_size < min_size:
        raise ValueError(
            f"{dat_path} is too small ({file_size} B < required {min_size} B)"
        )

    with dat_path.open("rb") as fh:
        fh.seek(RASC_BANK_DAT_OFFSET)
        raw_inverted = fh.read(RASC_BANK_SIZE)
    if len(raw_inverted) != RASC_BANK_SIZE:
        raise IOError("Short read from firmware RASC bank")

    bank = invert_bytes(raw_inverted)
    if not dry_run:
        out_dir.mkdir(parents=True, exist_ok=True)

    entries: list[dict[str, object]] = []
    for slot in RASC_SLOTS:
        blob = bank[slot.rel_offset : slot.rel_offset + slot.slot_size]
        hdr = parse_rasc_header(blob)
        fw_id = str(hdr["firmware_id"])
        if strict_id and not fw_id.startswith(slot.expected_id_prefix):
            raise ValueError(
                f"Slot {slot.name} ID mismatch: got {fw_id!r}, "
                f"expected prefix {slot.expected_id_prefix!r}"
            )
        if not dry_run:
            (out_dir / slot.filename).write_bytes(blob)
        entries.append(
            {
                "name": slot.name,
                "filename": slot.filename,
                "codec": slot.codec,
                "direction": slot.direction,
                "dat_offset": f"0x{slot.dat_offset:08X}",
                "flash_offset": f"0x{slot.flash_offset:08X}",
                "ram_va": f"0x{slot.ram_va:08X}",
                "slot_size": slot.slot_size,
                **hdr,
            }
        )

    manifest = {
        "schema": "fujisan-mali-v61-rasc-v1",
        "source_file": dat_path.name,
        "source_bank_dat_offset": f"0x{RASC_BANK_DAT_OFFSET:08X}",
        "source_bank_size": RASC_BANK_SIZE,
        "bank_sha256": hashlib.sha256(bank).hexdigest(),
        "firmwares": entries,
    }
    if not dry_run:
        (out_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
    return manifest


def run_selftest() -> int:
    """Verify extraction, ^0xFF inversion, header parsing, and manifest generation."""
    total = sum(s.slot_size for s in RASC_SLOTS)
    assert total == RASC_BANK_SIZE, f"Slot sum {total:#x} != {RASC_BANK_SIZE:#x}"

    bank = bytearray(RASC_BANK_SIZE)
    for slot in RASC_SLOTS:
        off = slot.rel_offset
        struct.pack_into("<II", bank, off + 0x00, RASC_MAGIC_0, RASC_MAGIC_1)
        id_bytes = (slot.expected_id_prefix + " SELFTEST").encode("ascii")
        bank[off + 0x08 : off + 0x08 + len(id_bytes)] = id_bytes
        struct.pack_into(
            "<IIII",
            bank,
            off + 0x60,
            slot.expected_text_len,
            slot.expected_bss_start,
            slot.expected_bss_pages,
            0x0003FBF9,
        )
        struct.pack_into(
            "<II",
            bank,
            off + 0xAC,
            slot.expected_shared_start,
            slot.expected_shared_size,
        )

    inverted_bank = invert_bytes(bytes(bank))
    with tempfile.TemporaryDirectory(prefix="fujisan-rasc-test-") as tmp:
        tmp_path = pathlib.Path(tmp)
        fake_dat = tmp_path / "stock-firmware.dat"
        out_dir = tmp_path / "out"
        with fake_dat.open("wb") as fh:
            fh.seek(RASC_BANK_DAT_OFFSET)
            fh.write(inverted_bank)

        manifest = extract_from_dat(fake_dat, out_dir, strict_id=True, dry_run=False)
        fws = manifest["firmwares"]
        assert isinstance(fws, list) and len(fws) == 4
        for slot, entry in zip(RASC_SLOTS, fws):
            out_file = out_dir / slot.filename
            assert out_file.is_file(), f"Missing {out_file}"
            assert out_file.stat().st_size == slot.slot_size
            assert entry["text_len"] == slot.expected_text_len
            assert entry["bss_start"] == slot.expected_bss_start
            assert entry["bss_pages"] == slot.expected_bss_pages
            assert entry["shared_start"] == slot.expected_shared_start
            assert entry["shared_size"] == slot.expected_shared_size
        assert (out_dir / "manifest.json").is_file()

    print("[extract-rasc-fw] selftest PASS (4/4 Mali-V61 RASC slots verified).")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Clean-room Mali-V61 4-RASC extractor from user-supplied stock firmware image"
    )
    parser.add_argument(
        "--firmware-dat",
        "--dat",
        dest="firmware_dat",
        type=pathlib.Path,
        help="Path to user-supplied SPX4-MF stock firmware image",
    )
    parser.add_argument(
        "--out-dir",
        type=pathlib.Path,
        default=pathlib.Path("build/out/mali-v61-fw"),
        help="Output directory for extracted RASC firmware binaries",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate firmware RASC bank without writing files",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="Run built-in self-test using synthetic RASC headers",
    )
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if args.firmware_dat is None:
        parser.error("either --firmware-dat <image.dat> or --selftest is required")

    manifest = extract_from_dat(args.firmware_dat, args.out_dir, dry_run=args.dry_run)
    for fw in manifest["firmwares"]:  # type: ignore[union-attr]
        print(
            f"[extract-rasc-fw] {fw['filename']:24s} "
            f"dat={fw['dat_offset']} size={fw['slot_size']:6d} B "
            f"text=0x{fw['text_len']:x} id={fw['firmware_id']!r}"
        )
    if not args.dry_run:
        print(f"[extract-rasc-fw] Wrote 4 binaries + manifest.json to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
