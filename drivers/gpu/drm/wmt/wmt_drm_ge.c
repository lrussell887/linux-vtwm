// SPDX-License-Identifier: GPL-2.0-only
/*
 * WonderMedia WM8505 DRM/KMS Graphics Driver
 *
 * 2D Graphics Engine (GE)
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>

#include "wmt_drm.h"

/*
 * =============================================================================
 * Graphics Engine (GE) 2D Hardware Abstraction
 * =============================================================================
 */

/**
 * wmt_ge_sync - Block until GE finishes current command queue
 */
int wmt_ge_sync(struct wmt_drm_device *wmt)
{
	u32 status;

	return readl_poll_timeout_atomic(wmt->ge_regs + GE_STATUS_OFF,
					 status, !(status & 4), 1, 100000);
}

/**
 * wmt_ge_hw_fill - Execute hardware-accelerated solid fill
 */
int wmt_ge_hw_fill(struct wmt_drm_device *wmt, struct drm_gem_dma_object *gem,
		   struct wmt_ge_op *req)
{
	void __iomem *regs = wmt->ge_regs;

	if (unlikely(!req->width || !req->height || !req->dest_pitch ||
		     (req->dest_pitch / 4) > WMT_GE_MAX_WIDTH ||
		     req->height > WMT_GE_MAX_HEIGHT ||
		     req->dest_y > WMT_GE_MAX_HEIGHT - req->height ||
		     req->width > (req->dest_pitch / 4) ||
		     req->dest_x > (req->dest_pitch / 4) - req->width ||
		     (req->dest_y + req->height) * req->dest_pitch > gem->base.size))
		return -EINVAL;

	writel(3, regs + GE_DEPTH_OFF);
	writel(0, regs + GE_HIGHCOLOR_OFF);

	writel(gem->dma_addr, regs + GE_DESTBASE_OFF);
	writel((req->dest_pitch / 4) - 1, regs + GE_DESTDISPW_OFF);
	writel((req->dest_y + req->height) - 1, regs + GE_DESTDISPH_OFF);
	writel(req->dest_x, regs + GE_DESTAREAX_OFF);
	writel(req->dest_y, regs + GE_DESTAREAY_OFF);
	writel(req->width - 1, regs + GE_DESTAREAW_OFF);
	writel(req->height - 1, regs + GE_DESTAREAH_OFF);

	writel(req->color, regs + GE_PAT0C_OFF);
	writel(GE_CMD_BLIT, regs + GE_COMMAND_OFF);
	writel(req->rop ? req->rop : GE_ROP_PAT_COPY, regs + GE_ROPCODE_OFF);
	writel(1, regs + GE_FIRE_OFF);

	return 0;
}

/**
 * wmt_ge_hw_blit - Execute hardware-accelerated BitBLT
 */
int wmt_ge_hw_blit(struct wmt_drm_device *wmt, struct drm_gem_dma_object *src_gem,
		   struct drm_gem_dma_object *dest_gem, struct wmt_ge_op *req)
{
	void __iomem *regs = wmt->ge_regs;

	if (unlikely(!req->width || !req->height || !req->src_pitch || !req->dest_pitch ||
		     (req->src_pitch / 4) > WMT_GE_MAX_WIDTH ||
		     (req->dest_pitch / 4) > WMT_GE_MAX_WIDTH ||
		     req->height > WMT_GE_MAX_HEIGHT ||
		     req->src_y > WMT_GE_MAX_HEIGHT - req->height ||
		     req->dest_y > WMT_GE_MAX_HEIGHT - req->height ||
		     req->width > (req->src_pitch / 4) ||
		     req->src_x > (req->src_pitch / 4) - req->width ||
		     req->width > (req->dest_pitch / 4) ||
		     req->dest_x > (req->dest_pitch / 4) - req->width ||
		     (req->src_y + req->height) * req->src_pitch > src_gem->base.size ||
		     (req->dest_y + req->height) * req->dest_pitch > dest_gem->base.size))
		return -EINVAL;

	writel(3, regs + GE_DEPTH_OFF);
	writel(0, regs + GE_HIGHCOLOR_OFF);

