/*
 * drivers/video/tegra/host/msenc/msenc.c
 *
 * Tegra MSENC Module Support
 *
 * Copyright (c) 2012-2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/slab.h>         /* for kzalloc */
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <mach/clk.h>
#include <asm/byteorder.h>      /* for parsing ucode image wrt endianness */
#include <linux/delay.h>	/* for udelay */
#include <linux/scatterlist.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#include <mach/pm_domains.h>

#include "dev.h"
#include "msenc.h"
#include "hw_msenc.h"
#include "bus_client.h"
#include "nvhost_acm.h"
#include "nvhost_scale.h"
#include "chip_support.h"
#include "nvhost_memmgr.h"
#include "t114/t114.h"
#include "t148/t148.h"
#include "t124/t124.h"

#define MSENC_IDLE_TIMEOUT_DEFAULT	10000	/* 10 milliseconds */
#define MSENC_IDLE_CHECK_PERIOD		10	/* 10 usec */

#define get_msenc(ndev) ((struct msenc *)(ndev)->dev.platform_data)
#define set_msenc(ndev, f) ((ndev)->dev.platform_data = f)

/* caller is responsible for freeing */
static char *msenc_get_fw_name(struct platform_device *dev)
{
	char *fw_name;
	u8 maj, min;
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	/* note size here is a little over...*/
	fw_name = kzalloc(32, GFP_KERNEL);
	if (!fw_name)
		return NULL;

	decode_msenc_ver(pdata->version, &maj, &min);
	switch (maj) {
	case 2:
		/* there are no minor versions so far for maj==2 */
		sprintf(fw_name, "nvhost_msenc02.fw");
		break;
	case 3:
		sprintf(fw_name, "nvhost_msenc03%d.fw", min);
		break;
	default:
		kfree(fw_name);
		return NULL;
	}

	dev_info(&dev->dev, "fw name:%s\n", fw_name);

	return fw_name;
}

static int msenc_dma_wait_idle(struct platform_device *dev, u32 *timeout)
{
	if (!*timeout)
		*timeout = MSENC_IDLE_TIMEOUT_DEFAULT;

	do {
		u32 check = min_t(u32, MSENC_IDLE_CHECK_PERIOD, *timeout);
		u32 dmatrfcmd = nvhost_device_readl(dev, msenc_dmatrfcmd_r());
		u32 idle_v = msenc_dmatrfcmd_idle_v(dmatrfcmd);

		if (msenc_dmatrfcmd_idle_true_v() == idle_v)
			return 0;

		udelay(MSENC_IDLE_CHECK_PERIOD);
		*timeout -= check;
	} while (*timeout);

	dev_err(&dev->dev, "dma idle timeout");

	return -1;
}

static int msenc_dma_pa_to_internal_256b(struct platform_device *dev,
		u32 offset, u32 internal_offset, bool imem)
{
	u32 cmd = msenc_dmatrfcmd_size_256b_f();
	u32 pa_offset =  msenc_dmatrffboffs_offs_f(offset);
	u32 i_offset = msenc_dmatrfmoffs_offs_f(internal_offset);
	u32 timeout = 0; /* default*/

	if (imem)
		cmd |= msenc_dmatrfcmd_imem_true_f();

	nvhost_device_writel(dev, msenc_dmatrfmoffs_r(), i_offset);
	nvhost_device_writel(dev, msenc_dmatrffboffs_r(), pa_offset);
	nvhost_device_writel(dev, msenc_dmatrfcmd_r(), cmd);

	return msenc_dma_wait_idle(dev, &timeout);

}

static int msenc_wait_idle(struct platform_device *dev, u32 *timeout)
{
	if (!*timeout)
		*timeout = MSENC_IDLE_TIMEOUT_DEFAULT;

	do {
		u32 check = min_t(u32, MSENC_IDLE_CHECK_PERIOD, *timeout);
		u32 w = nvhost_device_readl(dev, msenc_idlestate_r());

		if (!w)
			return 0;
		udelay(MSENC_IDLE_CHECK_PERIOD);
		*timeout -= check;
	} while (*timeout);

	return -1;
}

