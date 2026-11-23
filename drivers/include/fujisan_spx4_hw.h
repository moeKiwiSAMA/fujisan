/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fujisan OS — SPX4-MF Medium-Format Camera (SPX4 / X-Processor 4) Master Hardware Header
 *
 * Dual-compiled for both Linux 6.6.x kernel drivers (__KERNEL__) and the
 * standalone host C11 unit test suite (tests/test_fujisan_drivers.c).
 *
 * Every register offset, bitmask, IRQ, power-domain order, and protocol framing
 * rule below is anchored to SPX4-MF 2.13 static/dynamic DAT verification
 * (CMOS-RE-1..4 & Wave-3 hardware atlases). Zero proprietary firmware blobs.
 */

#ifndef _FUJISAN_SPX4_HW_H_
#define _FUJISAN_SPX4_HW_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

/* ============================================================================
 * 1. SPI1 Power-Peer Sub-MCU Protocol (CS GPIO Bank 12 Pin 5 active-HIGH)
 * ============================================================================ */

#define SPX4_PWR_MAX_RETRIES		3

#define SPX4_PWR_SEL_STATUS		0x00U
#define SPX4_PWR_SEL_CTRL		0x02U
#define SPX4_PWR_SEL_BATT_ADC		0x03U
#define SPX4_PWR_SEL_FLAGS		0x0EU
#define SPX4_PWR_SEL_SHUTDOWN_MASK	0x0FU
#define SPX4_PWR_SEL_WAKE_CAUSE		0x10U
#define SPX4_PWR_SEL_CMD		0x11U
#define SPX4_PWR_SEL_WAKE_CTRL		0x12U
#define SPX4_PWR_SEL_RTC_LOAD_LO	0x1DU
#define SPX4_PWR_SEL_RTC_LOAD_HI	0x1EU
#define SPX4_PWR_SEL_NVMEM_W0		0x1FU
#define SPX4_PWR_SEL_NVMEM_W1		0x20U
#define SPX4_PWR_SEL_NVMEM_W2		0x21U
#define SPX4_PWR_SEL_NVMEM_W3		0x22U
#define SPX4_PWR_SEL_RTC_SEC_LO		0x23U
#define SPX4_PWR_SEL_RTC_SEC_HI		0x24U
#define SPX4_PWR_SEL_WAKE_CNT_LO	0x25U
#define SPX4_PWR_SEL_WAKE_CNT_HI	0x26U
#define SPX4_PWR_SEL_RTC_CTRL		0x27U
#define SPX4_PWR_SEL_SMART_CAP		0x2FU
#define SPX4_PWR_SEL_SMART_SOC		0x30U
#define SPX4_PWR_SEL_SMART_AUTH		0x31U
#define SPX4_PWR_SEL_BATT_TEMP_DK	0x32U

#define SPX4_PWR_STATUS_READY		(1U << 15)
#define SPX4_PWR_STATUS_SHTDN_B14	(1U << 14)
#define SPX4_PWR_STATUS_SW_B10		(1U << 10)
#define SPX4_PWR_STATUS_SW_B9		(1U << 9)
#define SPX4_PWR_STATUS_SHTDN_B3	(1U << 3)

#define SPX4_PWR_CTRL_SHUTDOWN_FLAG	(1U << 2)
#define SPX4_PWR_CTRL_LOW_BATT		(1U << 8)
#define SPX4_PWR_CTRL_AUX_B15		(1U << 15)

#define SPX4_PWR_FLAGS_SRC_B0		(1U << 0)
#define SPX4_PWR_FLAGS_SLEEP_B4		(1U << 4)
#define SPX4_PWR_FLAGS_B5		(1U << 5)

#define SPX4_PWR_CMD_WAKE_ACK		0x3F00U
#define SPX4_PWR_CMD_SOC_ACTIVE		0x0008U
#define SPX4_PWR_CMD_POWER_OFF		0x0800U

#define SPX4_PWR_WAKE_CTRL_ARM		(1U << 0)
#define SPX4_PWR_WAKE_CTRL_SEL		(1U << 8)
#define SPX4_PWR_WAKE_CTRL_ARMED	(SPX4_PWR_WAKE_CTRL_SEL | SPX4_PWR_WAKE_CTRL_ARM)
#define SPX4_PWR_WAKE_MAX_SECS		0x00FFFFFFU

#define SPX4_PWR_RTC_CTRL_NVMEM_COMMIT	(1U << 0)
#define SPX4_PWR_RTC_CTRL_TIME_VALID	(1U << 1)
#define SPX4_PWR_RTC_CTRL_LOAD_STROBE	(1U << 9)
#define SPX4_PWR_RTC_COMMIT_BITS	(SPX4_PWR_RTC_CTRL_LOAD_STROBE | SPX4_PWR_RTC_CTRL_TIME_VALID)

#define SPX4_PWR_NVMEM_WORDS		4U
#define SPX4_PWR_NVMEM_BYTES		8U
#define SPX4_PWR_NVMEM_MAGIC_AA		0xAAU
#define SPX4_PWR_NVMEM_MAGIC_55		0x55U

#define SPX4_PWR_TEMP_KELVIN_OFFSET	2732
#define SPX4_PWR_TEMP_INVALID_HI	0xFFFEU

static inline uint8_t spx4_power_checksum(uint8_t b0, uint8_t b1, uint8_t b2)
{
	return (uint8_t)(~((unsigned int)b0 + (unsigned int)b1 + (unsigned int)b2));
}

static inline void spx4_power_encode_frame(uint8_t tx[4], uint8_t selector,
					   bool write, uint16_t val)
{
	tx[0] = (uint8_t)((selector & 0x7FU) | (write ? 0x80U : 0x00U));
	tx[1] = write ? (uint8_t)(val >> 8) : 0x00U;
	tx[2] = write ? (uint8_t)(val & 0xFFU) : 0x00U;
	tx[3] = spx4_power_checksum(tx[0], tx[1], tx[2]);
}

static inline bool spx4_power_verify_frame(const uint8_t rx[4])
{
	unsigned int sum = (unsigned int)rx[0] + (unsigned int)rx[1] +
			   (unsigned int)rx[2] + (unsigned int)rx[3];

	return (sum & 0xFFU) == 0xFFU;
}

static inline bool spx4_power_decode_response(const uint8_t rx[4], uint16_t *out_val)
{
	if (!spx4_power_verify_frame(rx))
		return false;
	if (out_val)
		*out_val = (uint16_t)(((uint16_t)rx[1] << 8) | rx[2]);
	return true;
}

static inline bool spx4_power_temp_dk_to_decidegc(uint16_t raw_dk, int32_t *out_decidegc)
{
	if (raw_dk == 0U || raw_dk >= SPX4_PWR_TEMP_INVALID_HI)
		return false;
	if (out_decidegc)
		*out_decidegc = (int32_t)raw_dk - SPX4_PWR_TEMP_KELVIN_OFFSET;
	return true;
}

static inline uint16_t spx4_power_build_shutdown_mask(uint16_t sel00, uint16_t sel0E)
{
	uint16_t mask = 0;

	if (sel0E & SPX4_PWR_FLAGS_SRC_B0)
		mask |= (1U << 0);
	if (!(sel00 & SPX4_PWR_STATUS_SHTDN_B3))
		mask |= (1U << 14);
	if (!(sel00 & SPX4_PWR_STATUS_SHTDN_B14))
		mask |= (1U << 15);
	return mask;
}

static inline uint8_t spx4_power_nvmem_checksum(const uint8_t buf7[7])
{
	unsigned int sum = 0;
	unsigned int i;

	for (i = 0; i < 7; i++)
		sum += buf7[i];
	return (uint8_t)((0U - sum) & 0xFFU);
}

static inline bool spx4_power_nvmem_verify(const uint8_t buf8[8])
{
	unsigned int sum = 0;
	unsigned int i;

	for (i = 0; i < 8; i++)
		sum += buf8[i];
	return (sum & 0xFFU) == 0U;
}

static inline void spx4_power_nvmem_pack_words(const uint8_t buf8[8], uint16_t words[4])
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		words[i] = (uint16_t)buf8[i * 2] | ((uint16_t)buf8[i * 2 + 1] << 8);
}

static inline void spx4_power_nvmem_unpack_words(const uint16_t words[4], uint8_t buf8[8])
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		buf8[i * 2] = (uint8_t)(words[i] & 0xFFU);
		buf8[i * 2 + 1] = (uint8_t)(words[i] >> 8);
	}
}

static inline void spx4_power_pack_wake_countdown(uint32_t secs, uint16_t *lo16, uint16_t *hi8)
{
	if (secs > SPX4_PWR_WAKE_MAX_SECS)
		secs = SPX4_PWR_WAKE_MAX_SECS;
	if (lo16)
		*lo16 = (uint16_t)(secs & 0xFFFFU);
	if (hi8)
		*hi8 = (uint16_t)((secs >> 16) & 0xFFU);
}

static inline uint32_t spx4_power_unpack_wake_countdown(uint16_t lo16, uint16_t hi8)
{
	return (uint32_t)lo16 | (((uint32_t)(hi8 & 0xFFU)) << 16);
}

/* ============================================================================
 * 2. SAR ADC (0xFFF40000) Interleaved 0x555/0xAAA & soc-keyed-io (0xFFF42000)
 * ============================================================================ */

#define SPX4_ADC_PHYS_BASE		0xFFF40000U
#define SPX4_ADC_PHYS_SIZE		0x00000048U

#define SPX4_ADC_REG_CTRL		0x00U
#define SPX4_ADC_REG_DIV		0x04U
#define SPX4_ADC_REG_GATE		0x08U
#define SPX4_ADC_REG_STATUS		0x0CU
#define SPX4_ADC_REG_DATA(ch)		(0x10U + ((ch) * 4U))
#define SPX4_ADC_REG_TRIM		0x40U
#define SPX4_ADC_REG_AUX		0x44U

#define SPX4_ADC_CH_COUNT		12U
#define SPX4_ADC_CTRL_START_BIT		(1U << 0)
#define SPX4_ADC_CTRL_CONT_BIT		(1U << 1)
#define SPX4_ADC_CTRL_MASK_SHIFT	16U

