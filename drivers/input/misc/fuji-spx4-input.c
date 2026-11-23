// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPX4-MF Medium-Format Camera Physical Body Keys, 8-Way Focus Stick, Mode Dial,
 * Command Dials & 3.5mm Audio Jack Switch Input Driver
 *
 * Wave-3 Hardware Alignment (DAT @ section4.bin 0x011F5D48, 0x0123C860..0x01240738):
 *   - Uses interleaved 0x555/0xAAA 12-channel SAR ADC @ 0xFFF40000 (strictly
 *     separate from soc-keyed-io @ 0xFFF42000).
 *   - Registers two standard Linux evdev input devices:
 *       1. "fuji-spx4-keys":
 *          - 8-Way Focus Stick (ADC CH9 + GPIO 7.9 active-LOW)
 *          - Rear ADC CH3 ladder: MENU/OK (KEY_ENTER), Q (KEY_PROG1), AE-L (KEY_LEFTALT)
 *          - Top  ADC CH6 ladder: Fn1 (KEY_F1), Fn2 (KEY_F2), Fn3 (KEY_F3), LIGHT (KEY_BRIGHTNESSUP)
 *          - 10-Position Mode Dial (ADC CH5: M, C1..C6, P, S, A -> sysfs mode_dial)
 *          - GPIO direct buttons (all active-LOW): DISP/BACK, PLAY, DELETE, AF-ON,
 *            DRIVE, Fn4, VIEW MODE, SHUTTER S1, SHUTTER S2, STILL/MOVIE (7.5)
 *          - Audio 3.5mm Jack switches (GPIO 6.4 Mic, GPIO 8.4 Headphone)
 *       2. "fuji-spx4-dials":
 *          - Front Command Dial (GPIO 1.2/1.3 quadrature -> REL_HWHEEL, push 2.2 -> BTN_MIDDLE)
 *          - Rear  Command Dial (ADC CH7/CH11 quadrature -> REL_WHEEL,  push 0.13 -> BTN_RIGHT)
 */

#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "fuji_spx4_hw.h"

extern int fuji_spx4_adc_read_all(u16 samples[SPX4_ADC_CH_COUNT]);

#define SPX4_GPIO_BASE_FALLBACK	0xFFF41000U
#define SPX4_GPIO_IN(bank)	(((bank) * 0x40) + 0x14)

struct spx4_gpio_key_def {
	u8 bank;
	u8 pin;
	u16 keycode;
	bool active_low;
	bool is_dial_btn;
};

static const struct spx4_gpio_key_def spx4_gpio_keys[] = {
	{ 9,  5, KEY_ESC,             true, false }, /* DISP/BACK */
	{ 9,  6, KEY_PLAYPAUSE,       true, false }, /* PLAY */
	{ 6,  2, KEY_BACKSPACE,       true, false }, /* DELETE */
	{ 0, 14, KEY_LEFTMETA,        true, false }, /* AF-ON -> Super/Mod */
	{ 6,  1, KEY_PROG2,           true, false }, /* DRIVE */
	{ 2, 13, KEY_F4,              true, false }, /* Fn4 */
	{ 7,  6, KEY_SWITCHVIDEOMODE, true, false }, /* VIEW MODE */
	{ 0,  0, KEY_CAMERA_FOCUS,    true, false }, /* SHUTTER S1 */
	{ 0,  1, KEY_CAMERA,          true, false }, /* SHUTTER S2 */
	{ 2,  2, BTN_MIDDLE,          true, true  }, /* Front Dial Push */
	{ 0, 13, BTN_RIGHT,           true, true  }, /* Rear Dial Push */
};

static const char *const spx4_mode_dial_names[] = {
	"M", "C1", "C2", "C3", "C4", "C5", "C6", "P", "S", "A",
};

/* DAT 4x4 quadrature transition matrices (0x01812F90 front, 0x01813004 rear) */
static const s8 quad_front[16] = {
	 0, -1,  1,  0,
	 1,  0,  0, -1,
	-1,  0,  0,  1,
	 0,  1, -1,  0,
};

static const s8 quad_rear[16] = {
	 0,  1, -1,  0,
	-1,  0,  0,  1,
	 1,  0,  0, -1,
	 0, -1,  1,  0,
};

struct fuji_spx4_input {
	struct device *dev;
	void __iomem *gpio_base;
	struct input_dev *keys_dev;
	struct input_dev *dials_dev;
	struct delayed_work key_work;
	struct delayed_work dial_work;
	bool armed_gpio[ARRAY_SIZE(spx4_gpio_keys)];
	bool last_gpio[ARRAY_SIZE(spx4_gpio_keys)];
	u8 front_q;
	u8 rear_q;
	bool rear_a;
	bool rear_b;
	bool last_hp;
	bool last_mic;
	bool last_movie_sw;
	int mode_dial;
};

