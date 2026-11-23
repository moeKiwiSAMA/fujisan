// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C Master Controller Driver for Fujisan SPX4 (Milbeaut-style byte I2C)
 *   - I2C0 @ 0xFFF3D000 (GIC SPI 65)
 *   - I2C1 @ 0xFFF3E000 (GIC SPI 66 — EVF Eye Sensor 0x39, PMIC/IMU)
 *   - I2C2 @ 0xFFF3F000 (GIC SPI 67 — Rear Capacitive Touchscreen 0x55)
 *
 * Strictly uses 8-bit accesses (readb/writeb) at 4-byte-spaced offsets
 * (+0x00..+0x1C), matching both real SPX4 silicon (S4 0x011E0C2C..0x011E11F0)
 * and spx4-emulator soc-i2c (I2cFactory.kt):
 *   +0x00 BSR (RO1) Bus Status: bit7=BB, bit4=LRB(NACK), bit3=TRX, bit0=FBT
 *   +0x04 BCR (RW1) Bus Control: bit5=SCC(rep-start), bit4=MSS(master),
 *                                bit3=ACK, bit1=INTE, bit0=INT
 *   +0x08 CCR (RW1) Clock Control: bit5=EN
 *   +0x0C ADR (RW1) Own slave address (0)
 *   +0x10 DAR (RW1) Data register (tx stage / rx read)
 *   +0x14 CSR (RW1) Clock divider (0..63)
 *   +0x18 FSR (RW1) Frequency select
 *   +0x1C REC (RW1) Bus recovery
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define SPX4_I2C_BSR		0x00
#define SPX4_I2C_BCR		0x04
#define SPX4_I2C_CCR		0x08
#define SPX4_I2C_ADR		0x0C
#define SPX4_I2C_DAR		0x10
#define SPX4_I2C_CSR		0x14
#define SPX4_I2C_FSR		0x18

#define SPX4_BSR_LRB		BIT(4) /* 1 = NACK */
#define SPX4_BCR_SCC		BIT(5)
#define SPX4_BCR_MSS		BIT(4)
#define SPX4_BCR_ACK		BIT(3)
#define SPX4_BCR_INT		BIT(0)
#define SPX4_CCR_EN		BIT(5)

struct fuji_spx4_i2c {
	struct i2c_adapter adap;
	void __iomem *base;
	bool skip_probe_io;
};

static int spx4_i2c_wait_int(struct fuji_spx4_i2c *bus)
{
	int tries = 2000;

	while (!(readb_relaxed(bus->base + SPX4_I2C_BCR) & SPX4_BCR_INT)) {
		if (--tries <= 0)
			return -ETIMEDOUT;
		udelay(5);
	}
	return 0;
}

static void spx4_i2c_stop(struct fuji_spx4_i2c *bus)
{
	writeb_relaxed(0x00, bus->base + SPX4_I2C_BCR);
	udelay(10);
}

static int spx4_i2c_xfer_msg(struct fuji_spx4_i2c *bus, struct i2c_msg *msg,
			     bool repeated_start)
{
	u8 addr8 = (u8)((msg->addr << 1) | ((msg->flags & I2C_M_RD) ? 1 : 0));
	u8 bcr, bsr;
	int i, ret;

	/* Stage 7-bit address + R/W bit */
	writeb_relaxed(addr8, bus->base + SPX4_I2C_DAR);
	bcr = SPX4_BCR_MSS | (repeated_start ? SPX4_BCR_SCC : 0);
	writeb_relaxed(bcr, bus->base + SPX4_I2C_BCR);

	ret = spx4_i2c_wait_int(bus);
	if (ret)
		return ret;

	bsr = readb_relaxed(bus->base + SPX4_I2C_BSR);
	if (bsr & SPX4_BSR_LRB)
		return -ENXIO;

	if (msg->flags & I2C_M_RD) {
		for (i = 0; i < msg->len; i++) {
			bool send_ack = (i + 1 < msg->len);

			bcr = SPX4_BCR_MSS | (send_ack ? SPX4_BCR_ACK : 0);
			writeb_relaxed(bcr, bus->base + SPX4_I2C_BCR);
			ret = spx4_i2c_wait_int(bus);
			if (ret)
				return ret;
			msg->buf[i] = readb_relaxed(bus->base + SPX4_I2C_DAR);
		}
	} else {
		for (i = 0; i < msg->len; i++) {
			writeb_relaxed(msg->buf[i], bus->base + SPX4_I2C_DAR);
			writeb_relaxed(SPX4_BCR_MSS, bus->base + SPX4_I2C_BCR);
			ret = spx4_i2c_wait_int(bus);
			if (ret)
				return ret;
			bsr = readb_relaxed(bus->base + SPX4_I2C_BSR);
			if (bsr & SPX4_BSR_LRB)
				return -ENXIO;
		}
	}

	return 0;
}