#define SPX4_ADC_SCAN_MASK_EVEN		0x555U
#define SPX4_ADC_SCAN_MASK_ODD		0xAAAU
#define SPX4_ADC_SCAN_MASK_ALL		0xFFFU

enum spx4_mode_dial_pos {
	SPX4_MODE_UNKNOWN = -1,
	SPX4_MODE_M = 0,
	SPX4_MODE_C1,
	SPX4_MODE_C2,
	SPX4_MODE_C3,
	SPX4_MODE_C4,
	SPX4_MODE_C5,
	SPX4_MODE_C6,
	SPX4_MODE_P,
	SPX4_MODE_S,
	SPX4_MODE_A,
};

static inline int spx4_adc_decode_mode_dial(uint16_t raw)
{
	if (raw >= 100U && raw <= 590U)
		return SPX4_MODE_M;
	if (raw >= 591U && raw <= 994U)
		return SPX4_MODE_C1;
	if (raw >= 995U && raw <= 1394U)
		return SPX4_MODE_C2;
	if (raw >= 1395U && raw <= 1795U)
		return SPX4_MODE_C3;
	if (raw >= 1796U && raw <= 2198U)
		return SPX4_MODE_C4;
	if (raw >= 2199U && raw <= 2606U)
		return SPX4_MODE_C5;
	if (raw >= 2607U && raw <= 3023U)
		return SPX4_MODE_C6;
	if (raw >= 3024U && raw <= 3450U)
		return SPX4_MODE_P;
	if (raw >= 3451U && raw <= 3893U)
		return SPX4_MODE_S;
	if (raw >= 3894U && raw <= 4095U)
		return SPX4_MODE_A;
	return SPX4_MODE_UNKNOWN;
}

#define SPX4_KEYED_IO_PHYS_BASE		0xFFF42000U
#define SPX4_KEYED_IO_PHYS_SIZE		0x00000100U

#define SPX4_KEYED_IO_COMMAND		0x00U
#define SPX4_KEYED_IO_READY_STAT	0x04U
#define SPX4_KEYED_IO_MASK_CTRL		0x08U
#define SPX4_KEYED_IO_STATUS		0x0CU
#define SPX4_KEYED_IO_STATUS_CLR	0x10U
#define SPX4_KEYED_IO_SEC_STAT		0x14U
#define SPX4_KEYED_IO_ACTIVE		0x18U
#define SPX4_KEYED_IO_ENABLE		0x20U
#define SPX4_KEYED_IO_COMPANION		0x28U
#define SPX4_KEYED_IO_DISABLE		0x40U
#define SPX4_KEYED_IO_AUX_CTRL		0x70U
#define SPX4_KEYED_IO_ROUTE(n)		(0x80U + ((n) * 4U))
#define SPX4_KEYED_IO_TAIL_F8		0xF8U
#define SPX4_KEYED_IO_TAIL_FC		0xFCU

#define SPX4_KEYED_IO_CMD_KEY1		0x0000AA01U	/* Power Domain ON */
#define SPX4_KEYED_IO_CMD_KEY2		0x0000AA02U	/* Power Domain OFF */
#define SPX4_KEYED_IO_CMD_FLAG_B31	0x80000000U
#define SPX4_KEYED_IO_STAT_ON_DONE	0x00000010U
#define SPX4_KEYED_IO_STAT_OFF_DONE	0x00000020U
#define SPX4_KEYED_IO_VALID_STAT_MASK	0x00000073U
#define SPX4_KEYED_IO_ROUTE_BAD_BITS	0xE0E0E0E0U

static inline bool spx4_keyed_io_cmd_valid(uint32_t cmd)
{
	uint32_t key = cmd & ~SPX4_KEYED_IO_CMD_FLAG_B31;

	return key == SPX4_KEYED_IO_CMD_KEY1 || key == SPX4_KEYED_IO_CMD_KEY2;
}

static inline bool spx4_keyed_io_status_mask_valid(uint32_t val)
{
	return (val & ~SPX4_KEYED_IO_VALID_STAT_MASK) == 0U;
}

static inline bool spx4_keyed_io_route_valid(uint32_t val)
{
	return (val & SPX4_KEYED_IO_ROUTE_BAD_BITS) == 0U;
}

static inline uint32_t spx4_keyed_io_apply_commit(uint32_t active,
						  uint32_t enable,
						  uint32_t disable)
{
	return (active | enable) & ~disable;
}

/* ============================================================================
 * 3. Hardware JPEG Encoder (0xFFFA0000, IRQ 135) & Decoder (0xFFFA1000, IRQ 136)
 * ============================================================================ */

#define SPX4_JPEG_ENC_PHYS_BASE		0xFFFA0000U
#define SPX4_JPEG_ENC_PHYS_SIZE		0x00000134U
#define SPX4_JPEG_ENC_GIC_IRQ		135U

#define SPX4_JPEG_DEC_PHYS_BASE		0xFFFA1000U
#define SPX4_JPEG_DEC_PHYS_SIZE		0x0000012CU
#define SPX4_JPEG_DEC_GIC_IRQ		136U

#define SPX4_JPEG_DMA_HI_PHYS_BASE	0xFFF4A740U
#define SPX4_JPEG_DMA_HI_PHYS_SIZE	0x00000008U
#define SPX4_JPEG_DMA_HI_SRC_OFF	0x00U
#define SPX4_JPEG_DMA_HI_DST_OFF	0x04U

#define SPX4_JENC_CTRL			0x000U
#define SPX4_JENC_FORMAT_CTRL		0x00CU
#define SPX4_JENC_INT_STATUS		0x024U
#define SPX4_JENC_INT_MASK		0x028U
#define SPX4_JENC_SUBSAMPLE		0x040U
#define SPX4_JENC_SRC_SIZE		0x044U
#define SPX4_JENC_QSCALE		0x048U
#define SPX4_JENC_DRI			0x080U
#define SPX4_JENC_QTBL_CTRL		0x084U
#define SPX4_JENC_QTBL_DATA		0x08CU
#define SPX4_JENC_OUT_BYTES		0x094U
#define SPX4_JENC_MARKER_CTRL		0x0B0U
#define SPX4_JENC_MISC			0x0F8U
#define SPX4_JENC_SRC_YC_ADDR		0x100U
#define SPX4_JENC_DST_JPEG_ADDR		0x10CU
#define SPX4_JENC_DST_JPEG_MAX		0x110U
#define SPX4_JENC_DMA_STAT		0x114U
#define SPX4_JENC_DMA_CTRL		0x118U
#define SPX4_JENC_MODE			0x11CU
#define SPX4_JENC_SWAP_CTRL		0x120U
#define SPX4_JENC_WINDOW_DIM		0x12CU
#define SPX4_JENC_LINE_PITCH		0x130U

#define SPX4_JENC_CTRL_START		(1U << 0)
#define SPX4_JENC_CTRL_RESET		(1U << 8)
#define SPX4_JENC_INT_DONE		(1U << 4)
#define SPX4_JENC_INT_ERR		(1U << 17)
#define SPX4_JENC_INT_ERR_MASK		0x00FF0000U
#define SPX4_JENC_QTBL_LOAD_EN		(1U << 8)
#define SPX4_JENC_QTBL_SEL_LUMA		0U
#define SPX4_JENC_QTBL_SEL_CHROMA	1U
#define SPX4_JENC_FORMAT_DEFAULT	(0x0AU << 16)
#define SPX4_JENC_MODE_ENCODE		1U
#define SPX4_JENC_SUBSAMPLE_422		0U

#define SPX4_JDEC_CTRL			0x000U
#define SPX4_JDEC_INT_STATUS		0x024U
#define SPX4_JDEC_INT_MASK		0x028U
#define SPX4_JDEC_IMG_SIZE		0x044U
#define SPX4_JDEC_SRC_JPEG_ADDR		0x100U
#define SPX4_JDEC_DST_YC_ADDR		0x10CU
#define SPX4_JDEC_DMA_CTRL		0x118U
#define SPX4_JDEC_OFF_MODE		0x11CU
#define SPX4_JDEC_LINE_PITCH		0x128U

static inline void spx4_dma34_split(uint64_t pa, uint32_t *lo32, uint32_t *hi2)
{
	if (lo32)
		*lo32 = (uint32_t)(pa & 0xFFFFFFFFULL);
	if (hi2)
		*hi2 = (uint32_t)((pa >> 32) & 0x3ULL);
}

static inline uint32_t spx4_jpeg_pack_dim(uint16_t width, uint16_t height)
{
	return ((uint32_t)height << 16) | (uint32_t)width;
}

static inline uint32_t spx4_jpeg_pack_qscale(uint8_t luma_q, uint8_t chroma_q)
{
	return (((uint32_t)chroma_q & 0x0FU) << 4) | ((uint32_t)luma_q & 0x0FU);
}

static inline uint32_t spx4_jpeg_pack_qtbl_word(const uint8_t q4[4])
{
	return (uint32_t)q4[0] |
	       ((uint32_t)q4[1] << 8) |
	       ((uint32_t)q4[2] << 16) |
	       ((uint32_t)q4[3] << 24);
}

/* ============================================================================
 * 4. DMP PICA / M2D OpenVG VGLIB 2D Accelerator (0xFFFD0000..0xFFFD20FF, IRQ 92)
 * ============================================================================ */

#define SPX4_VGLIB_PHYS_BASE		0xFFFD0000U
#define SPX4_VGLIB_PHYS_SIZE		0x00002100U
#define SPX4_VGLIB_GIC_IRQ		92U

#define SPX4_VGLIB_FILL_START		0x0010U
#define SPX4_VGLIB_FILL_END		0x0014U
#define SPX4_VGLIB_FILL_COLOR		0x0018U
#define SPX4_VGLIB_FILL_KICK		0x001CU
#define SPX4_VGLIB_IRQ_STATUS		0x0034U
#define SPX4_VGLIB_BUSY_STATUS		0x0058U
#define SPX4_VGLIB_FILL_EXT0		0x00E0U
#define SPX4_VGLIB_FILL_EXT1		0x00E4U
#define SPX4_VGLIB_FILL_EXT2		0x00E8U
#define SPX4_VGLIB_FILL_EXT3		0x00ECU