static inline bool spx4_read_pin(struct fuji_spx4_input *inp, u8 bank, u8 pin)
{
	u32 v = readl_relaxed(inp->gpio_base + SPX4_GPIO_IN(bank));

	return !!(v & BIT(pin));
}

static void spx4_decode_ch3_ladder(struct input_dev *dev, u16 v)
{
	bool q = false, ok = false, ael = false;

	if (v >= 3103 && v <= 3776) {
		q = true;
	} else if (v >= 2637 && v <= 3102) {
		ok = true;
	} else if (v >= 2345 && v <= 2636) {
		q = true;
		ok = true;
	} else if (v >= 2117 && v <= 2344) {
		ael = true;
	} else if (v >= 1894 && v <= 2116) {
		q = true;
		ael = true;
	} else if (v >= 1712 && v <= 1893) {
		ok = true;
		ael = true;
	}

	input_report_key(dev, KEY_PROG1, q);
	input_report_key(dev, KEY_ENTER, ok);
	input_report_key(dev, KEY_LEFTALT, ael);
}

static void spx4_decode_ch6_ladder(struct input_dev *dev, u16 v)
{
	bool fn1 = false, fn2 = false, fn3 = false, light = false;

	if (v >= 3103 && v <= 3776) {
		fn1 = true;
	} else if (v >= 2637 && v <= 3102) {
		fn3 = true;
	} else if (v >= 2345 && v <= 2636) {
		fn1 = true;
		fn3 = true;
	} else if (v >= 2117 && v <= 2344) {
		fn2 = true;
	} else if (v >= 1894 && v <= 2116) {
		fn1 = true;
		fn2 = true;
	} else if (v >= 1712 && v <= 1893) {
		fn2 = true;
		fn3 = true;
	} else if (v <= 782) {
		light = true;
	}

	input_report_key(dev, KEY_F1, fn1);
	input_report_key(dev, KEY_F2, fn2);
	input_report_key(dev, KEY_F3, fn3);
	input_report_key(dev, KEY_BRIGHTNESSUP, light);
}

static void spx4_decode_focus_stick(struct input_dev *dev, bool gate_low, u16 v)
{
	bool up = false, dn = false, lt = false, rt = false, press = false;

	if (gate_low) {
		if (v >= 2606 && v <= 3041)
			dn = true;
		else if (v >= 3042 && v <= 3719)
			lt = true;
		else if (v >= 2220 && v <= 2605) {
			dn = true;
			lt = true;
		} else if (v >= 1448 && v <= 1688)
			rt = true;
		else if (v >= 1231 && v <= 1447) {
			dn = true;
			rt = true;
		} else if (v >= 1930 && v <= 2219)
			up = true;
		else if (v >= 1689 && v <= 1929) {
			up = true;
			lt = true;
		} else if (v >= 100 && v <= 1230) {
			up = true;
			rt = true;
		} else if (v > 3719 || v < 100)
			press = true;
	}

	input_report_key(dev, KEY_UP, up);
	input_report_key(dev, KEY_DOWN, dn);
	input_report_key(dev, KEY_LEFT, lt);
	input_report_key(dev, KEY_RIGHT, rt);
	if (press)
		input_report_key(dev, KEY_ENTER, true);
}