int msenc_boot(struct platform_device *dev)
{
	u32 timeout;
	u32 offset;
	int err = 0;
	struct msenc *m = get_msenc(dev);

	/* check if firmware is loaded or not */
	if (!m || !m->valid)
		return -ENOMEDIUM;

	nvhost_device_writel(dev, msenc_dmactl_r(), 0);
	nvhost_device_writel(dev, msenc_dmatrfbase_r(),
		(sg_dma_address(m->pa->sgl) + m->os.bin_data_offset) >> 8);

	for (offset = 0; offset < m->os.data_size; offset += 256)
		msenc_dma_pa_to_internal_256b(dev,
					   m->os.data_offset + offset,
					   offset, false);

	msenc_dma_pa_to_internal_256b(dev, m->os.code_offset, 0, true);

	/* setup msenc interrupts and enable interface */
	nvhost_device_writel(dev, msenc_irqmset_r(),
			(msenc_irqmset_ext_f(0xff) |
				msenc_irqmset_swgen1_set_f() |
				msenc_irqmset_swgen0_set_f() |
				msenc_irqmset_exterr_set_f() |
				msenc_irqmset_halt_set_f()   |
				msenc_irqmset_wdtmr_set_f()));
	nvhost_device_writel(dev, msenc_irqdest_r(),
			(msenc_irqdest_host_ext_f(0xff) |
				msenc_irqdest_host_swgen1_host_f() |
				msenc_irqdest_host_swgen0_host_f() |
				msenc_irqdest_host_exterr_host_f() |
				msenc_irqdest_host_halt_host_f()));
	nvhost_device_writel(dev, msenc_itfen_r(),
			(msenc_itfen_mthden_enable_f() |
				msenc_itfen_ctxen_enable_f()));

	/* boot msenc */
	nvhost_device_writel(dev, msenc_bootvec_r(), msenc_bootvec_vec_f(0));
	nvhost_device_writel(dev, msenc_cpuctl_r(),
			msenc_cpuctl_startcpu_true_f());

	timeout = 0; /* default */

	err = msenc_wait_idle(dev, &timeout);
	if (err != 0) {
		dev_err(&dev->dev, "boot failed due to timeout");
		return err;
	}

	return 0;
}

static int msenc_setup_ucode_image(struct platform_device *dev,
		u32 *ucode_ptr,
		const struct firmware *ucode_fw)
{
	struct msenc *m = get_msenc(dev);
	/* image data is little endian. */
	struct msenc_ucode_v1 ucode;
	int w;

	/* copy the whole thing taking into account endianness */
	for (w = 0; w < ucode_fw->size / sizeof(u32); w++)
		ucode_ptr[w] = le32_to_cpu(((u32 *)ucode_fw->data)[w]);

	ucode.bin_header = (struct msenc_ucode_bin_header_v1 *)ucode_ptr;
	/* endian problems would show up right here */
	if (ucode.bin_header->bin_magic != 0x10de) {
		dev_err(&dev->dev,
			   "failed to get firmware magic");
		return -EINVAL;
	}
	if (ucode.bin_header->bin_ver != 1) {
		dev_err(&dev->dev,
			   "unsupported firmware version");
		return -ENOENT;
	}
	/* shouldn't be bigger than what firmware thinks */
	if (ucode.bin_header->bin_size > ucode_fw->size) {
		dev_err(&dev->dev,
			   "ucode image size inconsistency");
		return -EINVAL;
	}

	dev_dbg(&dev->dev,
		"ucode bin header: magic:0x%x ver:%d size:%d",
		ucode.bin_header->bin_magic,
		ucode.bin_header->bin_ver,
		ucode.bin_header->bin_size);
	dev_dbg(&dev->dev,
		"ucode bin header: os bin (header,data) offset size: 0x%x, 0x%x %d",
		ucode.bin_header->os_bin_header_offset,
		ucode.bin_header->os_bin_data_offset,
		ucode.bin_header->os_bin_size);
	ucode.os_header = (struct msenc_ucode_os_header_v1 *)
		(((void *)ucode_ptr) + ucode.bin_header->os_bin_header_offset);

	dev_dbg(&dev->dev,
		"os ucode header: os code (offset,size): 0x%x, 0x%x",
		ucode.os_header->os_code_offset,
		ucode.os_header->os_code_size);
	dev_dbg(&dev->dev,
		"os ucode header: os data (offset,size): 0x%x, 0x%x",
		ucode.os_header->os_data_offset,
		ucode.os_header->os_data_size);
	dev_dbg(&dev->dev,
		"os ucode header: num apps: %d",
		ucode.os_header->num_apps);

	m->os.size = ucode.bin_header->os_bin_size;
	m->os.bin_data_offset = ucode.bin_header->os_bin_data_offset;
	m->os.code_offset = ucode.os_header->os_code_offset;
	m->os.data_offset = ucode.os_header->os_data_offset;
	m->os.data_size   = ucode.os_header->os_data_size;

	return 0;
}