#define SPX4_VGLIB_FILL_KICK_SOLID	0x0F100201U
#define SPX4_VGLIB_FILL_KICK_EXT	0x0F100211U

#define SPX4_VGLIB_IRQ_CMD_DONE		(1U << 31)
#define SPX4_VGLIB_IRQ_COPY_DONE	(1U << 30)
#define SPX4_VGLIB_IRQ_FILL_DONE	(1U << 26)
#define SPX4_VGLIB_BUSY_PIPE_B26	(1U << 26)

#define SPX4_VGLIB_COPY_SRC		0x0C00U
#define SPX4_VGLIB_COPY_DST		0x0C04U
#define SPX4_VGLIB_COPY_SRC_DIM		0x0C08U
#define SPX4_VGLIB_COPY_DST_DIM		0x0C0CU
#define SPX4_VGLIB_COPY_MODE		0x0C10U
#define SPX4_VGLIB_COPY_KICK		0x0C18U
#define SPX4_VGLIB_COPY_INIT_CFG	0x0C1CU
#define SPX4_VGLIB_COPY_EXT0		0x0C20U
#define SPX4_VGLIB_COPY_EXT1		0x0C24U
#define SPX4_VGLIB_COPY_EXT2		0x0C28U

#define SPX4_VGLIB_COPY_MODE_LINEAR	1U
#define SPX4_VGLIB_COPY_MODE_MORTON8	2U
#define SPX4_VGLIB_COPY_MODE_EXT_BIT	(1U << 3)
#define SPX4_VGLIB_COPY_INIT_VAL	0x00008000U

#define SPX4_VGLIB_CMD_RST0		0x1000U
#define SPX4_VGLIB_CMD_ACK		0x1020U
#define SPX4_VGLIB_UNLOCK_MAGIC0	0x10A0U
#define SPX4_VGLIB_IRQ_MASK0		0x10C0U
#define SPX4_VGLIB_IRQ_MASK1		0x10C4U
#define SPX4_VGLIB_STRM0_KICK		0x10D0U
#define SPX4_VGLIB_STRM0_ADDR		0x10D4U
#define SPX4_VGLIB_STRM0_SIZE8		0x10DCU
#define SPX4_VGLIB_STRM1_KICK		0x10E0U
#define SPX4_VGLIB_STRM1_ADDR		0x10E4U
#define SPX4_VGLIB_STRM1_SIZE8		0x10ECU
#define SPX4_VGLIB_PIPE_DRAIN_RD	0x1200U
#define SPX4_VGLIB_PIPE_TRIG		0x1214U
#define SPX4_VGLIB_INIT_CTRL		0x1248U
#define SPX4_VGLIB_SUBMIT_CLR		0x1298U
#define SPX4_VGLIB_CMD_STAT_RST		0x1800U
#define SPX4_VGLIB_UNLOCK_MAGIC1	0x1880U
#define SPX4_VGLIB_IRQ_MASK2		0x18C0U
#define SPX4_VGLIB_STRM2_ARG0		0x20E0U
#define SPX4_VGLIB_STRM2_ARG1		0x20E8U
#define SPX4_VGLIB_STRM2_KICK		0x20F0U

#define SPX4_VGLIB_MAGIC_UNLOCK		0x12345678U
#define SPX4_VGLIB_IRQ_MASK_DEFAULT	0xFFFFFFF0U
#define SPX4_VGLIB_PIPE_TRIG_NORM	0x0000FFFFU
#define SPX4_VGLIB_PIPE_TRIG_EXT	0x0100FFFFU
#define SPX4_VGLIB_PIPE_DRAIN_TRIG	0x01000000U

#define SPX4_DMP_REG_PATH_CFG0		0x0002U
#define SPX4_DMP_REG_PATH_CFG1		0x0003U
#define SPX4_DMP_REG_FILL_RULE0		0x0004U
#define SPX4_DMP_REG_FILL_RULE1		0x0005U
#define SPX4_DMP_REG_DRAW_COMMIT	0x0006U
#define SPX4_DMP_REG_FRAG_EN		0x0010U
#define SPX4_DMP_REG_MATRIX_3X3		0x0041U
#define SPX4_DMP_REG_STRM0_SYNC		0x0080U
#define SPX4_DMP_REG_STRM0_SEQ		0x0085U
#define SPX4_DMP_REG_STRM1_SYNC		0x00A0U
#define SPX4_DMP_REG_STRM1_SEQ		0x00A6U
#define SPX4_DMP_REG_SCISSOR		0x00C0U
#define SPX4_DMP_REG_TEX0_BASE		0x0155U
#define SPX4_DMP_REG_BLEND_CTRL		0x0184U
#define SPX4_DMP_REG_SOLID_COLOR	0x0185U
#define SPX4_DMP_REG_LUT_FIFO		0x0188U
#define SPX4_DMP_REG_FB_BASE		0x01A4U
#define SPX4_DMP_REG_FB_FMT		0x01B0U
#define SPX4_DMP_OP_INLINE_QUAD		0x00000022U

static inline uint32_t spx4_vglib_pkt_hdr(uint16_t reg_id, uint8_t byte_mask,
					  uint16_t payload_words, bool auto_inc)
{
	uint32_t extra = (payload_words > 0U) ? ((uint32_t)(payload_words - 1U) & 0x1FFU) : 0U;

	return ((uint32_t)reg_id << 16) |
	       (((uint32_t)byte_mask & 0x0FU) << 12) |
	       (extra << 3) |
	       0x00000004U |
	       (auto_inc ? 0x00000002U : 0x00000000U);
}

static inline uint32_t spx4_vglib_stream_size8(uint32_t word_count)
{
	return word_count >> 1;
}

static inline uint32_t spx4_vglib_stream2_units(uint32_t byte_val)
{
	return byte_val >> 3;
}

static inline bool spx4_vglib_build_solid_rect(uint32_t *s0, size_t s0_cap, uint32_t *s0_len,
					       uint32_t *s1, size_t s1_cap, uint32_t *s1_len,
					       uint32_t fb_pa, uint16_t width, uint16_t height,
					       uint32_t abgr_color, uint32_t seq_id)
{
	size_t n0 = 0, n1 = 0;

	if (!s0 || !s1 || !s0_len || !s1_len || s0_cap < 18U || s1_cap < 20U)
		return false;

	s0[n0++] = SPX4_DMP_OP_INLINE_QUAD;
	s0[n0++] = 0U;
	s0[n0++] = 0U;
	s0[n0++] = width;
	s0[n0++] = height;
	s0[n0++] = (uint32_t)(-(int32_t)width);
	s0[n0++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_STRM0_SYNC, 0xFU, 5U, true);
	s0[n0++] = 0U;
	s0[n0++] = 0U;
	s0[n0++] = 0U;
	s0[n0++] = 0U;
	s0[n0++] = 0U;
	s0[n0++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_DRAW_COMMIT, 0xFU, 1U, false);
	s0[n0++] = 1U;
	s0[n0++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_STRM0_SEQ, 0xFU, 1U, false);
	s0[n0++] = seq_id;
	if (n0 & 1U)
		s0[n0++] = 0U;

	s1[n1++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_FRAG_EN, 0x1U, 1U, false);
	s1[n1++] = 1U;
	s1[n1++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_SCISSOR, 0xFU, 2U, true);
	s1[n1++] = ((uint32_t)(width ? (width - 1U) : 0U) << 16);
	s1[n1++] = ((uint32_t)(height ? (height - 1U) : 0U) << 16);
	s1[n1++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_SOLID_COLOR, 0xFU, 1U, false);
	s1[n1++] = abgr_color;
	s1[n1++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_FB_BASE, 0xFU, 1U, false);
	s1[n1++] = fb_pa;
	s1[n1++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_STRM1_SYNC, 0xFU, 6U, true);
	s1[n1++] = 0U;
	s1[n1++] = 0U;
	s1[n1++] = 0U;
	s1[n1++] = 0U;
	s1[n1++] = 0U;
	s1[n1++] = 0x01000002U;
	s1[n1++] = spx4_vglib_pkt_hdr(SPX4_DMP_REG_STRM1_SEQ, 0xFU, 1U, false);
	s1[n1++] = seq_id;
	if (n1 & 1U)
		s1[n1++] = 0U;

	*s0_len = (uint32_t)n0;
	*s1_len = (uint32_t)n1;
	return true;
}

/* ============================================================================
 * 5. LCDC1 (0xFFFA9000) / LCDC0 (0xFFF7A000) & Display Clock Gate (0xFFF49700)
 * ============================================================================ */

#define SPX4_LCDC1_PHYS_BASE		0xFFFA9000U
#define SPX4_LCDC0_PHYS_BASE		0xFFF7A000U
#define SPX4_LCDC_CLK_GATE_PHYS		0xFFF49700U

#define SPX4_LCDC_CLK_MODE_MASK		0x00000300U
#define SPX4_LCDC_CLK_MODE_LCDC1	0x00000200U
#define SPX4_LCDC_CLK_MODE_OFF		0x00000000U
#define SPX4_LCDC_CLK_MODE_EVF		0x00000100U

#define SPX4_LCDC_PLANE_OSD0_BIT	(1U << 16)
#define SPX4_LCDC_PLANE_ACTIVE_BIT	(1U << 12)
#define SPX4_LCDC_PLANE_PIC0_BIT	(1U << 8)
#define SPX4_LCDC_PLANE_PIC1_BIT	(1U << 4)

#define SPX4_LCDC_OSD0_CTRL_OPAQUE	0x1FC7F088U
#define SPX4_LCDC_OSD0_CTRL_TRANSP	0x20080001U

static inline uint32_t spx4_lcdc_clk_gate_rmw(uint32_t cur, uint32_t mode)
{
	uint32_t cleared = cur & ~SPX4_LCDC_CLK_MODE_MASK;

	if (mode == 1U || mode == 2U || mode == 3U)
		return cleared | SPX4_LCDC_CLK_MODE_LCDC1;
	if (mode == 0U || mode == 4U)
		return cleared | SPX4_LCDC_CLK_MODE_OFF;
	return cleared | SPX4_LCDC_CLK_MODE_EVF;
}

