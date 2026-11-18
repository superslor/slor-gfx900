// SPDX-License-Identifier: GPL-2.0
/*
 * vega10_avfs_keep: keep AVFS on for a Vega 10 card with a custom PowerPlay table. amdgpu turns AVFS off whenever
 * a table is written to pp_table, which drops the chip to the SMU's static voltages (~140 mV higher at 1 GHz). This
 * takes AVFS out of every DisableSmuFeatures message (vega10_enable_smc_features) and leaves the driver's own
 * bookkeeping alone, so a reset re-enables it as usual. Load it before the table is written. "flips" counts the
 * messages it changed.
 */
#include <linux/module.h>
#include <linux/kprobes.h>

#define AVFS_FEATURE	(1u << 10)	/* data->smu_features[GNLD_AVFS].smu_feature_bitmap, GNLD_AVFS = 10 */

static unsigned int flips;
module_param(flips, uint, 0444);
MODULE_PARM_DESC(flips, "DisableSmuFeatures messages that had AVFS taken out");

/* vega10_enable_smc_features(struct pp_hwmgr *hwmgr, bool enable, uint32_t feature_mask): x86-64 rdi, rsi, rdx */
static int vega10_avfs_keep_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 mask = (u32)regs->dx;

	if (!(regs->si & 0xff) && (mask & AVFS_FEATURE)) {
		mask &= ~AVFS_FEATURE;
		if (!mask) {		/* AVFS alone: send "enable AVFS" instead, which is a no-op when it is on */
			regs->si = 1;
			mask = AVFS_FEATURE;
		}
		regs->dx = mask;
		flips++;
	}
	return 0;
}

static struct kprobe kp = {
	.symbol_name = "vega10_enable_smc_features",
	.pre_handler = vega10_avfs_keep_pre,
};

static int __init vega10_avfs_keep_init(void)
{
	int ret = register_kprobe(&kp);

	if (ret)
		pr_err("vega10_avfs_keep: cannot probe vega10_enable_smc_features (is amdgpu loaded?): %d\n", ret);
	else
		pr_info("vega10_avfs_keep: AVFS stays on with a custom pp_table\n");
	return ret;
}

static void __exit vega10_avfs_keep_exit(void)
{
	unregister_kprobe(&kp);
}

module_init(vega10_avfs_keep_init);
module_exit(vega10_avfs_keep_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Keep Vega 10 AVFS on when a custom PowerPlay table is loaded");
MODULE_SOFTDEP("pre: amdgpu");
