/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"io-pgtable-msm-secure: " fmt

#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <soc/qcom/scm.h>
#include <asm/cacheflush.h>

#include "io-pgtable.h"

#define IOMMU_SECURE_MAP2_FLAT 0x12
#define IOMMU_SECURE_UNMAP2_FLAT 0x13
#define IOMMU_TLBINVAL_FLAG 0x00000001

#define io_pgtable_to_data(x)						\
	container_of((x), struct msm_secure_io_pgtable, iop)

#define io_pgtable_ops_to_pgtable(x)					\
	container_of((x), struct io_pgtable, ops)

#define io_pgtable_ops_to_data(x)					\
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

struct msm_secure_io_pgtable {
	struct io_pgtable iop;
};

static int msm_secure_map(struct io_pgtable_ops *ops, unsigned long iova,
			phys_addr_t paddr, size_t size, int iommu_prot)
{
	return -EINVAL;
}

static dma_addr_t msm_secure_get_phys_addr(struct scatterlist *sg)
{
	/*
	 * Try sg_dma_address first so that we can
	 * map carveout regions that do not have a
	 * struct page associated with them.
	 */
	dma_addr_t pa = sg_dma_address(sg);

	if (pa == 0)
		pa = sg_phys(sg);
	return pa;
}

static int msm_secure_map_sg(struct io_pgtable_ops *ops, unsigned long iova,
			   struct scatterlist *sg, unsigned int nents,
			   int iommu_prot, size_t *size)
{
	struct msm_secure_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	int ret = -EINVAL;
	struct scatterlist *tmp, *sgiter;
	dma_addr_t *pa_list = 0;
	unsigned int cnt, offset = 0, chunk_offset = 0;
	dma_addr_t pa;
	void *flush_va, *flush_va_end;
	unsigned long len = 0;
	struct scm_desc desc = {0};
	int i;
	u32 resp;

	for_each_sg(sg, tmp, nents, i)
		len += tmp->length;

	if (!IS_ALIGNED(iova, SZ_1M) || !IS_ALIGNED(len, SZ_1M))
		return -EINVAL;

	if (sg->length == len) {
		cnt = 1;
		pa = msm_secure_get_phys_addr(sg);
		if (!IS_ALIGNED(pa, SZ_1M))
			return -EINVAL;

		desc.args[0] = virt_to_phys(&pa);
		desc.args[1] = cnt;
		desc.args[2] = len;
		flush_va = &pa;
	} else {
		sgiter = sg;
		if (!IS_ALIGNED(sgiter->length, SZ_1M))
			return -EINVAL;
		cnt = sg->length / SZ_1M;
		while ((sgiter = sg_next(sgiter))) {
			if (!IS_ALIGNED(sgiter->length, SZ_1M))
				return -EINVAL;
			cnt += sgiter->length / SZ_1M;
		}

		pa_list = kmalloc_array(cnt, sizeof(*pa_list), GFP_KERNEL);
		if (!pa_list)
			return -ENOMEM;

		sgiter = sg;
		cnt = 0;
		pa = msm_secure_get_phys_addr(sgiter);
		while (offset < len) {

			if (!IS_ALIGNED(pa, SZ_1M)) {
				kfree(pa_list);
				return -EINVAL;
			}

			pa_list[cnt] = pa + chunk_offset;
			chunk_offset += SZ_1M;
			offset += SZ_1M;
			cnt++;

			if (chunk_offset >= sgiter->length && offset < len) {
				chunk_offset = 0;
				sgiter = sg_next(sgiter);
				pa = msm_secure_get_phys_addr(sgiter);
			}
		}

		desc.args[0] = virt_to_phys(pa_list);
		desc.args[1] = cnt;
		desc.args[2] = SZ_1M;
		flush_va = pa_list;
	}

	desc.args[3] = cfg->arm_msm_secure_cfg.sec_id;
	desc.args[4] = cfg->arm_msm_secure_cfg.cbndx;
	desc.args[5] = iova;
	desc.args[6] = len;
	desc.args[7] = 0;

	desc.arginfo = SCM_ARGS(8, SCM_RW, SCM_VAL, SCM_VAL, SCM_VAL, SCM_VAL,
			SCM_VAL, SCM_VAL, SCM_VAL);

	/*
	 * Ensure that the buffer is in RAM by the time it gets to TZ
	 */

	flush_va_end = (void *) (((unsigned long) flush_va) +
			(cnt * sizeof(*pa_list)));
	dmac_clean_range(flush_va, flush_va_end);

	if (is_scm_armv8()) {
		ret = scm_call2(SCM_SIP_FNID(SCM_SVC_MP,
					 IOMMU_SECURE_MAP2_FLAT), &desc);
		resp = desc.ret[0];

		if (ret || resp)
			ret = -EINVAL;
		else
			ret = len;
	 }

	kfree(pa_list);
	return ret;
}

static size_t msm_secure_unmap(struct io_pgtable_ops *ops, unsigned long iova,
			  size_t len)
{
	struct msm_secure_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	int ret = -EINVAL;
	struct scm_desc desc = {0};

	if (!IS_ALIGNED(iova, SZ_1M) || !IS_ALIGNED(len, SZ_1M))
		return ret;

	desc.args[0] = cfg->arm_msm_secure_cfg.sec_id;
	desc.args[1] = cfg->arm_msm_secure_cfg.cbndx;
	desc.args[2] = iova;
	desc.args[3] = len;
	desc.args[4] = IOMMU_TLBINVAL_FLAG;
	desc.arginfo = SCM_ARGS(5);

	if (is_scm_armv8()) {
		ret = scm_call2(SCM_SIP_FNID(SCM_SVC_MP,
			IOMMU_SECURE_UNMAP2_FLAT), &desc);

		if (!ret)
			ret = len;
	}
	return ret;
}

static phys_addr_t msm_secure_iova_to_phys(struct io_pgtable_ops *ops,
					 unsigned long iova)
{
	return -EINVAL;
}

static struct msm_secure_io_pgtable *
msm_secure_alloc_pgtable_data(struct io_pgtable_cfg *cfg)
{
	struct msm_secure_io_pgtable *data;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->iop.ops = (struct io_pgtable_ops) {
		.map		= msm_secure_map,
		.map_sg		= msm_secure_map_sg,
		.unmap		= msm_secure_unmap,
		.iova_to_phys	= msm_secure_iova_to_phys,
	};

	return data;
}

static struct io_pgtable *
msm_secure_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct msm_secure_io_pgtable *data =
		msm_secure_alloc_pgtable_data(cfg);

	return &data->iop;
}

static void msm_secure_free_pgtable(struct io_pgtable *iop)
{
	struct msm_secure_io_pgtable *data = io_pgtable_to_data(iop);

	kfree(data);
}

struct io_pgtable_init_fns io_pgtable_arm_msm_secure_init_fns = {
	.alloc	= msm_secure_alloc_pgtable,
	.free	= msm_secure_free_pgtable,
};
