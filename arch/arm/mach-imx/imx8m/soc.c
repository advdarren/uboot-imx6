/*
 * Copyright 2017 NXP
 *
 * Peng Fan <peng.fan@nxp.com>
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <asm/arch/imx-regs.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/hab.h>
#include <asm/mach-imx/boot_mode.h>
#include <asm/mach-imx/syscounter.h>
#include <asm/armv8/mmu.h>
#include <errno.h>
#include <fdt_support.h>
#include <fsl_wdog.h>
#include <imx_sip.h>
#include <generated/version_autogenerated.h>

DECLARE_GLOBAL_DATA_PTR;

#if defined(CONFIG_SECURE_BOOT)
struct imx_sec_config_fuse_t const imx_sec_config_fuse = {
	.bank = 1,
	.word = 3,
};
#endif

int timer_init(void)
{
#ifdef CONFIG_SPL_BUILD
	struct sctr_regs *sctr = (struct sctr_regs *)SYSCNT_CTRL_BASE_ADDR;
	unsigned long freq = readl(&sctr->cntfid0);

	/* Update with accurate clock frequency */
	asm volatile("msr cntfrq_el0, %0" : : "r" (freq) : "memory");

	clrsetbits_le32(&sctr->cntcr, SC_CNTCR_FREQ0 | SC_CNTCR_FREQ1,
			SC_CNTCR_FREQ0 | SC_CNTCR_ENABLE | SC_CNTCR_HDBG);
#endif

	gd->arch.tbl = 0;
	gd->arch.tbu = 0;

	return 0;
}

void enable_tzc380(void)
{
	struct iomuxc_gpr_base_regs *gpr =
		(struct iomuxc_gpr_base_regs *)IOMUXC_GPR_BASE_ADDR;

	/* Enable TZASC and lock setting */
	setbits_le32(&gpr->gpr[10], GPR_TZASC_EN);
	setbits_le32(&gpr->gpr[10], GPR_TZASC_EN_LOCK);
}

void set_wdog_reset(struct wdog_regs *wdog)
{
	/*
	 * Output WDOG_B signal to reset external pmic or POR_B decided by
	 * the board design. Without external reset, the peripherals/DDR/
	 * PMIC are not reset, that may cause system working abnormal.
	 * WDZST bit is write-once only bit. Align this bit in kernel,
	 * otherwise kernel code will have no chance to set this bit.
	 */
	setbits_le16(&wdog->wcr, WDOG_WDT_MASK | WDOG_WDZST_MASK);
}

