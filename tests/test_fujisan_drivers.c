// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — Host C11 Driver Unit Test Suite
 *
 * Compiles with: gcc -O2 -Wall -Wextra -Werror -std=c11 -Idrivers/include
 * Tests:
 *   1. Base SPX4 BSP codecs (Power-Peer SPI1, SAR ADC 0x555/0xAAA, soc-keyed-io,
 *      JPEG enc/dec, VGLIB DMP PICA packets, LCDC1 clock gate/RMW, Timer Ch7-only)
 *   2. NEW 4-Layer Imaging PM/Clock Tree (PD 2/4/6-8/9/10/13, Layer-3->Layer-4
 *      power-on ordering & strict reverse power-off ordering, PLL dividers)
 *   3. NEW Sony IMX461 102MP CMOS (SPI [0x81]/[0x82] framing, 0x004F read-gate,
 *      sensor temp 0x0040/41, VMAX/HMAX/SHR packing, all 25 DAT mode tables)
 *   4. NEW Dual-Bank 8-Lane SLVS-EC RX + PHY + CCDTG (active-low lane mask,
 *      CCDTG C024 one-hot route, H/V timing, IRQs 296/297/298/100/102)
 *   5. NEW CCDIF RAW DMA + Semi-Planar Y+UV LV + 20-Ch 3A/PDAF Stats Engine
 *      (holes [0x108..0x120] & [0x2D4..0x2E0] unblocked, 2x28 KiB SRAM banks,
 *      4-slot 600 KiB PDAF DDR ring, 34-bit AXI high-2 split)
 *   6. NEW GF-Mount LENSCOM SPI0 Frame32 (S12 sentinel, 128B identity header,
 *      AF/Iris command packing)
 *   7. NEW ARM Mali-V61 4K VPU (72B mve_buffer_frame NV12 0x0201 & 10-bit Y0L2
 *      0x0121 stride*4, RASC magic check, LSID 2-level MMU PTE encoding)
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fujisan_spx4_hw.h"

/* Ensure 72-byte mve_buffer_frame matches DAT @ S4 0x01205840 exactly */
_Static_assert(sizeof(struct fujisan_mve_buffer_frame) == 72U,
	       "mve_buffer_frame must be exactly 72 bytes (0x48)");

static void test_base_spx4_bsp(void)
{
	uint8_t tx[4], rx[4];
	uint16_t val = 0, lo16 = 0, hi8 = 0;
	int32_t decidegc = 0;
	uint8_t nv[8] = { 0x12, 0x34, 0x56, 0x78, 0x20, 0x05, SPX4_PWR_NVMEM_MAGIC_AA, 0x00 };
	uint32_t lo = 0, hi = 0;
	uint32_t s0[24], s1[24], s0_len = 0, s1_len = 0;

	/* Power-Peer SPI1 */
	spx4_power_encode_frame(tx, SPX4_PWR_SEL_CMD, true, SPX4_PWR_CMD_POWER_OFF);
	assert(tx[0] == 0x91U && tx[1] == 0x08U && tx[2] == 0x00U);
	assert(spx4_power_verify_frame(tx));

	rx[0] = SPX4_PWR_SEL_BATT_TEMP_DK;
	rx[1] = 0x0BU;
	rx[2] = 0xA6U;
	rx[3] = spx4_power_checksum(rx[0], rx[1], rx[2]);
	assert(spx4_power_decode_response(rx, &val) && val == 2982U);
	assert(spx4_power_temp_dk_to_decidegc(val, &decidegc) && decidegc == 250);
	assert(!spx4_power_temp_dk_to_decidegc(0xFFFFU, &decidegc));

	nv[7] = spx4_power_nvmem_checksum(nv);
	assert(spx4_power_nvmem_verify(nv));
	spx4_power_pack_wake_countdown(0x00123456U, &lo16, &hi8);
	assert(spx4_power_unpack_wake_countdown(lo16, hi8) == 0x00123456U);

	/* SAR ADC & Keyed-IO separation */
	assert((SPX4_ADC_SCAN_MASK_EVEN & SPX4_ADC_SCAN_MASK_ODD) == 0U);
	assert((SPX4_ADC_SCAN_MASK_EVEN | SPX4_ADC_SCAN_MASK_ODD) == SPX4_ADC_SCAN_MASK_ALL);
	assert(spx4_adc_decode_mode_dial(300U) == SPX4_MODE_M);
	assert(spx4_adc_decode_mode_dial(4000U) == SPX4_MODE_A);
	assert(spx4_keyed_io_cmd_valid(SPX4_KEYED_IO_CMD_KEY1));
	assert(spx4_keyed_io_cmd_valid(SPX4_KEYED_IO_CMD_KEY2 | SPX4_KEYED_IO_CMD_FLAG_B31));
	assert(!spx4_keyed_io_cmd_valid(0x0000AA03U));

	/* JPEG + VGLIB + LCDC1 + Timer Ch7 */
	spx4_dma34_split(0x0000000312345678ULL, &lo, &hi);
	assert(lo == 0x12345678U && hi == 3U);
	assert(spx4_jpeg_pack_dim(1024U, 768U) == 0x03000400U);
	assert(spx4_vglib_build_solid_rect(s0, 24, &s0_len, s1, 24, &s1_len,
					   0x059ED000U, 1024U, 768U, 0xFF112233U, 1U));
	assert((s0_len & 1U) == 0U && (s1_len & 1U) == 0U);
	assert(spx4_lcdc_clk_gate_rmw(0x00000105U, 1U) == 0x00000205U);
	assert((spx4_lcdc_plane_en_rmw(0x110U, true) & SPX4_LCDC_PLANE_ACTIVE_BIT) != 0U);
	assert((spx4_timer_ch7_ctrl_val(true) & SPX4_TIMER_CH0_6_FORBIDDEN) == 0U);

	printf("[PASS] 1. Base SPX4 BSP codecs (Power-Peer, ADC, Keyed-IO, JPEG, VGLIB, LCDC1, Timer Ch7)\n");
}

