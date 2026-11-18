// SPDX-License-Identifier: GPL-2.0
/*
 * vega10_cu_unlock: clear the user CU mask (GC_USER_SHADER_ARRAY_CONFIG) a Vega 10 VBIOS sets at POST, just
 * before amdgpu reads it in gfx_v9_0_get_cu_info, so the driver brings up every unfused CU. Clearing it while
 * the GPU runs hangs the first waves sent to the new CUs. Fused-off CUs stay off. Loaded ahead of amdgpu by a
 * modprobe softdep. "unlocks" counts the inits it changed.
 */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/uaccess.h>

#define GRBM_GFX_INDEX      (0xC200 * 4)    /* GC seg1 0xA000 + 0x2200 */
#define GC_USER_SA_CONFIG   (0x2270 * 4)    /* GC seg0 0x2000 + 0x0270 */
#define CC_GC_SA_CONFIG     (0x226F * 4)
#define GRBM_BROADCAST_ALL  0xE0000000u     /* SE, SH and instance broadcast */
#define NUM_SE              4
#define MAX_CARDS           8

static unsigned int unlocks;
module_param(unlocks, uint, 0444);
MODULE_PARM_DESC(unlocks, "gfx inits whose user CU mask was cleared");

static struct pci_dev * pdevs[MAX_CARDS];
static void __iomem * mmios[MAX_CARDS];
static int ncards;
static bool probe_on, guessed;

/* gfx_v9_0_get_cu_info is an .isra clone, so its argument list is not certain: look for amdgpu_device (dev, pdev
 * as its first two fields) in the first four argument registers, reading through a fault-safe copy. */
static int which_card(struct pt_regs *regs)
{
	unsigned long args[4] = { regs->di, regs->si, regs->dx, regs->cx };
	void * head[2];
	int a, i;

	for (a = 0; a < 4; ++a) {
		if (copy_from_kernel_nofault(head, (void *) args[a], sizeof(head)))
			continue;
		for (i = 0; i < ncards; ++i)
			if (head[1] == pdevs[i] && head[0] == &pdevs[i]->dev)
				return i;
	}
	return -1;
}

static int cu_unlock_pre(struct kprobe *p, struct pt_regs *regs)
{
	int se, i = which_card(regs);
	void __iomem * mmio;
	u32 before = 0;

	if (i < 0) {
		if (!guessed)
			pr_warn("vega10_cu_unlock: cannot tell which card is starting - first card only (as 1.0)\n");
		guessed = true;
		i = 0;
	}
	mmio = mmios[i];
	for (se = 0; se < NUM_SE; ++se) {
		writel((u32) se << 16 | 1u << 30, mmio + GRBM_GFX_INDEX);   /* SE se, SH 0, instance broadcast */
		before |= readl(mmio + GC_USER_SA_CONFIG);
		writel(0, mmio + GC_USER_SA_CONFIG);
	}
	writel(GRBM_BROADCAST_ALL, mmio + GRBM_GFX_INDEX);
	if (before) {
		unlocks++;
		pr_info("vega10_cu_unlock: %s: user CU mask %#x cleared\n", pci_name(pdevs[i]), before);
	}
	return 0;
}

static struct kprobe kp = {
	.symbol_name = "gfx_v9_0_get_cu_info.isra.0",
	.pre_handler = cu_unlock_pre,
};

static void cu_unlock_arm(void)
{
	int ret;

	if (probe_on)
		return;
	ret = register_kprobe(&kp);
	if (ret) {
		pr_err("vega10_cu_unlock: cannot probe %s: %d\n", kp.symbol_name, ret);
		return;
	}
	probe_on = true;
	pr_info("vega10_cu_unlock: armed\n");
}

static int cu_unlock_module_cb(struct notifier_block *nb, unsigned long state, void *data)
{
	struct module *mod = data;

	if (state == MODULE_STATE_COMING && !strcmp(mod->name, "amdgpu"))
		cu_unlock_arm();
	return NOTIFY_DONE;
}

static struct notifier_block cu_unlock_nb = { .notifier_call = cu_unlock_module_cb };

static int __init vega10_cu_unlock_init(void)
{
	struct pci_dev * pdev = NULL;
	void __iomem * mmio;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_ATI, 0x6860, pdev)) && ncards < MAX_CARDS) {   /* Vega 10 (MI25) */
		mmio = pci_resource_len(pdev, 5) >= 0x40000 ? ioremap(pci_resource_start(pdev, 5), 0x40000) : NULL;
		if (!mmio) {
			pr_err("vega10_cu_unlock: %s: cannot map the register BAR\n", pci_name(pdev));
			continue;
		}
		mmios[ncards] = mmio;
		pdevs[ncards++] = pci_dev_get(pdev);
	}
	pci_dev_put(pdev);                          /* NULL, or the card past MAX_CARDS */
	if (!ncards) {
		pr_info("vega10_cu_unlock: no Vega 10 card, nothing to do\n");
		return 0;
	}
	pr_info("vega10_cu_unlock: %d Vega 10 card(s)\n", ncards);
	register_module_notifier(&cu_unlock_nb);   /* amdgpu loaded already: its gfx init has run, nothing to do */
	return 0;
}

static void __exit vega10_cu_unlock_exit(void)
{
	int i;

	unregister_module_notifier(&cu_unlock_nb);
	if (probe_on)
		unregister_kprobe(&kp);
	for (i = 0; i < ncards; ++i) {
		iounmap(mmios[i]);
		pci_dev_put(pdevs[i]);
	}
}

module_init(vega10_cu_unlock_init);
module_exit(vega10_cu_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Clear the VBIOS user CU mask on Vega 10 before amdgpu reads it");