static int spx4_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct fuji_spx4_i2c *bus = i2c_get_adapdata(adap);
	int i, ret = 0;

	/* Ensure controller is enabled */
	if (!(readb_relaxed(bus->base + SPX4_I2C_CCR) & SPX4_CCR_EN)) {
		writeb_relaxed(0x00, bus->base + SPX4_I2C_ADR);
		writeb_relaxed(0x10, bus->base + SPX4_I2C_CSR);
		writeb_relaxed(0x02, bus->base + SPX4_I2C_FSR);
		writeb_relaxed(SPX4_CCR_EN, bus->base + SPX4_I2C_CCR);
	}

	for (i = 0; i < num; i++) {
		ret = spx4_i2c_xfer_msg(bus, &msgs[i], i > 0);
		if (ret)
			break;
	}

	spx4_i2c_stop(bus);
	return ret ? ret : num;
}

static u32 spx4_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm spx4_i2c_algo = {
	.master_xfer	= spx4_i2c_xfer,
	.functionality	= spx4_i2c_func,
};

static int fuji_spx4_i2c_probe(struct platform_device *pdev)
{
	struct fuji_spx4_i2c *bus;
	int ret;

	bus = devm_kzalloc(&pdev->dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return -ENOMEM;

	bus->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bus->base))
		return PTR_ERR(bus->base);

	/* Put controller into clean idle state */
	writeb_relaxed(0x00, bus->base + SPX4_I2C_CCR);
	writeb_relaxed(0x00, bus->base + SPX4_I2C_ADR);
	writeb_relaxed(0x10, bus->base + SPX4_I2C_CSR);
	writeb_relaxed(0x02, bus->base + SPX4_I2C_FSR);
	writeb_relaxed(SPX4_CCR_EN, bus->base + SPX4_I2C_CCR);

	bus->adap.owner = THIS_MODULE;
	bus->adap.algo = &spx4_i2c_algo;
	bus->adap.dev.parent = &pdev->dev;
	bus->adap.dev.of_node = pdev->dev.of_node;
	bus->adap.nr = of_alias_get_id(pdev->dev.of_node, "i2c");
	snprintf(bus->adap.name, sizeof(bus->adap.name), "fuji-spx4-i2c%d",
		 bus->adap.nr >= 0 ? bus->adap.nr : 0);
	i2c_set_adapdata(&bus->adap, bus);
	platform_set_drvdata(pdev, bus);

	ret = i2c_add_numbered_adapter(&bus->adap);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Fujisan SPX4 I2C bus %d registered\n", bus->adap.nr);
	return 0;
}

static int fuji_spx4_i2c_remove(struct platform_device *pdev)
{
	struct fuji_spx4_i2c *bus = platform_get_drvdata(pdev);

	i2c_del_adapter(&bus->adap);
	writeb_relaxed(0x00, bus->base + SPX4_I2C_CCR);
	return 0;
}

static const struct of_device_id fuji_spx4_i2c_of_match[] = {
	{ .compatible = "fujisan,spx4-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_i2c_of_match);

static struct platform_driver fuji_spx4_i2c_driver = {
	.probe	= fuji_spx4_i2c_probe,
	.remove	= fuji_spx4_i2c_remove,
	.driver	= {
		.name		= "fuji-spx4-i2c",
		.of_match_table	= fuji_spx4_i2c_of_match,
	},
};
module_platform_driver(fuji_spx4_i2c_driver);

MODULE_DESCRIPTION("Fujisan SPX4 byte-mode I2C master driver");
MODULE_LICENSE("GPL");