static struct mm_region imx8m_mem_map[] = {
	{
		/* ROM */
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x100000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		.virt = 0x100000UL,
		.phys = 0x100000UL,
		.size = 0x8000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		.virt = 0x7C0000UL,
		.phys = 0x7C0000UL,
		.size = 0x80000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* OCRAM */
		.virt = 0x900000UL,
		.phys = 0x900000UL,
		.size = 0x200000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		/* AIPS */
		.virt = 0xB00000UL,
		.phys = 0xB00000UL,
		.size = 0x3f500000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* DRAM1 */
		.virt = 0x40000000UL,
		.phys = 0x40000000UL,
		.size = 0xC0000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		/* DRAM2 */
		.virt = 0x100000000UL,
		.phys = 0x100000000UL,
		.size = 0x040000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = imx8m_mem_map;

u32 get_cpu_rev(void)
{
	struct anamix_pll *ana_pll = (struct anamix_pll *)ANATOP_BASE_ADDR;
	u32 reg = readl(&ana_pll->digprog);
	u32 type = (reg >> 16) & 0xff;
	u32 rom_version;

	reg &= 0xff;

	if (reg == CHIP_REV_1_0) {
		/*
		 * For B0 chip, the DIGPROG is not updated, still TO1.0.
		 * we have to check ROM version further
		 */
		rom_version = readl((void __iomem *)ROM_VERSION_A0);
		if (rom_version != CHIP_REV_1_0) {
			rom_version = readl((void __iomem *)ROM_VERSION_B0);
			if (rom_version >= CHIP_REV_2_0)
				reg = CHIP_REV_2_0;
		}
	}

	return (type << 12) | reg;
}

static void imx_set_wdog_powerdown(bool enable)
{
	struct wdog_regs *wdog1 = (struct wdog_regs *)WDOG1_BASE_ADDR;
	struct wdog_regs *wdog2 = (struct wdog_regs *)WDOG2_BASE_ADDR;
	struct wdog_regs *wdog3 = (struct wdog_regs *)WDOG3_BASE_ADDR;

	/* Write to the PDE (Power Down Enable) bit */
	writew(enable, &wdog1->wmcr);
	writew(enable, &wdog2->wmcr);
	writew(enable, &wdog3->wmcr);
}

int arch_cpu_init(void)
{
	/*
	 * Init timer at very early state, because sscg pll setting
	 * will use it
	 */
	timer_init();

	if (IS_ENABLED(CONFIG_SPL_BUILD)) {
		clock_init();
		imx_set_wdog_powerdown(false);
	}

	return 0;
}

bool is_usb_boot(void)
{
	return get_boot_device() == USB_BOOT;
}

#ifdef CONFIG_OF_SYSTEM_SETUP
static int ft_add_optee_node(void *fdt, bd_t *bd)
{
	const char *path, *subpath;
	int offs;

	/*
	 * No TEE space allocated indicating no TEE running, so no
	 * need to add optee node in dts
	 */
	if (!rom_pointer[1])
		return 0;

	offs = fdt_increase_size(fdt, 512);
	if (offs) {
		printf("No Space for dtb\n");
		return 1;
	}

	path = "/firmware";
	offs = fdt_path_offset(fdt, path);
	if (offs < 0) {
		path = "/";
		offs = fdt_path_offset(fdt, path);

		if (offs < 0) {
			printf("Could not find root node.\n");
			return 1;
		}

		subpath = "firmware";
		offs = fdt_add_subnode(fdt, offs, subpath);
		if (offs < 0) {
			printf("Could not create %s node.\n", subpath);
		}
	}

	subpath = "optee";
	offs = fdt_add_subnode(fdt, offs, subpath);
	if (offs < 0) {
		printf("Could not create %s node.\n", subpath);
	}

	fdt_setprop_string(fdt, offs, "compatible", "linaro,optee-tz");
	fdt_setprop_string(fdt, offs, "method", "smc");

	return 0;
}

int ft_system_setup(void *blob, bd_t *bd)
{
	int i = 0;
	int rc;
	int nodeoff;
	
	if (get_boot_device() == USB_BOOT) {
		static const char *nodes_path[] = {
			"/dcss@32e00000",
			"/hdmi@32c00000",
			"/hdmi_cec@32c33800",
			"/hdmi_drm@32c00000",
			"/display-subsystem",
			"/sound-hdmi"
		};
		const char *status = "disabled";

		for (i = 0; i < ARRAY_SIZE(nodes_path); i++) {
			nodeoff = fdt_path_offset(blob, nodes_path[i]);
			if (nodeoff < 0)
				continue; /* Not found, skip it */

			printf("Found %s node\n", nodes_path[i]);

add_status:
			rc = fdt_setprop(blob, nodeoff, "status", status, strlen(status) + 1);
			if (rc) {
				if (rc == -FDT_ERR_NOSPACE) {
					rc = fdt_increase_size(blob, 512);
					if (!rc)
						goto add_status;
				}
				printf("Unable to update property %s:%s, err=%s\n",
					nodes_path[i], "status", fdt_strerror(rc));
			} else {
				printf("Modify %s:%s disabled\n",
					nodes_path[i], "status");
			}
		}

		const char *usb_dwc3_path = "/usb@38100000/dwc3";
		nodeoff = fdt_path_offset(blob, usb_dwc3_path);
		if (nodeoff >= 0) {
			const char *speed = "high-speed";
			printf("Found %s node\n", usb_dwc3_path);

usb_modify_speed:

			rc = fdt_setprop(blob, nodeoff, "maximum-speed", speed, strlen(speed) + 1);
			if (rc) {
				if (rc == -FDT_ERR_NOSPACE) {
					rc = fdt_increase_size(blob, 512);
					if (!rc)
						goto usb_modify_speed;
				}
				printf("Unable to set property %s:%s, err=%s\n",
					usb_dwc3_path, "maximum-speed", fdt_strerror(rc));
			} else {
				printf("Modify %s:%s = %s\n",
					usb_dwc3_path, "maximum-speed", speed);
			}
		}else {
			printf("Can't found %s node\n", usb_dwc3_path);
		}
	}

	/* Disable the CPU idle for A0 chip since the HW does not support it */
	if (is_soc_rev(CHIP_REV_1_0)) {
		static const char * const nodes_path[] = {
			"/cpus/cpu@0",
			"/cpus/cpu@1",
			"/cpus/cpu@2",
			"/cpus/cpu@3",
		};

		for (i = 0; i < ARRAY_SIZE(nodes_path); i++) {
			nodeoff = fdt_path_offset(blob, nodes_path[i]);
			if (nodeoff < 0)
				continue; /* Not found, skip it */

			printf("Found %s node\n", nodes_path[i]);

			rc = fdt_delprop(blob, nodeoff, "cpu-idle-states");
			if (rc) {
				printf("Unable to update property %s:%s, err=%s\n",
				       nodes_path[i], "status", fdt_strerror(rc));
				return rc;
			}

			printf("Remove %s:%s\n", nodes_path[i],
			       "cpu-idle-states");
		}
	}

	return ft_add_optee_node(blob, bd);
}
#endif

void reset_cpu(ulong addr)
{
	struct watchdog_regs *wdog = (struct watchdog_regs *)WDOG1_BASE_ADDR;

	/* Clear WDA to trigger WDOG_B immediately */
	writew((WCR_WDE | WCR_SRS), &wdog->wcr);

	while (1) {
		/*
		 * spin for .5 seconds before reset
		 */
	}
}

#if defined(CONFIG_ARCH_MISC_INIT)
#define FSL_SIP_BUILDINFO			0xC2000003
#define FSL_SIP_BUILDINFO_GET_COMMITHASH	0x00
static void acquire_buildinfo(void)
{
	uint64_t atf_commit = 0;

	/* Get ARM Trusted Firmware commit id */
	atf_commit = call_imx_sip(FSL_SIP_BUILDINFO,
				  FSL_SIP_BUILDINFO_GET_COMMITHASH, 0, 0, 0);
	if (atf_commit == 0xffffffff) {
		debug("ATF does not support build info\n");
		atf_commit = 0x30; /* Display 0, 0 ascii is 0x30 */
	}

	printf("\n BuildInfo:\n  - ATF %s\n  - %s\n\n", (char *)&atf_commit,
	       U_BOOT_VERSION);
}

int arch_misc_init(void)
{
	acquire_buildinfo();

	return 0;
}
#endif

#ifdef CONFIG_USB_XHCI_IMX8M
#define FSL_SIP_GPC			0xC2000000
#define FSL_SIP_CONFIG_GPC_PM_DOMAIN	0x03
int imx8m_usb_power(int usb_id, bool on)
{
	unsigned long ret;

	if (usb_id > 1)
		return -EINVAL;

	ret = call_imx_sip(FSL_SIP_GPC,
			FSL_SIP_CONFIG_GPC_PM_DOMAIN, 2 + usb_id, on, 0);
	if (ret)
		return -EPERM;

	return 0;
}
#endif