static void test_pm_4layer_power_tree(void)
{
	struct fujisan_pm_domain_desc d;
	uint32_t active = 0U;
	uint32_t rst = 0U, axi = 0U;
	static const uint8_t verified_pds[] = {
		FUJISAN_PD_CMOS_SLVSEC_CCDIF,
		FUJISAN_PD_LCDC,
		FUJISAN_PD_IIFP_0,
		FUJISAN_PD_UNIRESZ_JPEG_STILL,
		FUJISAN_PD_MALI_V61,
		FUJISAN_PD_RECC,
	};
	size_t i;

	for (i = 0; i < sizeof(verified_pds); i++) {
		assert(fujisan_pm_get_domain_desc(verified_pds[i], &d));
		assert(d.pd_mask != 0U);
		assert(d.rstclk_parent != 0U);
		assert(d.axiclk_parent != 0U);
		assert(d.pll_div_pa >= 0xFF701000U && d.pll_div_pa < 0xFF70D000U);
	}
	assert(!fujisan_pm_get_domain_desc(99U, &d));

	/* Simulate CMOS PD 2 cold-boot Power-ON: Layer 1 -> Layer 3 BEFORE Layer 4 */
	assert(fujisan_pm_get_domain_desc(FUJISAN_PD_CMOS_SLVSEC_CCDIF, &d));
	assert(d.pd_mask == (1U << 2));
	active = spx4_keyed_io_apply_commit(active, d.pd_mask, 0U);
	assert(active == (1U << 2));
	rst |= d.rstclk_parent;
	assert((axi & d.axiclk_parent) == 0U); /* Layer 3 set BEFORE Layer 4 */
	axi |= d.axiclk_parent;
	rst |= d.rstclk_sub;
	axi |= d.axiclk_sub;
	assert(rst == (d.rstclk_parent | d.rstclk_sub));
	assert(axi == (d.axiclk_parent | d.axiclk_sub));

	/* Strict reverse Power-OFF: Layer 4 BEFORE Layer 3 -> PD OFF */
	axi &= ~d.axiclk_sub;
	rst &= ~d.rstclk_sub;
	axi &= ~d.axiclk_parent;
	rst &= ~d.rstclk_parent;
	active = spx4_keyed_io_apply_commit(active, 0U, d.pd_mask);
	assert(rst == 0U && axi == 0U && active == 0U);

	/* IIFP spans 3 power domains (bits 6, 7, 8) */
	assert(fujisan_pm_get_domain_desc(FUJISAN_PD_IIFP_0, &d));
	assert(d.pd_mask == ((1U << 6) | (1U << 7) | (1U << 8)));

	/* PLL divider word helper */
	assert(fujisan_pll_div_word(0x12U, true) == 0x112U);
	assert(fujisan_pll_div_word(0x12U, false) == 0x012U);

	printf("[PASS] 2. 4-Layer Imaging PM/Clock tree (PD 2/4/6-8/9/10/13, 0xFFF483xx->0xFF70F0xx)\n");
}