static inline uint32_t spx4_lcdc_plane_en_rmw(uint32_t cur, bool hide_pic_planes)
{
	uint32_t en = cur;

	if (hide_pic_planes)
		en &= ~(SPX4_LCDC_PLANE_PIC0_BIT | SPX4_LCDC_PLANE_PIC1_BIT);
	en |= SPX4_LCDC_PLANE_OSD0_BIT | SPX4_LCDC_PLANE_ACTIVE_BIT;
	return en;
}

static inline uint32_t spx4_xrgb8888_to_lcdc_rgba(uint32_t xrgb)
{
	return (xrgb << 8) | 0x000000FFU;
}

/* ============================================================================
 * 6. 54 MHz Interval Timer (0xFFF3C300) — STRICT Ch7-ONLY Contract
 * ============================================================================ */

#define SPX4_TIMER_PHYS_BASE		0xFFF3C300U
#define SPX4_TIMER_REG_CTRL		0x00U
#define SPX4_TIMER_CH7_INDEX		7U
#define SPX4_TIMER_CH7_COUNT_OFF	0x48U
#define SPX4_TIMER_CH7_RELOAD_OFF	0x4CU
#define SPX4_TIMER_CH7_DISABLE_VAL	0x00800000U
#define SPX4_TIMER_CH7_ENABLE_VAL	0x00800080U
#define SPX4_TIMER_CH0_6_FORBIDDEN	0x007F007FU

static inline uint32_t spx4_timer_ch7_ctrl_val(bool enable)
{
	return enable ? SPX4_TIMER_CH7_ENABLE_VAL : SPX4_TIMER_CH7_DISABLE_VAL;
}

/* ============================================================================
 * 7. 4-Layer Imaging Power-Domain & Clock Tree (fujisan-spx4-pm, CMOS-RE-4 DAT)
 *    Layer 1: Keyed Power Domains 0xFFF42000 (18 domains d=0..17, 0xAA01/AA02)
 *    Layer 2: PLLs & Dividers 0xFF701000..0xFF70C080 (+0xF0 busy poll)
 *    Layer 3: Peripheral Reset/Clk Gates 0xFFF48300..0xFFF48334 & 0xFFF48608
 *    Layer 4: AXI/Bus Clock Gates 0xFF70F000..0xFF70F03C
 *    INVARIANT: Power-ON MUST set 0xFFF483xx BEFORE 0xFF70F0xx; Power-OFF reverses!
 * ============================================================================ */

#define FUJISAN_PM_NUM_DOMAINS		18U

/* Verified DAT Power Domain IDs (0xFFF42020 bit index) */
#define FUJISAN_PD_CMOS_SLVSEC_CCDIF	2U
#define FUJISAN_PD_LCDC			4U
#define FUJISAN_PD_IIFP_0		6U
#define FUJISAN_PD_IIFP_1		7U
#define FUJISAN_PD_IIFP_2		8U
#define FUJISAN_PD_UNIRESZ_JPEG_STILL	9U
#define FUJISAN_PD_MALI_V61		10U
#define FUJISAN_PD_RECC			13U

#define FUJISAN_RSTCLK_PHYS_BASE	0xFFF48300U
#define FUJISAN_RSTCLK_CMOS_OFF		0x08U	/* 0xFFF48308 */
#define FUJISAN_RSTCLK_AUX_OFF		0x0CU	/* 0xFFF4830C */
#define FUJISAN_RSTCLK_LCDC_OFF		0x10U	/* 0xFFF48310 */
#define FUJISAN_RSTCLK_IIFP_OFF		0x14U	/* 0xFFF48314 */
#define FUJISAN_RSTCLK_JPEG_OFF		0x18U	/* 0xFFF48318 */
#define FUJISAN_RSTCLK_V61_OFF		0x20U	/* 0xFFF48320 */
#define FUJISAN_RSTCLK_RECC_OFF		0x30U	/* 0xFFF48330 */
#define FUJISAN_RSTCLK_EXT_SEL_OFF	0x300U	/* 0xFFF48600 */
#define FUJISAN_RSTCLK_EXT_CMOS_OFF	0x308U	/* 0xFFF48608 */

#define FUJISAN_AXICLK_PHYS_BASE	0xFF70F000U
#define FUJISAN_AXICLK_CMOS_OFF		0x04U	/* 0xFF70F004 */
#define FUJISAN_AXICLK_AUX_OFF		0x08U	/* 0xFF70F008 */
#define FUJISAN_AXICLK_LCDC_OFF		0x0CU	/* 0xFF70F00C */
#define FUJISAN_AXICLK_RECC_OFF		0x10U	/* 0xFF70F010 */
#define FUJISAN_AXICLK_JPEG_OFF		0x14U	/* 0xFF70F014 */
#define FUJISAN_AXICLK_V61_OFF		0x18U	/* 0xFF70F018 */
#define FUJISAN_AXICLK_IIFP0_OFF	0x24U	/* 0xFF70F024 */
#define FUJISAN_AXICLK_IIFP1_OFF	0x28U	/* 0xFF70F028 */
#define FUJISAN_AXICLK_IIFP2_OFF	0x2CU	/* 0xFF70F02C */

#define FUJISAN_PLL_PHYS_BASE		0xFF701000U
#define FUJISAN_PLL_BUSY_OFF		0xF0U
#define FUJISAN_PLL_DIV_EN_BIT		(1U << 8)
#define FUJISAN_PLL_DIV_MASK		0xFFU

/* Interconnect / sensor-bus bridge (S5 0x02394E88..0x02394F04 DAT) */
#define FUJISAN_SYSBRIDGE_PHYS_BASE	0xFFF49500U
#define FUJISAN_SYSBRIDGE_CTRL_OFF	0x00U	/* write 0x3 */
#define FUJISAN_SYSBRIDGE_CFG0_OFF	0x30U	/* write 0x10000704 */
#define FUJISAN_SYSBRIDGE_CFG1_OFF	0x34U	/* write 0x70 -> 0x71 */
#define FUJISAN_SYSBRIDGE_LCDC_OFF	0x200U	/* 0xFFF49700 = 0x200 */

/* 34-bit AXI Upper-Address Bank (0xFFF4A700..0xFFF4A793, all 18 DAT regs) */
#define FUJISAN_AXI_HI_PHYS_BASE	0xFFF4A700U
#define FUJISAN_AXI_HI_IIFP_B9_0	0x10U
#define FUJISAN_AXI_HI_IIFP_B9_1	0x14U
#define FUJISAN_AXI_HI_MOVDC_0		0x38U
#define FUJISAN_AXI_HI_MOVDC_1		0x3CU
#define FUJISAN_AXI_HI_JPEG_0		0x40U
#define FUJISAN_AXI_HI_JPEG_1		0x44U
#define FUJISAN_AXI_HI_LSRAW_0		0x50U
#define FUJISAN_AXI_HI_LSRAW_1		0x54U
#define FUJISAN_AXI_HI_PLAYDSP_0	0x60U
#define FUJISAN_AXI_HI_PLAYDSP_1	0x68U
#define FUJISAN_AXI_HI_PLAYDSP_2	0x6CU
#define FUJISAN_AXI_HI_UNIDMAC_0	0x70U
#define FUJISAN_AXI_HI_UNIDMAC_1	0x74U
#define FUJISAN_AXI_HI_STILL_0		0x78U
#define FUJISAN_AXI_HI_STILL_1		0x7CU
#define FUJISAN_AXI_HI_RAWFEED_0	0x88U
#define FUJISAN_AXI_HI_RAWFEED_1	0x8CU
#define FUJISAN_AXI_HI_RECC_0		0x90U

struct fujisan_pm_domain_desc {
	uint8_t domain_id;
	uint32_t pd_mask;	/* bitmask in 0xFFF42020 / 0xFFF42040 */
	uint16_t rstclk_off;	/* offset from 0xFFF48300 */
	uint32_t rstclk_parent;	/* Layer-3 parent bits */
	uint32_t rstclk_sub;	/* Layer-3 sub-engine bits */
	uint16_t axiclk_off;	/* offset from 0xFF70F000 */
	uint32_t axiclk_parent;	/* Layer-4 parent bits */
	uint32_t axiclk_sub;	/* Layer-4 sub-engine bits */
	uint32_t pll_div_pa;	/* primary PLL divider PA (0x015AC5F0) */
	const char *name;
};

static inline uint32_t fujisan_pll_div_word(uint8_t divider, bool enable)
{
	return (enable ? FUJISAN_PLL_DIV_EN_BIT : 0U) | ((uint32_t)divider & FUJISAN_PLL_DIV_MASK);
}

static inline bool fujisan_pm_get_domain_desc(uint8_t domain_id,
					      struct fujisan_pm_domain_desc *out)
{
	if (!out)
		return false;