	/* Source area */
	writel(src_gem->dma_addr, regs + GE_SRCBASE_OFF);
	writel((req->src_pitch / 4) - 1, regs + GE_SRCDISPW_OFF);
	writel((req->src_y + req->height) - 1, regs + GE_SRCDISPH_OFF);
	writel(req->src_x, regs + GE_SRCAREAX_OFF);
	writel(req->src_y, regs + GE_SRCAREAY_OFF);
	writel(req->width - 1, regs + GE_SRCAREAW_OFF);
	writel(req->height - 1, regs + GE_SRCAREAH_OFF);

	/* Destination area */
	writel(dest_gem->dma_addr, regs + GE_DESTBASE_OFF);
	writel((req->dest_pitch / 4) - 1, regs + GE_DESTDISPW_OFF);
	writel((req->dest_y + req->height) - 1, regs + GE_DESTDISPH_OFF);
	writel(req->dest_x, regs + GE_DESTAREAX_OFF);
	writel(req->dest_y, regs + GE_DESTAREAY_OFF);
	writel(req->width - 1, regs + GE_DESTAREAW_OFF);
	writel(req->height - 1, regs + GE_DESTAREAH_OFF);

	writel(GE_CMD_BLIT, regs + GE_COMMAND_OFF);
	writel(req->rop ? req->rop : GE_ROP_SRC_COPY, regs + GE_ROPCODE_OFF);
	writel(1, regs + GE_FIRE_OFF);

	return 0;
}

/*
 * =============================================================================
 * Custom Userspace IOCTLs
 * =============================================================================
 */

int wmt_drm_ioctl_ge_batch(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct wmt_ge_batch_req *req = data;
	struct wmt_ge_op *ops;
	struct wmt_drm_device *wmt;
	struct drm_gem_object *cached_dest_obj = NULL;
	struct drm_gem_object *cached_src_obj = NULL;
	u32 cached_dest_handle = 0;
	u32 cached_src_handle = 0;
	int ret = 0;

	if (req->num_ops == 0 || req->num_ops > 8192)
		return -EINVAL;

	ops = vmemdup_user(req->ops, req->num_ops * sizeof(*ops));
	if (IS_ERR(ops))
		return PTR_ERR(ops);

	wmt = to_wmt_drm(dev);

	mutex_lock(&wmt->ge_mutex);

	for (u32 i = 0; i < req->num_ops; i++) {
		struct wmt_ge_op *op = &ops[i];

		/* Destination GEM Caching */
		if (op->dest_handle != cached_dest_handle || !cached_dest_obj) {
			drm_gem_object_put(cached_dest_obj);

			cached_dest_obj = drm_gem_object_lookup(file_priv, op->dest_handle);
			if (!cached_dest_obj) {
				ret = -ENOENT;
				break;
			}
			cached_dest_handle = op->dest_handle;
		}

		/* Source GEM Caching */
		if (op->type == WMT_GE_OP_BLIT &&
		    (op->src_handle != cached_src_handle || !cached_src_obj)) {
			drm_gem_object_put(cached_src_obj);

			cached_src_obj = drm_gem_object_lookup(file_priv, op->src_handle);
			if (!cached_src_obj) {
				ret = -ENOENT;
				break;
			}
			cached_src_handle = op->src_handle;
		}

		ret = wmt_ge_sync(wmt);
		if (ret)
			break;

		if (op->type == WMT_GE_OP_FILL) {
			ret = wmt_ge_hw_fill(wmt, to_drm_gem_dma_obj(cached_dest_obj), op);
		} else if (op->type == WMT_GE_OP_BLIT) {
			ret = wmt_ge_hw_blit(wmt, to_drm_gem_dma_obj(cached_src_obj),
					     to_drm_gem_dma_obj(cached_dest_obj), op);
		} else {
			ret = -EINVAL;
		}

		if (ret)
			break;
	}

	wmt_ge_sync(wmt);
	mutex_unlock(&wmt->ge_mutex);

	drm_gem_object_put(cached_dest_obj);
	drm_gem_object_put(cached_src_obj);
	kvfree(ops);

	return ret;
}
