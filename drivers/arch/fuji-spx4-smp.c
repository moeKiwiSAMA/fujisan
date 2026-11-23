// SPDX-License-Identifier: GPL-2.0-only
/*
 * Quad-Core Cortex-A7 SMP Bring-Up Operations for Fujisan SPX4 (SPX4-MF)
 *
 * Verified from SPX4-MF 2.13 Boot ROM (0xFFF00294) & boot.bin (0x000005CC):
 *   - Secondary cores 1..3 park in WFI waiting on GIC SGIs 1..3.
 *   - Wakeup mailbox registers: 0xFFF4A104 (core1), 0xFFF4A108 (core2), 0xFFF4A10C (core3).
 *   - On real 4-core silicon, writing secondary_startup PA + firing GIC SGI wakes cores 1..3.
 *   - When booted with "nosmp" (e.g. single-core emulator runs), secondary boot is skipped cleanly.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <asm/cacheflush.h>
#include <asm/smp_plat.h>

#define SPX4_SMP_MAILBOX_BASE	0xFFF4A100U
#define SPX4_SMP_CORE_REG(cpu)	((cpu) * 4U)

static void __iomem *spx4_smp_base;

static void __init fuji_spx4_smp_prepare_cpus(unsigned int max_cpus)
{
	if (!spx4_smp_base)
		spx4_smp_base = ioremap(SPX4_SMP_MAILBOX_BASE, 0x20);
}

static int fuji_spx4_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	if (!spx4_smp_base || cpu == 0 || cpu >= 4)
		return -EINVAL;

	writel_relaxed(__pa_symbol(secondary_startup),
		       spx4_smp_base + SPX4_SMP_CORE_REG(cpu));
	dsb_sev();
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return 0;
}

static const struct smp_operations fuji_spx4_smp_ops __initconst = {
	.smp_prepare_cpus	= fuji_spx4_smp_prepare_cpus,
	.smp_boot_secondary	= fuji_spx4_boot_secondary,
};

CPU_METHOD_OF_DECLARE(fuji_spx4_smp, "fujisan,spx4-smp", &fuji_spx4_smp_ops);