	switch (domain_id) {
	case FUJISAN_PD_CMOS_SLVSEC_CCDIF:
		out->domain_id = domain_id;
		out->pd_mask = (1U << 2);
		out->rstclk_off = FUJISAN_RSTCLK_CMOS_OFF;
		out->rstclk_parent = 0x00000003U;
		out->rstclk_sub = 0x00000FFCU;
		out->axiclk_off = FUJISAN_AXICLK_CMOS_OFF;
		out->axiclk_parent = 0x00000007U;
		out->axiclk_sub = 0x0003FFF8U;
		out->pll_div_pa = 0xFF701248U;
		out->name = "cmos-slvsec-ccdif";
		return true;
	case FUJISAN_PD_LCDC:
		out->domain_id = domain_id;
		out->pd_mask = (1U << 4);
		out->rstclk_off = FUJISAN_RSTCLK_LCDC_OFF;
		out->rstclk_parent = 0x00000007U;
		out->rstclk_sub = 0x00000000U;
		out->axiclk_off = FUJISAN_AXICLK_LCDC_OFF;
		out->axiclk_parent = 0x00000003U;
		out->axiclk_sub = 0x000000F8U;
		out->pll_div_pa = 0xFF701444U;
		out->name = "lcdc0-lcdc1";
		return true;
	case FUJISAN_PD_IIFP_0:
	case FUJISAN_PD_IIFP_1:
	case FUJISAN_PD_IIFP_2:
		out->domain_id = domain_id;
		out->pd_mask = (1U << 6) | (1U << 7) | (1U << 8);
		out->rstclk_off = FUJISAN_RSTCLK_IIFP_OFF;
		out->rstclk_parent = 0x00000001U;
		out->rstclk_sub = 0x00070F3EU;
		out->axiclk_off = FUJISAN_AXICLK_IIFP0_OFF;
		out->axiclk_parent = 0x00000003U;
		out->axiclk_sub = 0x000000FCU;
		out->pll_div_pa = 0xFF702044U;
		out->name = "iifp";
		return true;
	case FUJISAN_PD_UNIRESZ_JPEG_STILL:
		out->domain_id = domain_id;
		out->pd_mask = (1U << 9);
		out->rstclk_off = FUJISAN_RSTCLK_JPEG_OFF;
		out->rstclk_parent = 0x00000001U;
		out->rstclk_sub = 0x0000001EU;
		out->axiclk_off = FUJISAN_AXICLK_JPEG_OFF;
		out->axiclk_parent = 0x0000001FU;
		out->axiclk_sub = 0x0001FFE0U;
		out->pll_div_pa = 0xFF701840U;
		out->name = "uniresz-jpeg-still";
		return true;
	case FUJISAN_PD_MALI_V61:
		out->domain_id = domain_id;
		out->pd_mask = (1U << 10);
		out->rstclk_off = FUJISAN_RSTCLK_V61_OFF;
		out->rstclk_parent = 0x00000001U;
		out->rstclk_sub = 0x00000002U;
		out->axiclk_off = FUJISAN_AXICLK_V61_OFF;
		out->axiclk_parent = 0x00000001U;
		out->axiclk_sub = 0x00000006U;
		out->pll_div_pa = 0xFF70184CU;
		out->name = "mali-v61";
		return true;
	case FUJISAN_PD_RECC:
		out->domain_id = domain_id;
		out->pd_mask = (1U << 13);
		out->rstclk_off = FUJISAN_RSTCLK_RECC_OFF;
		out->rstclk_parent = 0x00000001U;
		out->rstclk_sub = 0x0000007EU;
		out->axiclk_off = FUJISAN_AXICLK_RECC_OFF;
		out->axiclk_parent = 0x00000003U;
		out->axiclk_sub = 0x000FFFFCU;
		out->pll_div_pa = 0xFF701848U;
		out->name = "recc";
		return true;
	default:
		return false;
	}
}

/* ============================================================================
 * 8. Sony IMX461 102MP CMOS Sensor & SPI Cmd Engine (0xFFF6C200, CMOS-RE-1 DAT)
 * ============================================================================ */

#define FUJISAN_IMX461_SPI_PHYS_BASE	0xFFF6C200U
#define FUJISAN_IMX461_SPI_PHYS_SIZE	0x00000044U

#define FUJISAN_IMX461_SPI_CTRL		0x00U	/* b0 enable/reset */
#define FUJISAN_IMX461_SPI_START_BUSY	0x04U	/* W b0=1 start; R b0=1 busy */
#define FUJISAN_IMX461_SPI_XFER_CFG	0x08U	/* [17:16] mode (1=W, 3=R), [12:8] bytes, [7:0] pkts */
#define FUJISAN_IMX461_SPI_DMA_PTR	0x0CU
#define FUJISAN_IMX461_SPI_RX_STAT	0x20U	/* RO: b8, [1:0] */
#define FUJISAN_IMX461_SPI_RX_DATA0	0x24U	/* RO: 0x24..0x40 */

#define FUJISAN_IMX461_CMD_WRITE	0x81U	/* ~0x7E */
#define FUJISAN_IMX461_CMD_READ		0x82U	/* ~0x7D */

/* Verified DAT IMX461 internal sensor register addresses */
#define FUJISAN_IMX461_REG_STBY		0x0000U	/* bit 0: active/stream */
#define FUJISAN_IMX461_REG_MDSEL	0x0001U	/* 8-bit mode code */
#define FUJISAN_IMX461_REG_REGHOLD	0x0002U	/* b6=0x40 group-hold, b7=0x80 latch */
#define FUJISAN_IMX461_REG_PGC_LO	0x0012U	/* 16-bit LE analog gain */
#define FUJISAN_IMX461_REG_PGC_HI	0x0013U
#define FUJISAN_IMX461_REG_DGAIN	0x0018U	/* 8-bit digital gain */
#define FUJISAN_IMX461_REG_VMAX_LO	0x001FU	/* 12-bit dynamic VMAX */
#define FUJISAN_IMX461_REG_VMAX_HI	0x0020U
#define FUJISAN_IMX461_REG_HMAX_LO	0x0023U	/* 15-bit dynamic HMAX/SVR */
#define FUJISAN_IMX461_REG_HMAX_HI	0x0024U
#define FUJISAN_IMX461_REG_TEMP_LO	0x0040U	/* 16-bit LE sensor temperature */
#define FUJISAN_IMX461_REG_TEMP_HI	0x0041U
#define FUJISAN_IMX461_REG_SHR_0	0x0047U	/* 24-bit exposure lines */
#define FUJISAN_IMX461_REG_SHR_1	0x0048U
#define FUJISAN_IMX461_REG_SHR_2	0x0049U
#define FUJISAN_IMX461_REG_READ_GATE	0x004FU	/* bit 3 (0x08) opens SPI readback */
#define FUJISAN_IMX461_REG_SLEEP_GATE	0x019EU	/* bit 2 (0x04) wake/sleep gate */

#define FUJISAN_IMX461_REGHOLD_BIT	0x40U
#define FUJISAN_IMX461_LATCH_BIT	0x80U
#define FUJISAN_IMX461_READ_GATE_OPEN	0x08U
#define FUJISAN_IMX461_SLEEP_WAKE_BIT	0x04U

/* Verified DAT GPIO pin numbers (bank * 16 + pin) for IMX461 cold power-on */
#define FUJISAN_IMX461_GPIO_XCLR	((4U * 16U) + 5U)	/* GPIO(4,5) = 69 */
#define FUJISAN_IMX461_GPIO_RAIL_A	((4U * 16U) + 7U)	/* GPIO(4,7) = 71 */
#define FUJISAN_IMX461_GPIO_RAIL_B	((12U * 16U) + 2U)	/* GPIO(12,2) = 194 */
#define FUJISAN_IMX461_GPIO_RST_PULSE	((0U * 16U) + 15U)	/* GPIO(0,15) = 15 (10ms pulse) */
#define FUJISAN_IMX461_GPIO_SYNC0	((5U * 16U) + 1U)	/* GPIO(5,1) = 81 */
#define FUJISAN_IMX461_GPIO_SYNC1	((5U * 16U) + 0U)	/* GPIO(5,0) = 80 */
#define FUJISAN_IMX461_RST_PULSE_MS	10U

#define FUJISAN_IMX461_NUM_MODES	25U

struct fujisan_imx461_mode_desc {
	uint8_t table_idx;	/* T[0..24] */
	uint8_t mdsel;		/* IMX461 reg 0x0001 */
	uint8_t bpp;		/* 16 / 14 / 12 */
	uint8_t ccdtg_b4;	/* 0xFFF6C024 one-hot selector */
	uint8_t ccdtg_b5;	/* 0xFFF6C024 route/depth code */
	uint16_t width;		/* active sensor width (11760 or 3920) */
	uint16_t height;	/* nominal active lines */
	uint16_t fps_x10;	/* nominal frame rate * 10 */
	const char *name;
};

static inline void fujisan_imx461_pack_write(uint8_t pkt[4], uint16_t reg, uint8_t val)
{
	pkt[0] = FUJISAN_IMX461_CMD_WRITE;
	pkt[1] = (uint8_t)(reg >> 8);
	pkt[2] = (uint8_t)(reg & 0xFFU);
	pkt[3] = val;
}

static inline void fujisan_imx461_pack_read(uint8_t pkt[4], uint16_t reg)
{
	pkt[0] = FUJISAN_IMX461_CMD_READ;
	pkt[1] = (uint8_t)(reg >> 8);
	pkt[2] = (uint8_t)(reg & 0xFFU);
	pkt[3] = 0x00U;
}

static inline uint32_t fujisan_imx461_xfer_cfg(bool is_read, uint8_t byte_cnt, uint8_t pkt_cnt)
{
	uint32_t mode = is_read ? 3U : 1U;

	return (mode << 16) | (((uint32_t)byte_cnt & 0x1FU) << 8) | (uint32_t)pkt_cnt;
}

static inline uint16_t fujisan_imx461_decode_temp_raw(uint8_t reg_40, uint8_t reg_41)
{
	return (uint16_t)reg_40 | ((uint16_t)reg_41 << 8);
}

static inline void fujisan_imx461_pack_vmax(uint16_t vmax, uint8_t *lo, uint8_t *hi)
{
	if (lo)
		*lo = (uint8_t)(vmax & 0xFFU);
	if (hi)
		*hi = (uint8_t)((vmax >> 8) & 0x0FU);
}

static inline void fujisan_imx461_pack_hmax(uint16_t hmax, uint8_t *lo, uint8_t *hi)
{
	if (lo)
		*lo = (uint8_t)(hmax & 0xFFU);
	if (hi)
		*hi = (uint8_t)((hmax >> 8) & 0x7FU);
}

static inline void fujisan_imx461_pack_shr(uint32_t shr, uint8_t out[3])
{
	out[0] = (uint8_t)(shr & 0xFFU);
	out[1] = (uint8_t)((shr >> 8) & 0xFFU);
	out[2] = (uint8_t)((shr >> 16) & 0xFFU);
}