static void test_imx461_sensor_and_25_modes(void)
{
	uint8_t pkt[4], shr[3];
	uint8_t lo = 0, hi = 0;
	struct fujisan_imx461_mode_desc m;
	uint8_t i;

	/* SPI [0x81, hi, lo, val] write & [0x82, hi, lo, 0] read framing */
	fujisan_imx461_pack_write(pkt, FUJISAN_IMX461_REG_READ_GATE, FUJISAN_IMX461_READ_GATE_OPEN);
	assert(pkt[0] == 0x81U && pkt[1] == 0x00U && pkt[2] == 0x4FU && pkt[3] == 0x08U);

	fujisan_imx461_pack_read(pkt, FUJISAN_IMX461_REG_MDSEL);
	assert(pkt[0] == 0x82U && pkt[1] == 0x00U && pkt[2] == 0x01U && pkt[3] == 0x00U);

	assert(fujisan_imx461_xfer_cfg(false, 4U, 1U) == 0x00010401U);
	assert(fujisan_imx461_xfer_cfg(true, 4U, 1U) == 0x00030401U);

	/* Sensor temperature 0x0040/0x0041 LE decode */
	assert(fujisan_imx461_decode_temp_raw(0x34U, 0x12U) == 0x1234U);

	/* Dynamic VMAX (12b), HMAX (15b), SHR (24b) */
	fujisan_imx461_pack_vmax(0x0ABCU, &lo, &hi);
	assert(lo == 0xBCU && hi == 0x0AU);
	fujisan_imx461_pack_hmax(0x5678U, &lo, &hi);
	assert(lo == 0x78U && hi == 0x56U);
	fujisan_imx461_pack_shr(0x00123456U, shr);
	assert(shr[0] == 0x56U && shr[1] == 0x34U && shr[2] == 0x12U);

	/* All 25 DAT mode tables */
	for (i = 0; i < FUJISAN_IMX461_NUM_MODES; i++) {
		assert(fujisan_imx461_get_mode(i, &m));
		assert(m.table_idx == i);
		assert(m.width == 11760U || m.width == 3920U || m.width == 3922U);
		assert(m.bpp == 16U || m.bpp == 14U || m.bpp == 12U);
	}
	assert(!fujisan_imx461_get_mode(25U, &m));

	/* Verify T[0] 102MP 16-bit Still & T[23] boot default */
	assert(fujisan_imx461_get_mode(0, &m));
	assert(m.mdsel == 0x00U && m.bpp == 16U && m.ccdtg_b4 == 4U && m.ccdtg_b5 == 3U);
	assert(fujisan_imx461_get_mode(23, &m));
	assert(m.mdsel == 0xB1U && m.width == 11760U);

	/* Cold power-on GPIO indices (bank*16 + pin) */
	assert(FUJISAN_IMX461_GPIO_XCLR == 69U);
	assert(FUJISAN_IMX461_GPIO_RAIL_A == 71U);
	assert(FUJISAN_IMX461_GPIO_RAIL_B == 194U);
	assert(FUJISAN_IMX461_GPIO_RST_PULSE == 15U);
	assert(FUJISAN_IMX461_RST_PULSE_MS == 10U);

	printf("[PASS] 3. IMX461 SPI [0x81]/[0x82], 0x004F read-gate, temp 0x0040/41, 25 DAT modes\n");
}

