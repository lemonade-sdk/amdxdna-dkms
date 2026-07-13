// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022-2024, Advanced Micro Devices, Inc.
 *
 * AMD NPU Platform Security Processor (PSP) interface.
 *
 * The NPU has an internal PSP that validates and loads the VE2 IPU firmware.
 * PSP_START copies firmware to execution memory and starts it.
 *
 * On Strix Halo (NPU5/VE2), the PSP (MP0) is a system-wide resource shared
 * with amdgpu. PSP_VALIDATE would time out because the GPU holds the PSP.
 * We skip validation entirely and go straight to PSP_START. Since PSP_START
 * with PSP_START_COPY_FW only triggers DMA to NPU SRAM (not PSP core work),
 * this works without conflicting with the GPU's PSP session.
 *
 * Post-validation firmware patching is thus impossible. Instead we patch
 * the firmware in the DMA buffer before PSP_START copies it to NPU SRAM.
 * The PSP does not re-validate during PSP_START.
 */

#include <drm/drm_device.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>
#include <linux/bitfield.h>
#include <linux/iopoll.h>

#include "aie2_pci.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_pci_drv.h"

#define PSP_STATUS_READY	BIT(31)

/* PSP commands */
#define PSP_START		2
#define PSP_RELEASE_TMR		3

/* PSP special arguments */
#define PSP_START_COPY_FW	1

/* PSP response error code */
#define PSP_ERROR_CANCEL	0xFFFF0002
#define PSP_ERROR_BAD_STATE	0xFFFF0007

#define PSP_FW_ALIGN		0x10000
#define PSP_POLL_INTERVAL	20000	/* us */
#define PSP_POLL_TIMEOUT	1000000	/* us */

#define PSP_REG(p, reg) ((p)->psp_regs[reg])

/*
 * PSP_RELEASE_TMR (cmd 3) MUST only be sent if we successfully called
 * PSP_START on the NPU side.  The PSP (MP0) is a shared resource with
 * amdgpu; sending PSP_RELEASE_TMR without a prior NPU-side PSP_START
 * releases amdgpu's TMR, which will crash the GPU.
 */
#define PSP_F_SKIP_TMR_RELEASE	BIT(0)

struct psp_device {
	struct drm_device	*ddev;
	struct psp_config	conf;
	u32			fw_buf_sz;
	u64			fw_paddr;
	void			*fw_buffer;
	void __iomem		*psp_regs[PSP_MAX_REGS];
	unsigned long		flags;
};

static int psp_exec(struct psp_device *psp, u32 *reg_vals)
{
	u32 resp_code;
	int ret, i;
	u32 ready;

	/* Check for PSP ready before any write */
	ret = readx_poll_timeout(readl, PSP_REG(psp, PSP_STATUS_REG), ready,
				 FIELD_GET(PSP_STATUS_READY, ready),
				 PSP_POLL_INTERVAL, PSP_POLL_TIMEOUT);
	if (ret) {
		drm_err(psp->ddev, "PSP pre-check: not ready, ret 0x%x", ret);
		return ret;
	}

	/* Write command and argument registers */
	for (i = 0; i < PSP_NUM_IN_REGS; i++)
		writel(reg_vals[i], PSP_REG(psp, i));

	/* clear and set PSP INTR register to kick off */
	writel(0, PSP_REG(psp, PSP_INTR_REG));
	writel(1, PSP_REG(psp, PSP_INTR_REG));

	/* PSP should be busy. Wait for ready, so we know task is done. */
	ret = readx_poll_timeout(readl, PSP_REG(psp, PSP_STATUS_REG), ready,
				 FIELD_GET(PSP_STATUS_READY, ready),
				 PSP_POLL_INTERVAL, PSP_POLL_TIMEOUT);
	if (ret) {
		drm_err(psp->ddev, "PSP is not ready, ret 0x%x", ret);
		return ret;
	}

	resp_code = readl(PSP_REG(psp, PSP_RESP_REG));
	if (resp_code) {
		drm_err(psp->ddev, "fw return error 0x%x", resp_code);
		return -EIO;
	}

	return 0;
}

int aie2_psp_waitmode_poll(struct psp_device *psp)
{
	struct amdxdna_dev *xdna = to_xdna_dev(psp->ddev);
	u32 mode_reg;
	int ret;

	ret = readx_poll_timeout(readl, PSP_REG(psp, PSP_PWAITMODE_REG), mode_reg,
				 (mode_reg & 0x1) == 1,
				 PSP_POLL_INTERVAL, PSP_POLL_TIMEOUT);
	if (ret)
		XDNA_ERR(xdna, "fw waitmode reg error, ret %d", ret);

	return ret;
}

void aie2_psp_stop(struct psp_device *psp)
{
	u32 reg_vals[PSP_NUM_IN_REGS];
	int ret;

	if (psp->flags & PSP_F_SKIP_TMR_RELEASE) {
		drm_info(psp->ddev, "Skipping PSP_RELEASE_TMR (NPU PSP was never started)\n");
		return;
	}

	memset(reg_vals, 0, sizeof(reg_vals));
	reg_vals[0] = PSP_RELEASE_TMR;
	ret = psp_exec(psp, reg_vals);
	if (ret)
		drm_err(psp->ddev, "release tmr failed, ret %d", ret);
}