static inline bool fujisan_imx461_get_mode(uint8_t idx, struct fujisan_imx461_mode_desc *out)
{
	static const struct fujisan_imx461_mode_desc modes[FUJISAN_IMX461_NUM_MODES] = {
		{  0, 0x00, 16, 4, 3, 11760, 8754,   40, "still-102mp-16bit" },
		{  1, 0x40, 14, 3, 2, 11760, 8754,   60, "still-102mp-14bit" },
		{  2, 0x80, 12, 2, 1, 11760, 8754,   87, "still-102mp-12bit" },
		{  3, 0x80, 12, 2, 1, 11760, 8754,  119, "still-102mp-es-cont" },
		{  4, 0xA0, 12, 2, 1, 11760, 2918,  545, "lv-fullwidth-binned" },
		{  5, 0x91, 12, 2, 1, 11760, 2160,  300, "movie-4k-30p-a" },
		{  6, 0x91, 12, 2, 1, 11760, 2160,  300, "movie-4k-30p-b" },
		{  7, 0x91, 12, 2, 1, 11760, 2160,  300, "movie-4k-30p-c" },
		{  8, 0xAA, 12, 2, 1,  3920, 2918,  600, "lv-1-3-60p" },
		{  9, 0xAF, 12, 2, 1, 11760, 2918,  857, "lv-fullwidth-85p" },
		{ 10, 0xB1, 12, 2, 1, 11760, 2160,  600, "lv-60p-full" },
		{ 11, 0xB1, 12, 2, 1,  3922, 2160,  600, "lv-60p-4k-crop" },
		{ 12, 0x92, 12, 2, 1, 11760, 1080,  600, "movie-fhd-60p-a" },
		{ 13, 0x92, 12, 2, 1, 11760, 1080,  500, "movie-fhd-50p" },
		{ 14, 0x92, 12, 2, 1, 11760, 1080,  600, "movie-fhd-60p-b" },
		{ 15, 0xB2, 12, 2, 1,  3920, 2160,  601, "lv-3920-60p" },
		{ 16, 0xB2, 12, 2, 1,  3920, 2160,  858, "lv-3920-85p" },
		{ 17, 0xB3, 12, 2, 1, 11760, 2918,  545, "lv-54p-crop" },
		{ 18, 0xB6, 12, 2, 1,  3920, 1458, 1401, "af-hs-140fps" },
		{ 19, 0xB8, 12, 2, 1,  3920,  972, 2350, "af-uhs-235fps" },
		{ 20, 0xB8, 12, 2, 1,  3920,  972, 2002, "af-uhs-200fps-pal" },
		{ 21, 0xB8, 12, 2, 1,  3920,  972, 2402, "af-uhs-240fps-ntsc" },
		{ 22, 0xB1, 12, 2, 1, 11760, 2160,  594, "movie-lv-59p4" },
		{ 23, 0xB1, 12, 2, 1, 11760, 2160,  600, "boot-default-60p" },
		{ 24, 0xB1, 12, 2, 1, 11760, 2160,  600, "lv-movie-60p-var" },
	};

	if (!out || idx >= FUJISAN_IMX461_NUM_MODES)
		return false;
	*out = modes[idx];
	return true;
}

/* ============================================================================
 * 9. Dual-Bank 8-Lane SLVS-EC RX (0xFFF69000/6A000) + PHY (0xFFF6B000) +
 *    CCDTG Timing Generator (0xFFF6C000, CMOS-RE-1 DAT)
 * ============================================================================ */

#define FUJISAN_SLVSEC_BANK0_PHYS	0xFFF69000U
#define FUJISAN_SLVSEC_BANK1_PHYS	0xFFF6A000U
#define FUJISAN_SLVSEC_BANK_SIZE	0x00000300U
#define FUJISAN_SLVSEC_PHY_PHYS		0xFFF6B000U
#define FUJISAN_SLVSEC_PHY_SIZE		0x00000100U
#define FUJISAN_CCDTG_PHYS		0xFFF6C000U
#define FUJISAN_CCDTG_SIZE		0x00000440U

/* GIC IRQ numbers (DAT @ 0x015A89AC) */
#define FUJISAN_IRQ_CCDTG_0		100U
#define FUJISAN_IRQ_CCDTG_1		102U
#define FUJISAN_IRQ_SLVSEC_CH0		296U
#define FUJISAN_IRQ_SLVSEC_ERR		297U
#define FUJISAN_IRQ_SLVSEC_CH1		298U

/* SLVS-EC Global regs (Bank 0 / Bank 1 offset +0x000..+0x028) */
#define FUJISAN_SLVSEC_GLB_CTRL		0x000U	/* b20,b16,b4,b0 ch reset/en */
#define FUJISAN_SLVSEC_GLB_PHYCFG	0x008U
#define FUJISAN_SLVSEC_GLB_LANEPDN	0x00CU	/* b[7:0] active-LOW (0=en, 1=dis); b12,b8 ctrl */
#define FUJISAN_SLVSEC_GLB_PCS_ERR	0x024U	/* R/W1C: [23:16] IllegalComma, [15:8] Disp, [7:0] InvCode */
#define FUJISAN_SLVSEC_GLB_PCS_EN	0x028U

/* SLVS-EC Per-Channel regs: Ch0=+0x100, Ch1=+0x200 */
#define FUJISAN_SLVSEC_CH_BASE(ch)	(0x100U + ((ch) * 0x100U))
#define FUJISAN_SLVSEC_CH_ENABLE	0x00U
#define FUJISAN_SLVSEC_CH_MODE		0x04U
#define FUJISAN_SLVSEC_CH_NORM_STAT	0x08U	/* W1C: b7,b6,b5,b4,b1,b0 */
#define FUJISAN_SLVSEC_CH_NORM_EN	0x0CU
#define FUJISAN_SLVSEC_CH_LINK_ERR	0x10U	/* W1C: 10 bits b[9:0] */
#define FUJISAN_SLVSEC_CH_LINK_EN	0x14U
#define FUJISAN_SLVSEC_CH_MAC_ERR	0x18U	/* W1C: b24,b16,b8,b0 */
#define FUJISAN_SLVSEC_CH_FIFO_ERR	0x1CU	/* W1C: [15:8] UF, [7:0] OF */
#define FUJISAN_SLVSEC_CH_MAC_EN	0x20U
#define FUJISAN_SLVSEC_CH_FIFO_EN	0x24U
#define FUJISAN_SLVSEC_CH_LANE_MUX	0x34U	/* 8 x 4-bit nibbles */
#define FUJISAN_SLVSEC_CH_LANE_MASK	0x38U	/* 8 x 1-bit active lanes */
#define FUJISAN_SLVSEC_CH_CNT0		0x44U	/* RO u13 */
#define FUJISAN_SLVSEC_CH_CNT1		0x48U	/* RO 2 x u13 */

#define FUJISAN_SLVSEC_NORM_IRQ_MASK	0x000000F3U
#define FUJISAN_SLVSEC_LINK_ERR_MASK	0x000003FFU
#define FUJISAN_SLVSEC_MAC_ERR_MASK	0x01010101U

/* SLVS-EC PHY (0xFFF6B000) */
#define FUJISAN_SLVSEC_PHY_CFG0		0x004U
#define FUJISAN_SLVSEC_PHY_FLAGS	0x008U
#define FUJISAN_SLVSEC_PHY_CFG1		0x00CU
#define FUJISAN_SLVSEC_PHY_TRIM0	0x010U
#define FUJISAN_SLVSEC_PHY_TRIM1	0x014U
#define FUJISAN_SLVSEC_PHY_CTRL		0x0F4U

/* CCDTG Timing Generator (0xFFF6C000) */
#define FUJISAN_CCDTG_ENABLE		0x004U	/* b0 enable */
#define FUJISAN_CCDTG_FMT_SYNC		0x008U	/* [1:0]/[3:2] bpp, b16/b17 sync */
#define FUJISAN_CCDTG_HV_TIMING		0x00CU	/* [14:0] H, [30:16] V (<=0x4000) */
#define FUJISAN_CCDTG_ROUTE		0x024U	/* [19:16] 1<<(b4-1), [7:0] b5 */
#define FUJISAN_CCDTG_TRIG_W1C		0x144U	/* [3:0] */
#define FUJISAN_CCDTG_PULSE_CH(n)	(0x280U + ((n) * 0x40U))	/* 6 channels */

enum fujisan_tgclk_mode {
	FUJISAN_TGCLK_ST = 0,
	FUJISAN_TGCLK_LO_36M = 1,
	FUJISAN_TGCLK_HI_72M = 2,
	FUJISAN_TGCLK_PS_27M = 3,
};

/* Active-low 8-lane power-down mask for 0xFFF6900C[7:0]: bit=0 enables lane! */
static inline uint8_t fujisan_slvsec_lane_pdn_mask(uint8_t active_lane_mask)
{
	return (uint8_t)(~active_lane_mask);
}

static inline uint32_t fujisan_ccdtg_route_word(uint8_t b4, uint8_t b5)
{
	uint32_t onehot = (b4 >= 1U && b4 <= 4U) ? (1U << (b4 - 1U)) : 0U;

	return (onehot << 16) | (uint32_t)b5;
}

static inline uint32_t fujisan_ccdtg_hv_word(uint16_t h, uint16_t v)
{
	return (((uint32_t)(v & 0x7FFFU)) << 16) | ((uint32_t)(h & 0x7FFFU));
}

/* ============================================================================
 * 10. CCDIF RAW DMA (0xFFF6E000), LV Semi-Planar Y+UV (0xFFF72000/73000/74000),
 *     20-Ch 3A/PDAF Stats Engine (0xFFF70000), Stats SRAM (0xFFF10000..DFFF) &
 *     PDAF DDR Ring (0x2C4B0000, CMOS-RE-2 & CMOS-RE-3 DAT)
 * ============================================================================ */

#define FUJISAN_CCDIF_RAW_PHYS		0xFFF6E000U
#define FUJISAN_CCDIF_STATS_PHYS	0xFFF70000U
#define FUJISAN_CCDIF_LV0_PHYS		0xFFF72000U
#define FUJISAN_CCDIF_LV1_PHYS		0xFFF73000U
#define FUJISAN_CCDIF_LV2_PHYS		0xFFF74000U
#define FUJISAN_MOVDC_PHYS		0xFFF7D000U