static void test_slvsec_and_ccdtg(void)
{
	/* Active-low 8-lane power-down mask: all 8 lanes enabled => 0x00; 4 lanes => 0xF0 */
	assert(fujisan_slvsec_lane_pdn_mask(0xFFU) == 0x00U);
	assert(fujisan_slvsec_lane_pdn_mask(0x0FU) == 0xF0U);
	assert(fujisan_slvsec_lane_pdn_mask(0x00U) == 0xFFU);

	/* CCDTG C024 one-hot [19:16] = 1<<(b4-1), [7:0] = b5 */
	assert(fujisan_ccdtg_route_word(4U, 3U) == 0x00080003U);
	assert(fujisan_ccdtg_route_word(3U, 2U) == 0x00040002U);
	assert(fujisan_ccdtg_route_word(2U, 1U) == 0x00020001U);

	/* CCDTG C00C H/V timing word */
	assert(fujisan_ccdtg_hv_word(11760U, 8754U) == ((8754U << 16) | 11760U));

	/* Dual-bank physical windows & GIC IRQs */
	assert(FUJISAN_SLVSEC_BANK0_PHYS == 0xFFF69000U);
	assert(FUJISAN_SLVSEC_BANK1_PHYS == 0xFFF6A000U);
	assert(FUJISAN_SLVSEC_PHY_PHYS == 0xFFF6B000U);
	assert(FUJISAN_CCDTG_PHYS == 0xFFF6C000U);
	assert(FUJISAN_IRQ_SLVSEC_CH0 == 296U && FUJISAN_IRQ_SLVSEC_ERR == 297U &&
	       FUJISAN_IRQ_SLVSEC_CH1 == 298U);

	printf("[PASS] 4. Dual-Bank 8-Lane SLVS-EC RX + PHY + CCDTG routing & IRQs 296/297/298\n");
}

static void test_ccdif_lv_raw_and_pdaf_stats(void)
{
	/* Stats SRAM double-buffered 2 x 28 KiB ping-pong */
	assert(fujisan_stats_sram_bank_pa(0U) == 0xFFF10000U);
	assert(fujisan_stats_sram_bank_pa(1U) == 0xFFF17000U);
	assert(FUJISAN_STATS_SRAM_BANK1 - FUJISAN_STATS_SRAM_BANK0 == 0x7000U);
	assert(FUJISAN_STATS_SRAM_SIZE == 0xE000U);

	/* PDAF 4-slot 600 KiB DDR ring (stride 0xD800 = 36 KiB pri + 18 KiB sec) */
	assert(fujisan_pdaf_slot_pri_pa(0U) == 0x2C4B0000U);
	assert(fujisan_pdaf_slot_sec_pa(0U) == 0x2C4B9000U);
	assert(fujisan_pdaf_slot_pri_pa(1U) == 0x2C4BD800U);
	assert(fujisan_pdaf_slot_pri_pa(2U) == 0x2C4CB000U);
	assert(fujisan_pdaf_slot_pri_pa(3U) == 0x2C4D8800U);
	assert(FUJISAN_PDAF_PRI_SIZE + FUJISAN_PDAF_SEC_SIZE == FUJISAN_PDAF_SLOT_STRIDE);

	/* Unblocked PDAF registers & 20-ch write-mask helper */
	assert(FUJISAN_STATS_FLT_CFG_START == 0x108U);
	assert(FUJISAN_STATS_PDAF_PRI_LO == 0x2D4U && FUJISAN_STATS_PDAF_SEC_HI == 0x2E0U);
	assert(fujisan_stats_ch_enable_word(0x3FFU, 0x155U) == 0x03FF0155U);

	/* LV Ch0/Ch1 2-plane semi-planar Y+UV offsets & RAW DMA */
	assert(FUJISAN_LV_PLANE0_Y_LO == 0xC4U && FUJISAN_LV_PLANE1_UV_LO == 0xCCU);
	assert(FUJISAN_RAW_PACK_MODE == 0x5CU && FUJISAN_RAW_SUBFMT == 0x70U);

	printf("[PASS] 5. CCDIF RAW DMA + Semi-Planar Y+UV LV + 20-ch 3A/PDAF Stats & 4-slot DDR ring\n");
}

static void test_lenscom_spi0_protocol(void)
{
	uint8_t hdr[FUJISAN_LENSCOM_HDR_BYTES];
	struct fujisan_lens_identity id;
	uint32_t af_frame, iris_frame;

	/* S12 Sentinel handshake */
	assert(fujisan_lenscom_verify_sentinel(0xC33CA55AU, 0xA55A3CC3U));
	assert(!fujisan_lenscom_verify_sentinel(0x00000000U, 0xA55A3CC3U));

	/* 128-byte Lens Identity Header (BCD v1.30 at [98..99]) */
	memset(hdr, 0, sizeof(hdr));
	hdr[24] = 45U;
	hdr[25] = 0U;
	hdr[26] = 100U;
	hdr[27] = 0U;
	hdr[28] = 40U;
	hdr[29] = 0U;
	hdr[98] = 0x01U;
	hdr[99] = 0x30U;
	assert(fujisan_lenscom_parse_identity(hdr, &id));
	assert(id.valid && id.fw_major_bcd == 0x01U && id.fw_minor_bcd == 0x30U);
	assert(id.min_focal_mm == 45U && id.max_focal_mm == 100U && id.min_fstop_x10 == 40U);

	/* Frame32 command packing */
	af_frame = fujisan_lenscom_pack_frame32(FUJISAN_LENSCOM_CMD_AF_DRIVE, 3U, 0x7FU);
	iris_frame = fujisan_lenscom_pack_frame32(FUJISAN_LENSCOM_CMD_IRIS_SET, 4U, 0x20U);
	assert((af_frame >> 20) == FUJISAN_LENSCOM_CMD_AF_DRIVE);
	assert((iris_frame >> 20) == FUJISAN_LENSCOM_CMD_IRIS_SET);

	printf("[PASS] 6. GF-Mount LENSCOM SPI0 Frame32 (S12 sentinel, 128B header, AF/Iris)\n");
}