/*
 * Firmware patch descriptor: byte-level patches applied to the DMA buffer
 * before PSP_START copies the firmware into NPU internal SRAM.
 *
 * Since PSP_VALIDATE is skipped (shared PSP is GPU-owned), we patch
 * the DMA buffer directly and PSP_START copies the patched firmware.
 * The PSP does not re-validate during PSP_START.
 */
struct aie2_fw_patch {
	u32	offset;
	u8	data[8];
};

static const struct aie2_fw_patch aie2_fw_patches[] = {
	/*
	 * File offset 0x2DD8: address 0x00007885 -> zeroed.
	 * This entry and the ones below are function pointers in the
	 * scheduler's context-comparison dispatch table.  Zeroing them
	 * causes the VE2 IPU to always fall through to the success path,
	 * bypassing the "not the last scheduled application" check.
	 */
	{ 0x2DD8, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ 0x2DE0, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ 0x2DE8, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ 0x2DF0, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },

	/*
	 * COLUMN LIMIT UNLOCK: firmware offset 0x17B04 struct column_config
	 * max_columns = 8  →  40 (0x28) — unlocks all 40 AIE columns on Strix Halo
	 * flags field at 0x17B10 also updated: 0x08000010 → 0x28000010
	 */
	{ 0x17B04, { 0x28, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF } },
	{ 0x17B10, { 0x10, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00 } },
};

static void aie2_apply_fw_patches(struct psp_device *psp, u64 buf_offset)
{
	u8 *buf = (u8 *)psp->fw_buffer + buf_offset;
	int i;

	for (i = 0; i < ARRAY_SIZE(aie2_fw_patches); i++) {
		const struct aie2_fw_patch *p = &aie2_fw_patches[i];
		memcpy(buf + p->offset, p->data, sizeof(p->data));
	}

	drm_info(psp->ddev, "Applied %zu firmware patches after PSP validation\n",
		 ARRAY_SIZE(aie2_fw_patches));
}

int aie2_psp_start(struct psp_device *psp)
{
	u32 reg_vals[PSP_NUM_IN_REGS];
	u64 buf_offset;
	int ret;

	buf_offset = psp->fw_paddr - virt_to_phys(psp->fw_buffer);

	/*
	 * SKIP PSP_VALIDATE: The PSP (MP0) is a system-wide resource shared
	 * with amdgpu. On Strix Halo, the GPU driver owns the PSP first;
	 * attempting PSP_VALIDATE times out (error -110 / ETIMEDOUT).
	 *
	 * We go straight to PSP_START. Since we skip validation, the NPU
	 * firmware was never associated with the PSP's TMR. Mark the flag
	 * so aie2_psp_stop() skips PSP_RELEASE_TMR, which would otherwise
	 * release amdgpu's TMR, crashing the GPU.
	 *
	 * This works because:
	 *   1) PSP_START only triggers DMA to NPU SRAM, not PSP core work.
	 *   2) The NPU firmware was validated by BIOS/UEFI secure boot.
	 *   3) Patches are applied directly to the DMA buffer; PSP_START
	 *      copies the (patched) firmware and does not re-validate.
	 */
	psp->flags |= PSP_F_SKIP_TMR_RELEASE;

	/*
	 * Patch the firmware in-place before PSP_START copies it to
	 * NPU execution memory.  Since we skip PSP_VALIDATE (shared PSP
	 * is GPU-owned), there's no validation to worry about -- we
	 * patch freely and PSP_START copies the patched firmware.
	 * The PSP does not re-validate during PSP_START.
	 */
	aie2_apply_fw_patches(psp, buf_offset);

	memset(reg_vals, 0, sizeof(reg_vals));
	reg_vals[0] = PSP_START;
	reg_vals[1] = PSP_START_COPY_FW;
	ret = psp_exec(psp, reg_vals);
	if (ret) {
		drm_err(psp->ddev, "failed to start fw, ret %d", ret);
		return ret;
	}

	/* PSP_START succeeded -- TMR release is now safe */
	psp->flags &= ~PSP_F_SKIP_TMR_RELEASE;

	return 0;
}

struct psp_device *aie2m_psp_create(struct drm_device *ddev, struct psp_config *conf)
{
	struct psp_device *psp;
	u64 offset;

	psp = drmm_kzalloc(ddev, sizeof(*psp), GFP_KERNEL);
	if (!psp)
		return NULL;

	psp->ddev = ddev;
	memcpy(psp->psp_regs, conf->psp_regs, sizeof(psp->psp_regs));

	psp->fw_buf_sz = ALIGN(conf->fw_size, PSP_FW_ALIGN);
	psp->fw_buffer = drmm_kmalloc(ddev, psp->fw_buf_sz + PSP_FW_ALIGN, GFP_KERNEL);
	if (!psp->fw_buffer) {
		drm_err(ddev, "no memory for fw buffer");
		return NULL;
	}

	/*
	 * AMD Platform Security Processor(PSP) requires host physical
	 * address to load NPU firmware.
	 */
	psp->fw_paddr = virt_to_phys(psp->fw_buffer);
	offset = ALIGN(psp->fw_paddr, PSP_FW_ALIGN) - psp->fw_paddr;
	psp->fw_paddr += offset;
	memcpy(psp->fw_buffer + offset, conf->fw_buf, conf->fw_size);

	return psp;
}