static void spx4_key_poll(struct work_struct *work)
{
	struct fuji_spx4_input *inp = container_of(to_delayed_work(work),
						   struct fuji_spx4_input, key_work);
	u16 adc[SPX4_ADC_CH_COUNT] = { 0 };
	bool adc_ok = (fuji_spx4_adc_read_all(adc) == 0);
	bool dial_changed = false;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(spx4_gpio_keys); i++) {
		const struct spx4_gpio_key_def *k = &spx4_gpio_keys[i];
		bool level = spx4_read_pin(inp, k->bank, k->pin);
		bool pressed = k->active_low ? !level : level;

		if (!inp->armed_gpio[i]) {
			if (!pressed)
				inp->armed_gpio[i] = true;
			continue;
		}

		if (pressed != inp->last_gpio[i]) {
			inp->last_gpio[i] = pressed;
			if (k->is_dial_btn) {
				input_report_key(inp->dials_dev, k->keycode, pressed);
				dial_changed = true;
			} else {
				input_report_key(inp->keys_dev, k->keycode, pressed);
			}
		}
	}

	/* STILL / MOVIE lever (GPIO 7.5 edge -> KEY_RECORD pulse) */
	{
		bool movie_sw = spx4_read_pin(inp, 7, 5);

		if (movie_sw != inp->last_movie_sw) {
			inp->last_movie_sw = movie_sw;
			input_report_key(inp->keys_dev, KEY_RECORD, true);
			input_sync(inp->keys_dev);
			input_report_key(inp->keys_dev, KEY_RECORD, false);
		}
	}

	if (adc_ok) {
		bool stick_gate = !spx4_read_pin(inp, 7, 9);

		spx4_decode_ch3_ladder(inp->keys_dev, adc[3]);
		spx4_decode_ch6_ladder(inp->keys_dev, adc[6]);
		spx4_decode_focus_stick(inp->keys_dev, stick_gate, adc[9]);
		inp->mode_dial = spx4_adc_decode_mode_dial(adc[5]);
	}

	/* 3.5mm Audio Jack Switches (GPIO 6.4 ext-mic active-LOW, GPIO 8.4 HP) */
	{
		bool mic = !spx4_read_pin(inp, 6, 4);
		bool hp = spx4_read_pin(inp, 8, 4);

		if (mic != inp->last_mic) {
			inp->last_mic = mic;
			input_report_switch(inp->keys_dev, SW_MICROPHONE_INSERT, mic);
		}
		if (hp != inp->last_hp) {
			inp->last_hp = hp;
			input_report_switch(inp->keys_dev, SW_HEADPHONE_INSERT, hp);
		}
	}

	input_sync(inp->keys_dev);
	if (dial_changed)
		input_sync(inp->dials_dev);

	schedule_delayed_work(&inp->key_work, msecs_to_jiffies(25));
}

static void spx4_dial_poll(struct work_struct *work)
{
	struct fuji_spx4_input *inp = container_of(to_delayed_work(work),
						   struct fuji_spx4_input, dial_work);
	u8 fcur = (spx4_read_pin(inp, 1, 2) ? 2 : 0) |
		  (spx4_read_pin(inp, 1, 3) ? 1 : 0);
	s8 fstep = quad_front[(inp->front_q << 2) | fcur];
	u16 adc[SPX4_ADC_CH_COUNT];
	s8 rstep = 0;

	inp->front_q = fcur;

	if (fuji_spx4_adc_read_all(adc) == 0) {
		if (adc[7] > 2560)
			inp->rear_a = true;
		else if (adc[7] < 1536)
			inp->rear_a = false;

		if (adc[11] > 2560)
			inp->rear_b = true;
		else if (adc[11] < 1536)
			inp->rear_b = false;

		{
			u8 rcur = (inp->rear_a ? 2 : 0) | (inp->rear_b ? 1 : 0);

			rstep = quad_rear[(inp->rear_q << 2) | rcur];
			inp->rear_q = rcur;
		}
	}

	if (fstep || rstep) {
		if (fstep)
			input_report_rel(inp->dials_dev, REL_HWHEEL, fstep);
		if (rstep)
			input_report_rel(inp->dials_dev, REL_WHEEL, rstep);
		input_sync(inp->dials_dev);
	}

	schedule_delayed_work(&inp->dial_work, msecs_to_jiffies(10));
}

static ssize_t mode_dial_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fuji_spx4_input *inp = dev_get_drvdata(dev);
	int pos = inp->mode_dial;

	if (pos >= 0 && pos < ARRAY_SIZE(spx4_mode_dial_names))
		return sysfs_emit(buf, "%s\n", spx4_mode_dial_names[pos]);
	return sysfs_emit(buf, "UNKNOWN\n");
}
static DEVICE_ATTR_RO(mode_dial);

static struct attribute *spx4_input_attrs[] = {
	&dev_attr_mode_dial.attr,
	NULL,
};
ATTRIBUTE_GROUPS(spx4_input);