#define FUJISAN_IRQ_CCDIF_RAW		105U
#define FUJISAN_IRQ_CCDIF_STATS		106U
#define FUJISAN_IRQ_LV_CH0		107U
#define FUJISAN_IRQ_LV_CH1		108U
#define FUJISAN_IRQ_LV_CH2		109U
#define FUJISAN_IRQ_MOVDC		110U

/* 0xFFF6E000 Live RAW+OB Write-DMA (all DAT offsets incl. previously missed 11) */
#define FUJISAN_RAW_CTRL		0x000U
#define FUJISAN_RAW_CFG			0x004U
#define FUJISAN_RAW_IRQ_W1C		0x008U	/* bit 8 */
#define FUJISAN_RAW_CROP_XY		0x00CU
#define FUJISAN_RAW_CROP_WH		0x010U
#define FUJISAN_RAW_OB0_XY		0x014U
#define FUJISAN_RAW_OB0_WH		0x018U
#define FUJISAN_RAW_OB_CTRL		0x01CU
#define FUJISAN_RAW_OB1_XY		0x020U
#define FUJISAN_RAW_OB1_WH		0x024U
#define FUJISAN_RAW_START		0x040U
#define FUJISAN_RAW_STRIDE_PRI		0x044U
#define FUJISAN_RAW_STRIDE_OB		0x048U
#define FUJISAN_RAW_ADDR_PRI_HI		0x04CU	/* [1:0] */
#define FUJISAN_RAW_ADDR_PRI_LO		0x050U
#define FUJISAN_RAW_ADDR_OB_HI		0x054U	/* [1:0] */
#define FUJISAN_RAW_ADDR_OB_LO		0x058U
#define FUJISAN_RAW_PACK_MODE		0x05CU	/* [2:0] 0..5 */
#define FUJISAN_RAW_BYTE_ORDER		0x060U
#define FUJISAN_RAW_SUBFMT		0x070U	/* [1:0] 0..2 */

/* 0xFFF72000 / 0xFFF73000 2-Plane Semi-Planar Y+UV Live-View DMA */
#define FUJISAN_LV_ENABLE		0x004U	/* bit 0 */
#define FUJISAN_LV_MODE			0x008U
#define FUJISAN_LV_PLANE0_Y_HI		0x0C0U	/* [1:0] */
#define FUJISAN_LV_PLANE0_Y_LO		0x0C4U	/* 8B-aligned */
#define FUJISAN_LV_PLANE1_UV_HI		0x0C8U	/* [1:0] */
#define FUJISAN_LV_PLANE1_UV_LO		0x0CCU	/* 8B-aligned */
#define FUJISAN_LV_STRIDE		0x0D0U	/* [15:0] */
#define FUJISAN_LV_FMT_CTRL		0x0D4U	/* [18:16] fmt 0..5, [21:20] subsamp */
#define FUJISAN_LV_RING_CTRL		0x0D8U	/* [3:0] ring cnt, [28] en */
#define FUJISAN_LV_IRQ_STATUS		0x0DCU	/* W1C + [13:12] active buf idx 0..3 */

/* 0xFFF70000 20-Ch 3A/PDAF/ROB Stats DMA Engine (holes [0x108..0x120] & [0x2D4..0x2E0] unblocked) */
#define FUJISAN_STATS_RESET		0x000U
#define FUJISAN_STATS_EN_LO		0x004U	/* [25:16] write-mask, [9:0] ch 0..9 */
#define FUJISAN_STATS_EN_HI		0x008U	/* [25:16] write-mask, [9:0] ch 10..19 */
#define FUJISAN_STATS_IRQ_W1C		0x00CU	/* bit 0 -> GIC IRQ 106 */
#define FUJISAN_STATS_FLT_CFG_START	0x108U	/* 0x108..0x120 */
#define FUJISAN_STATS_AE_PRI_LO		0x22CU
#define FUJISAN_STATS_AE_PRI_HI		0x230U
#define FUJISAN_STATS_BAYER_LO		0x254U
#define FUJISAN_STATS_BAYER_HI		0x258U
#define FUJISAN_STATS_ROB0_LO		0x264U
#define FUJISAN_STATS_ROB0_HI		0x268U
#define FUJISAN_STATS_PDAF_PRI_LO	0x2D4U	/* 34-bit Primary PDAF DDR base */
#define FUJISAN_STATS_PDAF_PRI_HI	0x2D8U
#define FUJISAN_STATS_PDAF_SEC_LO	0x2DCU	/* 34-bit Secondary PDAF DDR base */
#define FUJISAN_STATS_PDAF_SEC_HI	0x2E0U
#define FUJISAN_STATS_MASTER_CTRL	0x2E4U	/* bit 8 run/enable */
#define FUJISAN_STATS_BUSY		0x2E8U	/* bit 0 busy */

/* On-chip 56 KiB Double-Buffered Stats SRAM (2 x 28 KiB ping-pong banks) */
#define FUJISAN_STATS_SRAM_PHYS		0xFFF10000U
#define FUJISAN_STATS_SRAM_SIZE		0x0000E000U
#define FUJISAN_STATS_SRAM_BANK_SIZE	0x00007000U
#define FUJISAN_STATS_SRAM_BANK0	0xFFF10000U
#define FUJISAN_STATS_SRAM_BANK1	0xFFF17000U
#define FUJISAN_STATS_BAYER_OFFSET	0x00001060U	/* 4 x 16x16 u32 Bayer surface */
#define FUJISAN_STATS_ROB_OFFSET	0x00003A00U	/* 2 x 3 x 256B ROB OB-line stats */

/* PDAF 4-Slot DDR Ring (0x2C4B0000, 600 KiB, stride 0xD800 = 36 KiB pri + 18 KiB sec) */
#define FUJISAN_PDAF_RING_PHYS		0x2C4B0000U
#define FUJISAN_PDAF_RING_SIZE		0x00096000U
#define FUJISAN_PDAF_SLOT_STRIDE	0x0000D800U
#define FUJISAN_PDAF_PRI_SIZE		0x00009000U
#define FUJISAN_PDAF_SEC_OFFSET		0x00009000U
#define FUJISAN_PDAF_SEC_SIZE		0x00004800U
#define FUJISAN_PDAF_NUM_SLOTS		4U

static inline uint32_t fujisan_stats_sram_bank_pa(uint8_t bank)
{
	return (bank & 1U) ? FUJISAN_STATS_SRAM_BANK1 : FUJISAN_STATS_SRAM_BANK0;
}

static inline uint32_t fujisan_pdaf_slot_pri_pa(uint8_t slot)
{
	return FUJISAN_PDAF_RING_PHYS + ((uint32_t)(slot & 3U) * FUJISAN_PDAF_SLOT_STRIDE);
}

static inline uint32_t fujisan_pdaf_slot_sec_pa(uint8_t slot)
{
	return fujisan_pdaf_slot_pri_pa(slot) + FUJISAN_PDAF_SEC_OFFSET;
}

/* 20-ch stats write-mask helper: bit[25:16] = enable_mask, bit[9:0] = value */
static inline uint32_t fujisan_stats_ch_enable_word(uint16_t mask10, uint16_t val10)
{
	return (((uint32_t)(mask10 & 0x3FFU)) << 16) | ((uint32_t)(val10 & 0x3FFU));
}

/* ============================================================================
 * 11. GF-Mount LENSCOM SPI0 Frame32 Protocol (0xFFF32000, M9 DAT)
 * ============================================================================ */

#define FUJISAN_LENSCOM_SPI0_PHYS	0xFFF32000U
#define FUJISAN_LENSCOM_IRQ_RDY		182U

#define FUJISAN_LENSCOM_GPIO_MOUNT_DET	((0U * 16U) + 11U)	/* GPIO(0,11)=0 mounted */
#define FUJISAN_LENSCOM_GPIO_LENS_PWR	((15U * 16U) + 0U)	/* GPIO(15,0)=1 power */
#define FUJISAN_LENSCOM_GPIO_LENS_RDY	((15U * 16U) + 1U)	/* GPIO(15,1)=1 ready / IRQ 182 */

/* S12 Sentinel handshake & Bulk Mode commands (DAT @ M9 lenscom atlas) */
#define FUJISAN_LENSCOM_SENTINEL_TX	0xA55A3CC3U
#define FUJISAN_LENSCOM_SENTINEL_RX	0xC33CA55AU
#define FUJISAN_LENSCOM_HDR_BYTES	128U
#define FUJISAN_LENSCOM_BULK_END	0xFFFFU
#define FUJISAN_LENSCOM_BULK_ACK_PREFIX	0xFF00U
#define FUJISAN_LENSCOM_BULK_ACK_BYTE	0x5AU

#define FUJISAN_LENSCOM_CMD_BULK_HDR	0x014U
#define FUJISAN_LENSCOM_CMD_BULK_ACK	0x015U
#define FUJISAN_LENSCOM_CMD_BULK_EXIT	0x016U
#define FUJISAN_LENSCOM_CMD_BULK_ENTER	0x018U
#define FUJISAN_LENSCOM_CMD_POLL_STATUS	0x020U
#define FUJISAN_LENSCOM_CMD_AF_INIT	0x040U
#define FUJISAN_LENSCOM_CMD_AF_DRIVE	0x050U
#define FUJISAN_LENSCOM_CMD_AF_POS	0x054U
#define FUJISAN_LENSCOM_CMD_IRIS_SET	0x080U
#define FUJISAN_LENSCOM_CMD_IRIS_POS	0x0A8U
#define FUJISAN_LENSCOM_CMD_KEEPALIVE	0x0E0U

struct fujisan_lens_identity {
	uint8_t fw_major_bcd;	/* hdr[98] */
	uint8_t fw_minor_bcd;	/* hdr[99] */
	uint16_t min_focal_mm;	/* hdr[24..25] LE (0.1 mm or mm) */
	uint16_t max_focal_mm;	/* hdr[26..27] LE */
	uint16_t min_fstop_x10;	/* hdr[28..29] LE */
	bool valid;
};

