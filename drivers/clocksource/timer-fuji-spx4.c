// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clocksource and clockevent driver for Fujisan SPX4 54 MHz Interval Timer
 * (SPX4-MF soc-timer @ 0xFFF3C300, STRICT Channel 7 ONLY -> GIC SPI 64 edge).
 *
 * Register layout (verified from SPX4-MF 2.13 kernel.bin & soc-timer):
 *   +0x00  CTRL    0x00800000 (Ch7 disable) / 0x00800080 (Ch7 enable)
 *   +0x48  COUNT   32-bit down-counter for Ch7 (written 3x for APB sync)
 *   +0x4C  RELOAD  32-bit auto-reload for Ch7 (written 3x for APB sync)
 *
 * Hardware invariant: NEVER touch Ch0..Ch6 (+0x10..+0x44 or bits[22:16]/[6:0]).
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <linux/spinlock.h>

#include "fuji_spx4_hw.h"

#define SPX4_DEFAULT_FREQ	54000000U
#define SPX4_MAX_RELOAD		0x0CDFE600U /* 4.0 s @ 54 MHz */

struct fuji_spx4_timer {
	void __iomem *base;
	void __iomem *lcdc_commit;
	u32 freq;
	u32 cur_reload;
	u32 lcdc_div;
	u64 epoch_ticks;
	raw_spinlock_t lock;
	struct clock_event_device clkevt;
};

static struct fuji_spx4_timer spx4_tmr;
static atomic_t spx4_lcdc_heartbeat_en = ATOMIC_INIT(1);

void fuji_spx4_timer_set_lcdc_heartbeat(bool enable)
{
	atomic_set(&spx4_lcdc_heartbeat_en, enable ? 1 : 0);
}
EXPORT_SYMBOL_GPL(fuji_spx4_timer_set_lcdc_heartbeat);

static inline void spx4_timer_ch7_enable(struct fuji_spx4_timer *t, bool en)
{
	writel_relaxed(spx4_timer_ch7_ctrl_val(en), t->base + SPX4_TIMER_REG_CTRL);
}

static inline void spx4_timer_program(struct fuji_spx4_timer *t, u32 reload)
{
	if (reload == 0)
		reload = 1;
	spx4_timer_ch7_enable(t, false);
	mb();
	t->cur_reload = reload;
	/*
	 * Stock SPX4-MF 2.13 kernel.bin (0x0005A880..0x0005A8C4) writes Ch7 COUNT
	 * (+0x48) and RELOAD (+0x4C) 3x each for the SPX4 54 MHz APB synchronizer.
	 */
	writel_relaxed(reload, t->base + SPX4_TIMER_CH7_COUNT_OFF);
	writel_relaxed(reload, t->base + SPX4_TIMER_CH7_COUNT_OFF);
	writel_relaxed(reload, t->base + SPX4_TIMER_CH7_COUNT_OFF);
	mb();
	writel_relaxed(reload, t->base + SPX4_TIMER_CH7_RELOAD_OFF);
	writel_relaxed(reload, t->base + SPX4_TIMER_CH7_RELOAD_OFF);
	writel_relaxed(reload, t->base + SPX4_TIMER_CH7_RELOAD_OFF);
	mb();
	spx4_timer_ch7_enable(t, true);
	mb();
}

static u64 spx4_read_ticks_locked(struct fuji_spx4_timer *t)
{
	u32 cnt = readl_relaxed(t->base + SPX4_TIMER_CH7_COUNT_OFF);
	u32 delta = (cnt <= t->cur_reload) ? (t->cur_reload - cnt) : 0;

	return t->epoch_ticks + delta;
}

static u64 notrace spx4_sched_clock_read(void)
{
	unsigned long flags;
	u64 ticks;

	raw_spin_lock_irqsave(&spx4_tmr.lock, flags);
	ticks = spx4_read_ticks_locked(&spx4_tmr);
	raw_spin_unlock_irqrestore(&spx4_tmr.lock, flags);
	return ticks;
}

static u64 spx4_cs_read(struct clocksource *cs)
{
	return spx4_sched_clock_read();
}