int msenc_read_ucode(struct platform_device *dev, const char *fw_name)
{
	struct msenc *m = get_msenc(dev);
	const struct firmware *ucode_fw;
	int err;

	ucode_fw  = nvhost_client_request_firmware(dev, fw_name);
	if (!ucode_fw) {
		dev_err(&dev->dev, "failed to get msenc firmware\n");
		err = -ENOENT;
		return err;
	}

	/* allocate pages for ucode */
	m->mem_r = nvhost_memmgr_alloc(nvhost_get_host(dev)->memmgr,
				     roundup(ucode_fw->size, PAGE_SIZE),
				     PAGE_SIZE, mem_mgr_flag_uncacheable, 0);
	if (IS_ERR(m->mem_r)) {
		dev_err(&dev->dev, "nvmap alloc failed");
		err = PTR_ERR(m->mem_r);
		goto clean_up;
	}

	m->pa = nvhost_memmgr_pin(nvhost_get_host(dev)->memmgr, m->mem_r,
			&dev->dev);
	if (IS_ERR(m->pa)) {
		dev_err(&dev->dev, "nvmap pin failed for ucode");
		err = PTR_ERR(m->pa);
		m->pa = NULL;
		goto clean_up;
	}

	m->mapped = nvhost_memmgr_mmap(m->mem_r);
	if (IS_ERR_OR_NULL(m->mapped)) {
		dev_err(&dev->dev, "nvmap mmap failed");
		err = -ENOMEM;
		goto clean_up;
	}

	err = msenc_setup_ucode_image(dev, (u32 *)m->mapped, ucode_fw);
	if (err) {
		dev_err(&dev->dev, "failed to parse firmware image\n");
		return err;
	}

	m->valid = true;

	release_firmware(ucode_fw);

	return 0;

clean_up:
	if (m->mapped) {
		nvhost_memmgr_munmap(m->mem_r, (u32 *)m->mapped);
		m->mapped = NULL;
	}
	if (m->pa) {
		nvhost_memmgr_unpin(nvhost_get_host(dev)->memmgr, m->mem_r,
				&dev->dev, m->pa);
		m->pa = NULL;
	}
	if (m->mem_r) {
		nvhost_memmgr_put(nvhost_get_host(dev)->memmgr, m->mem_r);
		m->mem_r = NULL;
	}
	release_firmware(ucode_fw);
	return err;
}

int nvhost_msenc_init(struct platform_device *dev)
{
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);
	int err = 0;
	struct msenc *m;
	char *fw_name;

	fw_name = msenc_get_fw_name(dev);
	if (!fw_name) {
		dev_err(&dev->dev, "couldn't determine firmware name");
		return -EINVAL;
	}

	m = kzalloc(sizeof(struct msenc), GFP_KERNEL);
	if (!m) {
		dev_err(&dev->dev, "couldn't alloc ucode");
		kfree(fw_name);
		return -ENOMEM;
	}
	set_msenc(dev, m);

	err = msenc_read_ucode(dev, fw_name);
	kfree(fw_name);
	fw_name = 0;

	if (err || !m->valid) {
		dev_err(&dev->dev, "ucode not valid");
		goto clean_up;
	}

	nvhost_module_busy(dev);
	msenc_boot(dev);
	nvhost_module_idle(dev);

	if (pdata->scaling_init)
		nvhost_scale_hw_init(dev);

	return 0;

clean_up:
	dev_err(&dev->dev, "failed");
	return err;
}

void nvhost_msenc_deinit(struct platform_device *dev)
{
	struct msenc *m = get_msenc(dev);
	struct nvhost_device_data *pdata = platform_get_drvdata(dev);

	if (pdata->scaling_init)
		nvhost_scale_hw_deinit(dev);

	if (!m)
		return;

	/* unpin, free ucode memory */
	if (m->mapped) {
		nvhost_memmgr_munmap(m->mem_r, m->mapped);
		m->mapped = NULL;
	}
	if (m->pa) {
		nvhost_memmgr_unpin(nvhost_get_host(dev)->memmgr, m->mem_r,
			&dev->dev, m->pa);
		m->pa = NULL;
	}
	if (m->mem_r) {
		nvhost_memmgr_put(nvhost_get_host(dev)->memmgr, m->mem_r);
		m->mem_r = NULL;
	}
	kfree(m);
	set_msenc(dev, NULL);
	m->valid = false;
}

