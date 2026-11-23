# Fujisan OS — `banban-injector` (`B:\BANBANINJ`) Boot Contract

Fujisan OS is 100% compatible with **`banban-injector` (`B:\BANBANINJ`)** on both SPX4-MF hardware and SPX4 emulation environments (supporting both raw/qcow2 `.img` and host-folder `directory` SD Slot 2 mounting).

## 1. SD Card Slot 2 (`B:\`) Layout

```text
B:\
├── FFDB\
│   └── FFXFER.DAT                       # Transfer DB marker (1024 B)
├── DCIM\
│   └── 100_FJOS\                        # Standard DCF directory
├── BANBANINJ                            # Layer-1 Stage-2 bootloader (<= 192 KiB)
└── banban\
    ├── boot.cfg                         # Multi-boot menu config with [entry:fujisan]
    └── fujisan\
        ├── zImage                       # Pure-ARM32 Linux kernel (<= 5.00 MiB)
        ├── fujisan-spx4-mf.dtb          # Compiled FDT v17 DTB (<= 256 KiB)
        ├── initramfs.cpio.gz            # Stage-1 initramfs (<= 5.25 MiB)
        ├── rootfs.squashfs              # Optional Alpine armv7 read-only rootfs
        ├── overlay.img                  # Optional ext4 persistent writable overlay
        └── fujisan.apkovl.tar.gz        # Optional Alpine diskless config overlay
```

## 2. Hard Staging Budgets (Enforced by `Makefile` & CI)

| Artifact | Hard Ceiling | Pre-Takeover Staging PA | Final Linux PA |
| --- | ---: | --- | --- |
| `zImage` | **`5.00 MiB`** (`5,242,880 B`) | `0x2A69C000..0x2AB9BFFF` | `0x02800000` |
| `fujisan-spx4-mf.dtb` | **`256 KiB`** (`262,144 B`) | `0x2AB9C000..0x2ABDBFFF` | `0x03000000` (`r2`) |
| `initramfs.cpio.gz` | **`5.25 MiB`** (`5,505,024 B`) | `0x2B69C000..0x2BBDBFFF` | `0x03100000` |

## 3. `B:\banban\boot.cfg` Entry

```ini
menu=1
default=fujisan
splash_ms=1800
uart=0

[entry:fujisan]
title=Fujisan OS (SPX4-MF Linux)
mode=alpine
kernel=B:\banban\fujisan\zImage
dtb=B:\banban\fujisan\fujisan-spx4-mf.dtb
initrd=B:\banban\fujisan\initramfs.cpio.gz
cmdline=console=tty0 quiet root=/dev/ram0

[entry:stock]
title=Stock Camera Firmware
mode=stock
```

## 4. Required FDT v17 In-Place Fixup Paths

Before jumping to `0x02800000` (`r0 = 0`, `r1 = 0xFFFFFFFF`, `r2 = 0x03000000`, ARM32 SVC `0xD3`, MMU/caches/GIC off, Thumb `T = 0`), `BANBANINJ` patches `fujisan-spx4-mf.dtb` in-place at these fixed paths:

1. `/chosen/bootargs`: Pre-padded string placeholder patched from `boot.cfg` `cmdline`.
2. `/chosen/linux,initrd-start` & `/chosen/linux,initrd-end`: Patched to `<0x03100000>` and `<0x03100000 + initrd_size>`.
3. `/chosen/framebuffer0/reg`: Patched to `<fb_pa 0x00300000>` using live `LCDC1` active surface PA (`*(volatile u32 *)0xFFFA9044`, `1024x768`, stride `4096`, `format = "r8g8b8a8"`, `A = 0xFF`).
4. `/reserved-memory/framebuffer/reg`: Patched to match `/chosen/framebuffer0/reg`.
5. `/soc/serial@fff3a000` (`uart0`): Must keep `status = "disabled"` for retail hardware safety (unclocked `UART0` triggers an AXI bus abort on retail units).

## 5. Dual-Format SD2 Build

`scripts/build-sd2-fujisan.sh` exports both:
- `build/out/sd2-fujisan.img`: MBR + FAT32 image for physical SD cards or `raw`/`qcow2` emulator mounting.
- `build/out/sd-card-slot-2/`: Ready-to-drop host folder tree (zero symlinks, legal FAT32 names) for directory-backed SD Slot 2 mounting.
