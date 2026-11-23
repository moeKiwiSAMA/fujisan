# Fujisan OS — SPX4-MF Medium-Format Camera (SPX4) Linux Build & Verification Makefile
#
# Targets:
#   make all           Full CI verification: drivers-test, userland-test, dtb,
#                      apps (0-Thumb ARM32), initramfs, sd2-minimal, verify
#   make drivers-test  Host C11 unit tests for SPX4 BSP + 6 DAT-verified drivers
#   make userland-test Host C11 unit tests for Fujisan userland apps
#   make dtb           Compile fujisan-spx4-mf.dtb (<= 256 KiB + FDT check)
#   make apps          Build freestanding pure-ARM32 (0-Thumb) home/cam/term
#   make initramfs     Build deterministic offline initramfs (<= 5.25 MiB)
#   make sd2-minimal   Build dual-format SD2 image + directory tree (--minimal)
#   make verify        Run shell syntax, RASC selftest, and repo hygiene audit
#   make clean         Remove build/ artifacts

CC ?= gcc
DTC ?= dtc
ARM32_CROSS_COMPILE ?= $(shell if command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then echo arm-linux-gnueabihf-; else echo arm-none-eabi-; fi)
ARM32_CC := $(ARM32_CROSS_COMPILE)gcc
ARM32_READELF := $(ARM32_CROSS_COMPILE)readelf

BUILD_DIR := build/ci
OUT_DIR := build/out

HOST_CFLAGS := -O2 -Wall -Wextra -Werror -std=c11 \
	-Idrivers/include -Iapps/common \
	-Iapps/fujisan-home -Iapps/fujisan-cam -Iapps/fujisan-term

ARM32_CFLAGS := -mcpu=cortex-a7 -marm -mno-thumb-interwork -mno-unaligned-access \
	-mgeneral-regs-only -nostdlib -ffreestanding -static -Os -Wall -Wextra -Werror \
	-fno-stack-protector -fno-asynchronous-unwind-tables \
	-fno-tree-loop-distribute-patterns \
	-Wl,--build-id=none -Wl,-static -Wl,-z,noexecstack

.PHONY: all test drivers-test userland-test dtb home cam term apps initramfs sd2-minimal verify clean

all: drivers-test userland-test dtb apps initramfs sd2-minimal verify

$(BUILD_DIR) $(OUT_DIR):
	@mkdir -p $@

drivers-test: $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) -o $(BUILD_DIR)/test_fujisan_drivers tests/test_fujisan_drivers.c
	./$(BUILD_DIR)/test_fujisan_drivers

userland-test: $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) -o $(BUILD_DIR)/test_fujisan_userland tests/test_fujisan_userland.c
	./$(BUILD_DIR)/test_fujisan_userland

test: drivers-test userland-test

dtb: $(BUILD_DIR)
	$(DTC) -Wno-unit_address_vs_reg -I dts -O dtb \
		-o $(BUILD_DIR)/fujisan-spx4-mf.dtb dts/fujisan-spx4-mf.dts
	@SZ=$$(wc -c < $(BUILD_DIR)/fujisan-spx4-mf.dtb); \
		echo "[dtb] Built $(BUILD_DIR)/fujisan-spx4-mf.dtb ($${SZ} B <= 262144 B)"; \
		test "$${SZ}" -le 262144
	@$(DTC) -I dtb -O dts $(BUILD_DIR)/fujisan-spx4-mf.dtb 2>/dev/null | \
		python3 -c 'import sys; s=sys.stdin.read(); \
		assert "linux,initrd-start" in s and "linux,initrd-end" in s; \
		assert "simple-framebuffer" in s and "framebuffer0" in s; \
		assert "reserved-memory" in s and "pdaf-ring@2c4b0000" in s; \
		assert "fujisan,spx4-pm" in s and "fujisan,imx461" in s; \
		assert "fujisan,spx4-slvsec" in s and "fujisan,spx4-ccdif" in s; \
		assert "fujisan,spx4-mf-lenscom" in s and "fujisan,spx4-mali-v61" in s; \
		print("[dtb] Verified BANBANINJ FDT v17 fixup nodes & all 6 Fujisan imaging/PM nodes OK.")'

define CHECK_ZERO_THUMB
	@if $(ARM32_READELF) -sW $(1) | awk '$$8 == "$$t" || $$8 ~ /^\$$t\./ { exit 1 }'; then \
		echo "[arm32] Verified pure ARM32 (0 Thumb \$$t symbols): $(1)"; \
	else \
		echo "ERROR: $(1) contains Thumb symbols!" >&2; exit 1; \
	fi
endef

home: $(BUILD_DIR)
	$(ARM32_CC) $(ARM32_CFLAGS) -Iapps/common -Iapps/fujisan-home \
		-o $(BUILD_DIR)/fujisan-home apps/fujisan-home/fujisan_home.c
	$(call CHECK_ZERO_THUMB,$(BUILD_DIR)/fujisan-home)

cam: $(BUILD_DIR)
	$(ARM32_CC) $(ARM32_CFLAGS) -Iapps/common -Iapps/fujisan-cam \
		-o $(BUILD_DIR)/fujisan-cam apps/fujisan-cam/fujisan_cam.c
	$(call CHECK_ZERO_THUMB,$(BUILD_DIR)/fujisan-cam)

term: $(BUILD_DIR)
	$(ARM32_CC) $(ARM32_CFLAGS) -Iapps/common -Iapps/fujisan-term \
		-o $(BUILD_DIR)/fujisan-term apps/fujisan-term/fujisan_term.c
	$(call CHECK_ZERO_THUMB,$(BUILD_DIR)/fujisan-term)

apps: home cam term

initramfs: $(OUT_DIR) apps
	./scripts/build-initramfs.sh --offline --output $(OUT_DIR)/initramfs.cpio.gz
	@SZ=$$(wc -c < $(OUT_DIR)/initramfs.cpio.gz); \
		echo "[initramfs] Verified $(OUT_DIR)/initramfs.cpio.gz ($${SZ} B <= 5505024 B)"; \
		test "$${SZ}" -le 5505024

sd2-minimal: dtb initramfs
	./scripts/build-sd2-fujisan.sh --minimal --dummy-zimage \
		--dtb $(BUILD_DIR)/fujisan-spx4-mf.dtb \
		--initramfs $(OUT_DIR)/initramfs.cpio.gz

verify:
	./scripts/verify-configs.sh

clean:
	rm -rf build