int nvhost_msenc_finalize_poweron(struct platform_device *dev)
{
	return msenc_boot(dev);
}

static struct of_device_id tegra_msenc_of_match[] = {
#ifdef TEGRA_11X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra114-msenc",
		.data = (struct nvhost_device_data *)&t11_msenc_info },
#endif
#ifdef TEGRA_14X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra148-msenc",
		.data = (struct nvhost_device_data *)&t14_msenc_info },
#endif
#ifdef TEGRA_12X_OR_HIGHER_CONFIG
	{ .compatible = "nvidia,tegra124-msenc",
		.data = (struct nvhost_device_data *)&t14_msenc_info },
#endif
	{ },
};

#ifdef CONFIG_PM_GENERIC_DOMAINS
static int msenc_unpowergate(struct generic_pm_domain *domain)
{
	struct nvhost_device_data *pdata;

	pdata = container_of(domain, struct nvhost_device_data, pd);
	return nvhost_module_power_on(pdata->pdev);
}

static int msenc_powergate(struct generic_pm_domain *domain)
{
	struct nvhost_device_data *pdata;

	pdata = container_of(domain, struct nvhost_device_data, pd);
	return nvhost_module_power_off(pdata->pdev);
}
#endif

static int msenc_probe(struct platform_device *dev)
{
	int err = 0;
	struct nvhost_device_data *pdata = NULL;

	if (dev->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_device(tegra_msenc_of_match, &dev->dev);
		if (match)
			pdata = (struct nvhost_device_data *)match->data;
	} else
		pdata = (struct nvhost_device_data *)dev->dev.platform_data;

	WARN_ON(!pdata);
	if (!pdata) {
		dev_info(&dev->dev, "no platform data\n");
		return -ENODATA;
	}

	pdata->pdev = dev;
	pdata->init = nvhost_msenc_init;
	pdata->deinit = nvhost_msenc_deinit;
	pdata->finalize_poweron = nvhost_msenc_finalize_poweron;

	mutex_init(&pdata->lock);

	platform_set_drvdata(dev, pdata);
	dev->dev.platform_data = NULL;

	/* get the module clocks to sane state */
	nvhost_module_init(dev);

#ifdef CONFIG_PM_GENERIC_DOMAINS
	pdata->pd.name = "msenc";
	pdata->pd.power_off = msenc_powergate;
	pdata->pd.power_on = msenc_unpowergate;
	pdata->pd.dev_ops.start = nvhost_module_enable_clk;
	pdata->pd.dev_ops.stop = nvhost_module_disable_clk;

	/* add module power domain and also add its domain
	 * as sub-domain of MC domain */
	err = nvhost_module_add_domain(&pdata->pd, dev);

	/* overwrite save/restore fptrs set by pm_genpd_init */
	pdata->pd.domain.ops.suspend = nvhost_client_device_suspend;
	pdata->pd.domain.ops.resume = nvhost_client_device_resume;
	pdata->pd.dev_ops.restore_state = nvhost_module_finalize_poweron;
#endif

	/* enable runtime pm. this is needed now since we need to call
	 * _get_sync/_put during boot-up to ensure MC domain is ON */
	if (pdata->clockgate_delay) {
		pm_runtime_set_autosuspend_delay(&dev->dev,
			pdata->clockgate_delay);
		pm_runtime_use_autosuspend(&dev->dev);
	}
	pm_runtime_enable(&dev->dev);

	err = nvhost_client_device_get_resources(dev);
	if (err)
		return err;

	pm_runtime_get_sync(&dev->dev);
	err = nvhost_client_device_init(dev);
	if (pdata->clockgate_delay)
		pm_runtime_put_sync_autosuspend(&dev->dev);
	else
		pm_runtime_put(&dev->dev);
	if (err)
		return err;

	return 0;
}

static int __exit msenc_remove(struct platform_device *dev)
{
	/* Add clean-up */
	return 0;
}

static struct platform_driver msenc_driver = {
	.probe = msenc_probe,
	.remove = __exit_p(msenc_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "msenc",
#ifdef CONFIG_OF
		.of_match_table = tegra_msenc_of_match,
#endif
	}
};

static int __init msenc_init(void)
{
	return platform_driver_register(&msenc_driver);
}

static void __exit msenc_exit(void)
{
	platform_driver_unregister(&msenc_driver);
}

module_init(msenc_init);
module_exit(msenc_exit);