static int fuji_spx4_input_probe(struct platform_device *pdev)
{
	struct fuji_spx4_input *inp;
	size_t i;
	int ret;

	inp = devm_kzalloc(&pdev->dev, sizeof(*inp), GFP_KERNEL);
	if (!inp)
		return -ENOMEM;

	inp->dev = &pdev->dev;
	inp->mode_dial = SPX4_MODE_UNKNOWN;
	inp->gpio_base = devm_ioremap(&pdev->dev, SPX4_GPIO_BASE_FALLBACK, 0x400);
	if (!inp->gpio_base)
		return -ENOMEM;

	/* Arm active-LOW pins that are already idle HIGH at probe time */
	for (i = 0; i < ARRAY_SIZE(spx4_gpio_keys); i++) {
		const struct spx4_gpio_key_def *k = &spx4_gpio_keys[i];
		bool level = spx4_read_pin(inp, k->bank, k->pin);
		bool pressed = k->active_low ? !level : level;

		inp->armed_gpio[i] = !pressed;
	}
	inp->front_q = (spx4_read_pin(inp, 1, 2) ? 2 : 0) |
		       (spx4_read_pin(inp, 1, 3) ? 1 : 0);
	inp->last_mic = !spx4_read_pin(inp, 6, 4);
	inp->last_hp = spx4_read_pin(inp, 8, 4);
	inp->last_movie_sw = spx4_read_pin(inp, 7, 5);

	/* 1. Body keys & 8-way focus stick */
	inp->keys_dev = devm_input_allocate_device(&pdev->dev);
	if (!inp->keys_dev)
		return -ENOMEM;

	inp->keys_dev->name = "fuji-spx4-keys";
	inp->keys_dev->phys = "spx4/input0";
	inp->keys_dev->id.bustype = BUS_HOST;
	inp->keys_dev->dev.parent = &pdev->dev;

	__set_bit(EV_KEY, inp->keys_dev->evbit);
	__set_bit(EV_REP, inp->keys_dev->evbit);
	__set_bit(EV_SW, inp->keys_dev->evbit);

	for (i = 0; i < ARRAY_SIZE(spx4_gpio_keys); i++) {
		if (!spx4_gpio_keys[i].is_dial_btn)
			input_set_capability(inp->keys_dev, EV_KEY, spx4_gpio_keys[i].keycode);
	}
	input_set_capability(inp->keys_dev, EV_KEY, KEY_UP);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_DOWN);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_LEFT);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_RIGHT);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_ENTER);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_PROG1);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_LEFTALT);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_F1);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_F2);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_F3);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_BRIGHTNESSUP);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_RECORD);
	input_set_capability(inp->keys_dev, EV_KEY, KEY_POWER);
	input_set_capability(inp->keys_dev, EV_SW, SW_HEADPHONE_INSERT);
	input_set_capability(inp->keys_dev, EV_SW, SW_MICROPHONE_INSERT);

	ret = input_register_device(inp->keys_dev);
	if (ret)
		return ret;

	/* 2. Front & Rear Command Dials */
	inp->dials_dev = devm_input_allocate_device(&pdev->dev);
	if (!inp->dials_dev)
		return -ENOMEM;

	inp->dials_dev->name = "fuji-spx4-dials";
	inp->dials_dev->phys = "spx4/input1";
	inp->dials_dev->id.bustype = BUS_HOST;
	inp->dials_dev->dev.parent = &pdev->dev;

	input_set_capability(inp->dials_dev, EV_REL, REL_HWHEEL);
	input_set_capability(inp->dials_dev, EV_REL, REL_WHEEL);
	input_set_capability(inp->dials_dev, EV_KEY, BTN_MIDDLE);
	input_set_capability(inp->dials_dev, EV_KEY, BTN_RIGHT);

	ret = input_register_device(inp->dials_dev);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&inp->key_work, spx4_key_poll);
	INIT_DELAYED_WORK(&inp->dial_work, spx4_dial_poll);
	schedule_delayed_work(&inp->key_work, msecs_to_jiffies(50));
	schedule_delayed_work(&inp->dial_work, msecs_to_jiffies(50));

	platform_set_drvdata(pdev, inp);
	dev_info(&pdev->dev,
		 "SPX4-MF Medium-Format Camera physical controls registered (fuji-spx4-keys + fuji-spx4-dials + mode_dial)\n");
	return 0;
}

static int fuji_spx4_input_remove(struct platform_device *pdev)
{
	struct fuji_spx4_input *inp = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&inp->key_work);
	cancel_delayed_work_sync(&inp->dial_work);
	return 0;
}

static const struct of_device_id fuji_spx4_input_of_match[] = {
	{ .compatible = "fujisan,spx4-mf-input" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_input_of_match);

static struct platform_driver fuji_spx4_input_driver = {
	.probe	= fuji_spx4_input_probe,
	.remove	= fuji_spx4_input_remove,
	.driver	= {
		.name		= "fuji-spx4-input",
		.of_match_table	= fuji_spx4_input_of_match,
		.dev_groups	= spx4_input_groups,
	},
};
module_platform_driver(fuji_spx4_input_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera Body Keys, 8-Way Focus Stick, Mode Dial & Command Dials Driver");
MODULE_LICENSE("GPL");