static inline uint32_t fujisan_lenscom_pack_frame32(uint16_t cmd_id, uint8_t seq, uint8_t arg)
{
	return (((uint32_t)cmd_id & 0x0FFFU) << 20) |
	       (((uint32_t)seq & 0x0FU) << 16) |
	       ((uint32_t)arg << 8) |
	       (uint32_t)((cmd_id ^ seq ^ arg) & 0xFFU);
}

static inline bool fujisan_lenscom_verify_sentinel(uint32_t rx0, uint32_t rx1)
{
	return rx0 == FUJISAN_LENSCOM_SENTINEL_RX && rx1 == FUJISAN_LENSCOM_SENTINEL_TX;
}

static inline bool fujisan_lenscom_parse_identity(const uint8_t hdr[FUJISAN_LENSCOM_HDR_BYTES],
						  struct fujisan_lens_identity *out)
{
	if (!hdr || !out)
		return false;

	out->fw_major_bcd = hdr[98];
	out->fw_minor_bcd = hdr[99];
	out->min_focal_mm = (uint16_t)hdr[24] | ((uint16_t)hdr[25] << 8);
	out->max_focal_mm = (uint16_t)hdr[26] | ((uint16_t)hdr[27] << 8);
	out->min_fstop_x10 = (uint16_t)hdr[28] | ((uint16_t)hdr[29] << 8);
	out->valid = (out->fw_major_bcd != 0U || out->fw_minor_bcd != 0U);
	return out->valid;
}

/* ============================================================================
 * 12. ARM Mali-V61 4K VPU (0xFFE00000, IRQ 138, NV12/Y0L2 + LSID 2-Level MMU,
 *     CMOS-RE-4 DAT — Zero proprietary blobs; RASC FWs user-extracted)
 * ============================================================================ */

#define FUJISAN_MV61_PHYS_BASE		0xFFE00000U
#define FUJISAN_MV61_PHYS_SIZE		0x00001000U
#define FUJISAN_MV61_GIC_IRQ		138U

#define FUJISAN_MV61_VERSION		0x0000U
#define FUJISAN_MV61_ENABLE		0x0004U
#define FUJISAN_MV61_NCORES		0x0008U
#define FUJISAN_MV61_NLSID		0x000CU
#define FUJISAN_MV61_CORELSID		0x0010U
#define FUJISAN_MV61_JOBQUEUE		0x0014U
#define FUJISAN_MV61_IRQVE		0x0018U
#define FUJISAN_MV61_CLKFORCE		0x0024U
#define FUJISAN_MV61_FUSE		0x0034U
#define FUJISAN_MV61_BUSCTRL		0x0044U
#define FUJISAN_MV61_RESET		0x0050U

/* LSID[0..3] register windows (DAT @ h265_MapSession 0x01207508) */
#define FUJISAN_MV61_LSID_BASE(n)	(0x0200U + ((n) * 0x40U))
#define FUJISAN_MV61_LSID_CTRL		0x00U	/* write 0x4F0 */
#define FUJISAN_MV61_LSID_MMU_CTRL	0x04U	/* L1 PT physical descriptor */
#define FUJISAN_MV61_LSID_ALLOC		0x0CU	/* write 1 */
#define FUJISAN_MV61_LSID_NPROT		0x10U	/* write 0 */
#define FUJISAN_MV61_LSID_SCHED		0x14U	/* write 1 */
#define FUJISAN_MV61_LSID_TERMINATE	0x18U	/* write 1, poll 0 */
#define FUJISAN_MV61_LSID_IRQVE		0x1CU	/* write 0 */
#define FUJISAN_MV61_LSID_IRQHOST	0x20U	/* write 0 */
#define FUJISAN_MV61_LSID_BUSATR0	0x30U	/* 0x33 (RPC/Queues) */
#define FUJISAN_MV61_LSID_BUSATR1	0x34U	/* 0x33 (Bitstream/Internal) */
#define FUJISAN_MV61_LSID_BUSATR2	0x38U	/* 0x33 (Frame buffers) */
#define FUJISAN_MV61_LSID_BUSATR3	0x3CU	/* 0x23 (Firmware text/BSS) */

#define FUJISAN_MV61_LSID_CTRL_VAL	0x000004F0U
#define FUJISAN_MV61_BUSATR_DATA	0x00000033U
#define FUJISAN_MV61_BUSATR_FW		0x00000023U

/* LSID 2-Level MMU PTE encoding (DAT @ 0x01206624..0x01206F80) */
#define FUJISAN_MV61_PTE_UNMAPPED	0U
#define FUJISAN_MV61_PTE_RO		2U	/* Firmware .text */
#define FUJISAN_MV61_PTE_RW		3U	/* BSS / Queues / Buffers */

#define FUJISAN_MV61_L1_SLOT_FW		0U	/* 0x00000000 BUSATR3 */
#define FUJISAN_MV61_L1_SLOT_RPC	64U	/* 0x10000000 BUSATR0 */
#define FUJISAN_MV61_L1_SLOT_BITSTREAM	128U	/* 0x20000000 BUSATR1 */
#define FUJISAN_MV61_L1_SLOT_INTERNAL	264U	/* 0x42000000 BUSATR1 */
#define FUJISAN_MV61_L1_SLOT_FRAMEBUF	320U	/* 0x50000000 BUSATR2 */

/* ARM MVE v2 RASC Firmware Header Magic (after ^ 0xFF decode from user firmware.dat) */
#define FUJISAN_MV61_RASC_MAGIC0	0x0000EB5EU
#define FUJISAN_MV61_RASC_MAGIC1	0x00000204U

/* MVE Protocol Frame Codes & Formats (DAT @ 0x01205840..0x01205C94) */
#define FUJISAN_MVE_REQ_SET_OPTION	1010U	/* 0x3F2 */
#define FUJISAN_MVE_BUF_CODE_FRAME	3001U	/* 0xBB9 */
#define FUJISAN_MVE_BUF_CODE_PARAM	3003U	/* 0xBBB */

#define FUJISAN_MVE_FMT_YUV420_NV12	0x0201U	/* 8-bit 2-plane semi-planar YUV 4:2:0 */
#define FUJISAN_MVE_FMT_YUV420_Y0L2	0x0121U	/* 10-bit 1-plane packed 2x2-tiled (NOT AFBC) */
#define FUJISAN_MVE_FRAME_FLAG_4K	4U

/* 72-byte (0x48) mve_buffer_frame message (DAT @ 0x01205840) */
struct fujisan_mve_buffer_frame {
	uint32_t host_handle_lo;	/* +0x00 */
	uint32_t host_handle_hi;	/* +0x04 */
	uint32_t user_data_lo;		/* +0x08 */
	uint32_t user_data_hi;		/* +0x0C */
	uint32_t frame_flags;		/* +0x10 */
	uint16_t visible_width;		/* +0x14 */
	uint16_t visible_height;	/* +0x16 */
	uint16_t format;		/* +0x18: 0x0201 (NV12) or 0x0121 (Y0L2) */
	uint16_t reserved0;		/* +0x1A */
	uint32_t plane[3];		/* +0x1C..+0x24: MVE virtual addresses */
	uint32_t reserved1[3];		/* +0x28..+0x30 */
	uint32_t stride[3];		/* +0x34..+0x3C */
	uint32_t crop_left_right;	/* +0x40 */
	uint32_t crop_top_bottom;	/* +0x44 */
};

static inline uint32_t fujisan_mv61_encode_pte(uint32_t pa, uint8_t busatr_idx, uint8_t perm)
{
	return (((pa >> 12) & 0x0FFFFFFFU) << 2) |
	       (((uint32_t)(busatr_idx & 0x3U)) << 30) |
	       ((uint32_t)(perm & 0x3U));
}

static inline bool fujisan_mv61_check_rasc_header(const uint8_t *buf, size_t len)
{
	uint32_t w0, w1;

	if (!buf || len < 128U)
		return false;
	w0 = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	     ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
	w1 = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
	     ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
	return w0 == FUJISAN_MV61_RASC_MAGIC0 && w1 == FUJISAN_MV61_RASC_MAGIC1;
}

static inline void fujisan_mv61_fill_frame_nv12(struct fujisan_mve_buffer_frame *f,
						uint32_t y_vaddr, uint16_t width,
						uint16_t height, uint32_t stride)
{
	f->host_handle_lo = 0U;
	f->host_handle_hi = 0U;
	f->user_data_lo = 0U;
	f->user_data_hi = 0U;
	f->frame_flags = 0U;
	f->visible_width = width;
	f->visible_height = height;
	f->format = FUJISAN_MVE_FMT_YUV420_NV12;
	f->reserved0 = 0U;
	f->plane[0] = y_vaddr;
	f->plane[1] = y_vaddr + ((uint32_t)height * stride);
	f->plane[2] = 0U;
	f->reserved1[0] = 0U;
	f->reserved1[1] = 0U;
	f->reserved1[2] = 0U;
	f->stride[0] = stride;
	f->stride[1] = stride;
	f->stride[2] = 0U;
	f->crop_left_right = 0U;
	f->crop_top_bottom = 0U;
}

static inline void fujisan_mv61_fill_frame_y0l2(struct fujisan_mve_buffer_frame *f,
						uint32_t vaddr, uint16_t width,
						uint16_t height, uint32_t base_stride)
{
	f->host_handle_lo = 0U;
	f->host_handle_hi = 0U;
	f->user_data_lo = 0U;
	f->user_data_hi = 0U;
	f->frame_flags = FUJISAN_MVE_FRAME_FLAG_4K;
	f->visible_width = width;
	f->visible_height = height;
	f->format = FUJISAN_MVE_FMT_YUV420_Y0L2;
	f->reserved0 = 0U;
	f->plane[0] = vaddr;
	f->plane[1] = 0U;
	f->plane[2] = 0U;
	f->reserved1[0] = 0U;
	f->reserved1[1] = 0U;
	f->reserved1[2] = 0U;
	/* DAT @ 0x01205950: 10-bit Y0L2 uses single packed plane with stride[0] = stride << 2 */
	f->stride[0] = base_stride << 2;
	f->stride[1] = 0U;
	f->stride[2] = 0U;
	f->crop_left_right = 0U;
	f->crop_top_bottom = 0U;
}

#endif /* _FUJISAN_SPX4_HW_H_ */
