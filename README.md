> [!CAUTION]
> **EXPERIMENTAL AI-GENERATED CODE ("AI SLOP") — NO WARRANTY — USE AT YOUR OWN RISK.**
>
> This repository contains experimental, heavily AI-generated bootloader configurations, Linux kernel drivers, device trees, and low-level hardware code. **Running custom bootloaders, kernels, device trees, or hardware drivers on physical camera hardware can permanently brick the device, corrupt factory calibration or NVRAM, or cause hardware failure.** This project is provided strictly **"AS IS"** without warranty of any kind (see [LICENSE](LICENSE)). The authors and contributors accept **zero liability** for any device damage, malfunction, or data loss.

<p align="center">
  <img src="assets/logo.png" alt="Fujisan OS Logo" width="144" height="144" />
</p>

<h1 align="center">Fujisan OS</h1>

<p align="center">
  <strong>Clean-room pure-ARM32 Alpine Linux (<code>armv7</code>) for SPX4-MF medium-format camera platforms</strong>
</p>

**Fujisan OS** is a clean-room, pure-ARM32 Alpine Linux (`armv7`) operating system built for **SPX4-MF** medium-format camera platforms (**SPX4** quad-core ARM Cortex-A7 SoC + **IMX461** 102 MP CMOS sensor). It boots non-destructively via **`banban-injector` (`B:\BANBANINJ`)** from SD Card Slot 2 with **zero proprietary firmware binaries** in the repository and **strict read-only protection of NOR Flash factory calibration and BKUP partitions**.

## Highlights

- **`banban-injector` 100% Compatible**: Boots directly from `[entry:fujisan]` in `B:\banban\boot.cfg` with hard staging ceilings enforced in CI (`zImage <= 5.00 MiB`, `fujisan-spx4-mf.dtb <= 256 KiB`, `initramfs.cpio.gz <= 5.25 MiB`), pure ARM32 (`0` Thumb `$t` symbols), fixed FDT v17 fixup paths (`/chosen/bootargs`, `/chosen/linux,initrd-{start,end}`, `/chosen/framebuffer0/reg`, `/reserved-memory/framebuffer/reg`), and `/soc/serial@fff3a000` (`uart0`) `status = "disabled"` retail safety.
- **SPX4-MF Imaging & SoC BSP (`drivers/`, DT compatibles `fujisan,spx4-mf` / `fujisan,spx4-*`)**:
  - `fujisan-spx4-pm.c`: 4-layer cold-boot power/clock controller (`0xFFF42000` keyed `0xAA` PD `2/4/6-8/9/10/13` -> PLL `0xFF701000..C080` -> `0xFFF483xx` -> `0xFF70F0xx`).
  - `fujisan-imx461.c`: 102 MP IMX461 SPI command engine (`0xFFF6C200`), `0x004F bit 3` read-gate, sensor temperature (`0x0040/0x0041`), 25 mode tables, and GPIO power-on sequence.
  - `fujisan-slvsec.c`: Dual-bank 8-lane SLVS-EC RX (`0xFFF69000`/`0xFFF6A000`, IRQ `296`/`298`/`297`), SerDes PHY (`0xFFF6B000`), and CCDTG timing generator (`0xFFF6C000`).
  - `fujisan-ccdif-v4l2.c`: Semi-planar `Y+UV` (`NV12`) Live-View Ch0/1 (`0xFFF72000`/`0xFFF73000`), RAW DMA (`0xFFF6E000`), and 20-channel 3A/PDAF/ROB stats (`0xFFF70000`, 56 KiB SRAM `0xFFF10000`, 4-slot PDAF DDR ring `0x2C4B0000`).
  - `fujisan-lenscom.c`: Medium-format SPI0 Frame32 `v4l2-subdev` (`S12` sync `0xA55A3CC3 -> 0xC33CA55A`, 131B Bulk `0x014/0x015`, 128B Lens Identity Header, AF/iris drive).
  - `fujisan-mali-v61.c`: Mali-V61 (`0xFFE00000`, IRQ `138`) `NV12` (`0x0201`) / 10-bit `Y0L2` (`0x0121`) video codec with LSID 2-level MMU.