static void test_mali_v61_y0l2_rasc_and_lsid_mmu(void)
{
	struct fujisan_mve_buffer_frame f_nv12, f_y0l2;
	uint8_t rasc[128];
	uint32_t pte_text, pte_bss;

	/* 8-bit NV12 (0x0201) 2-plane semi-planar */
	fujisan_mv61_fill_frame_nv12(&f_nv12, 0x50000000U, 1920U, 1080U, 1920U);
	assert(f_nv12.format == 0x0201U);
	assert(f_nv12.plane[0] == 0x50000000U);
	assert(f_nv12.plane[1] == 0x50000000U + (1920U * 1080U));
	assert(f_nv12.stride[0] == 1920U && f_nv12.stride[1] == 1920U);

	/* 10-bit Y0L2 (0x0121) 1-plane packed 2x2-tiled (NOT AFBC; stride[0] = stride * 4) */
	fujisan_mv61_fill_frame_y0l2(&f_y0l2, 0x50000000U, 3840U, 2160U, 2304U);
	assert(f_y0l2.format == 0x0121U);
	assert(f_y0l2.frame_flags == 4U);
	assert(f_y0l2.plane[0] == 0x50000000U && f_y0l2.plane[1] == 0U);
	assert(f_y0l2.stride[0] == 9216U && f_y0l2.stride[1] == 0U);

	/* ARM MVE v2 RASC magic check (0x0000EB5E 0x00000204) */
	memset(rasc, 0, sizeof(rasc));
	rasc[0] = 0x5EU; rasc[1] = 0xEBU; rasc[2] = 0x00U; rasc[3] = 0x00U;
	rasc[4] = 0x04U; rasc[5] = 0x02U; rasc[6] = 0x00U; rasc[7] = 0x00U;
	assert(fujisan_mv61_check_rasc_header(rasc, sizeof(rasc)));
	rasc[0] ^= 0xFFU;
	assert(!fujisan_mv61_check_rasc_header(rasc, sizeof(rasc)));

	/* LSID 2-Level MMU PTE encoding (BUSATR3=3 RO=2 => 0xC0000002; RW=3 => 0xC0000003) */
	pte_text = fujisan_mv61_encode_pte(0x00CA0000U, 3U, FUJISAN_MV61_PTE_RO);
	pte_bss = fujisan_mv61_encode_pte(0x02400000U, 3U, FUJISAN_MV61_PTE_RW);
	assert((pte_text & 0xC0000003U) == 0xC0000002U);
	assert(((pte_text >> 2) & 0x0FFFFFFFU) == (0x00CA0000U >> 12));
	assert((pte_bss & 0xC0000003U) == 0xC0000003U);
	assert(FUJISAN_MV61_LSID_BASE(0U) == 0x0200U && FUJISAN_MV61_LSID_BASE(3U) == 0x02C0U);

	printf("[PASS] 7. ARM Mali-V61 4K VPU (NV12 0x0201, 10-bit Y0L2 0x0121, RASC magic, LSID MMU)\n");
}

int main(void)
{
	test_base_spx4_bsp();
	test_pm_4layer_power_tree();
	test_imx461_sensor_and_25_modes();
	test_slvsec_and_ccdtg();
	test_ccdif_lv_raw_and_pdaf_stats();
	test_lenscom_spi0_protocol();
	test_mali_v61_y0l2_rasc_and_lsid_mmu();
	printf("ALL FUJISAN SPX4 DRIVER UNIT TESTS PASSED!\n");
	return 0;
}