static struct clocksource spx4_clocksource = {
	.name	= "fuji-spx4-timer",
	.rating	= 300,
	.read	= spx4_cs_read,
	.mask	= CLOCKSOURCE_MASK(64),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int spx4_set_state_shutdown(struct clock_event_device *evt)
{
	struct fuji_spx4_timer *t = container_of(evt, struct fuji_spx4_timer, clkevt);
	unsigned long flags;

	raw_spin_lock_irqsave(&t->lock, flags);
	t->epoch_ticks = spx4_read_ticks_locked(t);
	spx4_timer_ch7_enable(t, false);
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return 0;
}

static int spx4_set_state_periodic(struct clock_event_device *evt)
{
	struct fuji_spx4_timer *t = container_of(evt, struct fuji_spx4_timer, clkevt);
	unsigned long flags;
	u32 reload = (t->freq / HZ) - 1;

	raw_spin_lock_irqsave(&t->lock, flags);
	t->epoch_ticks = spx4_read_ticks_locked(t);
	spx4_timer_program(t, reload);
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return 0;
}

static int spx4_set_state_oneshot(struct clock_event_device *evt)
{
	return spx4_set_state_shutdown(evt);
}

static int spx4_set_next_event(unsigned long delta, struct clock_event_device *evt)
{
	struct fuji_spx4_timer *t = container_of(evt, struct fuji_spx4_timer, clkevt);
	unsigned long flags;
	u32 reload = (delta > 0) ? (u32)(delta - 1) : 1;

	if (reload > SPX4_MAX_RELOAD)
		reload = SPX4_MAX_RELOAD;

	raw_spin_lock_irqsave(&t->lock, flags);
	t->epoch_ticks = spx4_read_ticks_locked(t);
	spx4_timer_program(t, reload);
	raw_spin_unlock_irqrestore(&t->lock, flags);
	return 0;
}

static irqreturn_t spx4_timer_interrupt(int irq, void *dev_id)
{
	struct fuji_spx4_timer *t = dev_id;
	unsigned long flags;

	raw_spin_lock_irqsave(&t->lock, flags);
	t->epoch_ticks += (u64)t->cur_reload + 1ULL;
	if (clockevent_state_oneshot(&t->clkevt))
		spx4_timer_ch7_enable(t, false);
	if (t->lcdc_commit && atomic_read(&spx4_lcdc_heartbeat_en)) {
		if (++t->lcdc_div >= 3) {
			t->lcdc_div = 0;
			writel_relaxed(1, t->lcdc_commit);
		}
	}
	raw_spin_unlock_irqrestore(&t->lock, flags);

	if (t->clkevt.event_handler)
		t->clkevt.event_handler(&t->clkevt);

	return IRQ_HANDLED;
}

static irqreturn_t spx4_lcdc_vsync_interrupt(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

static int __init fuji_spx4_timer_init(struct device_node *np)
{
	struct fuji_spx4_timer *t = &spx4_tmr;
	u32 freq = SPX4_DEFAULT_FREQ;
	u32 dt_ch = SPX4_TIMER_CH7_INDEX;
	u32 commit_pa = 0;
	int irq, lcdc_irq, ret;

	raw_spin_lock_init(&t->lock);

	t->base = of_iomap(np, 0);
	if (!t->base) {
		pr_err("fuji-spx4-timer: failed to map MMIO\n");
		return -ENOMEM;
	}

	of_property_read_u32(np, "clock-frequency", &freq);
	if (!of_property_read_u32(np, "fujisan,timer-channel", &dt_ch) &&
	    dt_ch != SPX4_TIMER_CH7_INDEX) {
		pr_warn("fuji-spx4-timer: ignoring DT channel %u; enforcing Ch7-only invariant\n",
			dt_ch);
	}

	t->freq = freq;
	t->cur_reload = (freq / HZ) - 1;
	t->epoch_ticks = 0;

	/*
	 * Configure optional LCDC1 VSYNC edge-triggered IRQ (GIC SPI 118)
	 * BEFORE any write to 0xFFFA9600 so GICD_ICFGR is rising-edge from t=0.
	 */
	lcdc_irq = irq_of_parse_and_map(np, 1);
	if (lcdc_irq > 0) {
		if (request_irq(lcdc_irq, spx4_lcdc_vsync_interrupt,
				IRQF_SHARED, "fuji-lcdc1-vsync-early", t))
			pr_warn("fuji-spx4-timer: optional LCDC1 irq %d request failed\n", lcdc_irq);
	}

	if (!of_property_read_u32(np, "fujisan,lcdc-commit-reg", &commit_pa) && commit_pa) {
		t->lcdc_commit = ioremap(commit_pa, 4);
		if (t->lcdc_commit)
			writel_relaxed(1, t->lcdc_commit);
	}

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("fuji-spx4-timer: failed to map IRQ\n");
		return -EINVAL;
	}

	spx4_timer_ch7_enable(t, false);

	t->clkevt.name = "fuji-spx4-timer";
	t->clkevt.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	t->clkevt.rating = 300;
	t->clkevt.cpumask = cpumask_of(0);
	t->clkevt.irq = irq;
	t->clkevt.set_state_shutdown = spx4_set_state_shutdown;
	t->clkevt.set_state_periodic = spx4_set_state_periodic;
	t->clkevt.set_state_oneshot = spx4_set_state_oneshot;
	t->clkevt.set_next_event = spx4_set_next_event;

	ret = request_irq(irq, spx4_timer_interrupt,
			  IRQF_TIMER | IRQF_IRQPOLL,
			  "fuji-spx4-timer", t);
	if (ret) {
		pr_err("fuji-spx4-timer: request_irq(%d) failed: %d\n", irq, ret);
		return ret;
	}

	clocksource_register_hz(&spx4_clocksource, t->freq);
	sched_clock_register(spx4_sched_clock_read, 64, t->freq);
	clockevents_config_and_register(&t->clkevt, t->freq, 0x10, SPX4_MAX_RELOAD);

	pr_info("fuji-spx4-timer: ch7 @ %u Hz (irq %d, lcdc_commit=0x%08x)\n",
		t->freq, irq, commit_pa);
	return 0;
}

TIMER_OF_DECLARE(fuji_spx4_timer, "fujisan,spx4-timer", fuji_spx4_timer_init);