- **Alpine `armv7` Diskless / Overlay Rootfs & OpenRC Daemons (`rootfs/`)**:
  - Supports read-only `rootfs.squashfs` + persistent ext4 `overlay.img` or `fujisan.apkovl.tar.gz` on SD2, plus read-write SD1 `/mnt/sd1/DCIM/100_FJOS`.
  - OpenRC services: `fujisan-usbd` (DWC3 NCM/UVC/MTP gadget), `fujisan-shoulderd` (`304x230` Sharp MIP shoulder LCD), and `fujisan-powerd` (IMX461 thermal & safe shutdown monitor).
- **Freestanding Pure-ARM32 Userland Apps (`apps/`)**:
  - `fujisan-home`: `1024x768` touch/evdev launcher (zero libc/libgcc, `0` Thumb `$t`).
  - `fujisan-cam`: V4L2 `NV12`/`UYVY` live view, IMX461 exposure/temp, SPI0 `LENSCOM` AF/aperture telemetry, and baseline JFIF capture to SD1 `DCIM/100_FJOS/FJOSxxxx.JPG`.
  - `fujisan-term`: On-screen SPX4-MF hardware diagnostics terminal.
- **Clean-Room Mali-V61 RASC Extractor (`tools/extract-rasc-fw.py`)**: Extracts all 4 ARM MVE v2 RASC codec firmwares (`^ 0xFF` at `0x02C60274..0x02D60274`) from a user-supplied stock firmware image without bundling any proprietary binaries.

## Quick Build & Verification

```sh
# Full offline CI pipeline: host driver + userland tests, DTB (<= 256 KiB),
# pure-ARM32 0-Thumb apps, deterministic offline initramfs (<= 5.25 MiB),
# dual-format minimal SD2 image/directory, and config/brand/hygiene checks:
make all

# Individual targets:
make drivers-test     # Host C11 unit tests for SPX4 BSP + 6 imaging/SoC drivers
make userland-test    # Host C11 unit tests for fujisan-home, fujisan-cam, fujisan-term
make dtb              # Compile & size-check fujisan-spx4-mf.dtb (<= 256 KiB)
make apps             # Build pure-ARM32 (0-Thumb) fujisan-home, fujisan-cam, fujisan-term
make initramfs        # Build deterministic offline initramfs.cpio.gz (<= 5.25 MiB)
make sd2-minimal      # Build dual-format SD2 (.img + sd-card-slot-2/ directory)
make verify           # Shell syntax, extract-rasc-fw.py --selftest, brand & text hygiene
```

### Extract Mali-V61 RASC Firmwares (User-Supplied Stock Firmware Image)

```sh
python3 tools/extract-rasc-fw.py --firmware-dat /path/to/stock-firmware.dat \
    --out-dir build/out/mali-v61-fw
```

## Documentation

- [Architecture Specification](docs/architecture.md) — 3-layer separation, DDR map, 4-layer SPX4 power/clock tree, NOR Flash read-only safety, OpenRC services, and userland apps.
- [IMX461 CMOS & Imaging Pipeline](docs/cmos-pipeline.md) — Hardware register contracts for IMX461, SLVS-EC, CCDTG, CCDIF, 3A/PDAF stats, Live-View semi-planar `Y+UV`, `LENSCOM`, BIS shutter, and Mali-V61.
- [`banban-injector` Boot Contract](docs/banban-injector-boot.md) — SD2 layout, `[entry:fujisan]`, hard size budgets, FDT v17 fixups, and dual-format deployment.

## License

GNU General Public License v2.0 only (`GPL-2.0-only`). See [LICENSE](LICENSE).
