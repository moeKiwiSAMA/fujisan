// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rear Capacitive Multi-Touchscreen Driver for SPX4-MF Medium-Format Camera
 *   - Bus:   I2C2 (0xFFF3F000), 7-bit address 0x55
 *   - Reset: GPIO Bank 7 Pin 2
 *   - IRQ:   GIC SPI 128 (edge/level interrupt)
 *
 * Exact 3-transfer wire protocol (DAT verified from section4 touchPanel T91):
 *   1. Read reg 0x10 (2 B): [touch_count (0..2), status_flags (abort if & 0x18)]
 *   2. Read reg 0x11 (7 B): Finger 0 [flags(b7=active,b3:0=id), x_lo, x_hi, y_lo, y_hi, z0, z1]
 *   3. Read reg 0x11 (7 B): Finger 1 [flags(b7=active,b3:0=id), x_lo, x_hi, y_lo, y_hi, z0, z1]
 *   Aux: reg 0x12 (1 B) heartbeat status, reg 0xF6 (1 B) command write.
 *
 * Evdev device name: "fuji-spx4-touchscreen" (1024 x 768 MT Type B + ST mirror).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>

#define SPX4_TP_REG_HEADER	0x10
#define SPX4_TP_REG_FIFO	0x11
#define SPX4_TP_REG_STATUS	0x12
#define SPX4_TP_REG_CMD		0xF6

#define SPX4_TP_MAX_SLOTS	2
#define SPX4_TP_WIDTH		1024
#define SPX4_TP_HEIGHT		768

struct fuji_spx4_touch {
	struct i2c_client *client;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	u32 max_x;
	u32 max_y;
};

static int spx4_tp_read_reg(struct i2c_client *client, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret = i2c_transfer(client->adapter, msgs, 2);

	return (ret == 2) ? 0 : (ret < 0 ? ret : -EIO);
}

static irqreturn_t spx4_tp_irq_thread(int irq, void *dev_id)
{
	struct fuji_spx4_touch *ts = dev_id;
	u8 hdr[2] = { 0 };
	u8 f0[7] = { 0 };
	u8 f1[7] = { 0 };
	const u8 *recs[2] = { f0, f1 };
	int i, count;

	if (spx4_tp_read_reg(ts->client, SPX4_TP_REG_HEADER, hdr, sizeof(hdr)))
		return IRQ_HANDLED;

	if (hdr[1] & 0x18)
		return IRQ_HANDLED;

	count = hdr[0];
	if (count > SPX4_TP_MAX_SLOTS)
		count = SPX4_TP_MAX_SLOTS;

	if (spx4_tp_read_reg(ts->client, SPX4_TP_REG_FIFO, f0, sizeof(f0)))
		return IRQ_HANDLED;
	if (spx4_tp_read_reg(ts->client, SPX4_TP_REG_FIFO, f1, sizeof(f1)))
		return IRQ_HANDLED;

	for (i = 0; i < SPX4_TP_MAX_SLOTS; i++) {
		const u8 *r = recs[i];
		bool active = (i < count) && !!(r[0] & 0x80);
		int slot = ((r[0] & 0x0F) == 1) ? 0 : (count == 1 ? 1 : i);
		u16 x = r[1] | ((u16)r[2] << 8);
		u16 y = r[3] | ((u16)r[4] << 8);

		if (slot >= SPX4_TP_MAX_SLOTS)
			slot = i;

		input_mt_slot(ts->input, slot);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, active);
		if (active) {
			if (x >= ts->max_x)
				x = ts->max_x - 1;
			if (y >= ts->max_y)
				y = ts->max_y - 1;
			input_report_abs(ts->input, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
		}
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	return IRQ_HANDLED;
}

static int fuji_spx4_touch_probe(struct i2c_client *client)
{
	struct fuji_spx4_touch *ts;
	int ret;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->max_x = SPX4_TP_WIDTH;
	ts->max_y = SPX4_TP_HEIGHT;
	of_property_read_u32(client->dev.of_node, "touchscreen-size-x", &ts->max_x);
	of_property_read_u32(client->dev.of_node, "touchscreen-size-y", &ts->max_y);

	ts->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return PTR_ERR(ts->reset_gpio);

	ts->input = devm_input_allocate_device(&client->dev);
	if (!ts->input)
		return -ENOMEM;

	ts->input->name = "fuji-spx4-touchscreen";
	ts->input->phys = "i2c2/0x55/input0";
	ts->input->id.bustype = BUS_I2C;
	ts->input->dev.parent = &client->dev;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y - 1, 0, 0);
	ret = input_mt_init_slots(ts->input, SPX4_TP_MAX_SLOTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, spx4_tp_irq_thread,
						IRQF_ONESHOT,
						"fuji-spx4-touchscreen", ts);
		if (ret)
			return ret;
	}

	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	i2c_set_clientdata(client, ts);
	dev_info(&client->dev,
		 "SPX4-MF Medium-Format Camera touchscreen registered (%ux%u, I2C2@0x%02x, irq=%d)\n",
		 ts->max_x, ts->max_y, client->addr, client->irq);
	return 0;
}

static const struct i2c_device_id fuji_spx4_touch_id[] = {
	{ "fuji-spx4-touch", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fuji_spx4_touch_id);

static const struct of_device_id fuji_spx4_touch_of_match[] = {
	{ .compatible = "fujisan,spx4-touchscreen" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_touch_of_match);

static struct i2c_driver fuji_spx4_touch_driver = {
	.probe		= fuji_spx4_touch_probe,
	.id_table	= fuji_spx4_touch_id,
	.driver = {
		.name		= "fuji-spx4-touchscreen",
		.of_match_table	= fuji_spx4_touch_of_match,
	},
};
module_i2c_driver(fuji_spx4_touch_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera Rear Capacitive Multi-Touchscreen Driver");
MODULE_LICENSE("GPL");
