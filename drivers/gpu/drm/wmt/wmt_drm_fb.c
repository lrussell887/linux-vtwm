// SPDX-License-Identifier: GPL-2.0-only
/*
 * WonderMedia WM8505 DRM/KMS Graphics Driver
 *
 * FBCon Hardware Acceleration
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#include <linux/fb.h>
#include <linux/mutex.h>
#include <linux/panic_notifier.h>

#include <drm/drm_fb_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

#include "wmt_drm.h"

/*
 * =============================================================================
 * FBCon Hardware Acceleration
 * =============================================================================
 */

static struct fb_ops wmt_accelerated_fb_ops;

static void wmt_fb_fillrect(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct drm_fb_helper *fb_helper = info->par;
	struct wmt_drm_device *wmt = to_wmt_drm(fb_helper->dev);
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(fb_helper->buffer->gem);

	if (unlikely(oops_in_progress || in_atomic())) {
		sys_fillrect(info, rect);
		return;
	}

	struct wmt_ge_op req = {
		.type		= WMT_GE_OP_FILL,
		.dest_pitch	= info->fix.line_length,
		.dest_x		= rect->dx,
		.dest_y		= rect->dy,
		.width		= rect->width,
		.height		= rect->height,
		.rop		= (rect->rop == ROP_XOR) ? GE_ROP_XOR : GE_ROP_PAT_COPY,
		.color		= (info->fix.visual == FB_VISUAL_TRUECOLOR ||
				   info->fix.visual == FB_VISUAL_DIRECTCOLOR) ?
				  ((u32 *)info->pseudo_palette)[rect->color] : rect->color
	};

	mutex_lock(&wmt->ge_mutex);
	if (!wmt_ge_sync(wmt)) {
		wmt_ge_hw_fill(wmt, dma_obj, &req);
		wmt_ge_sync(wmt);
	}
	mutex_unlock(&wmt->ge_mutex);
}

static void wmt_fb_copyarea(struct fb_info *info, const struct fb_copyarea *area)
{
	struct drm_fb_helper *fb_helper = info->par;
	struct wmt_drm_device *wmt = to_wmt_drm(fb_helper->dev);
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(fb_helper->buffer->gem);

	if (unlikely(oops_in_progress || in_atomic())) {
		sys_copyarea(info, area);
		return;
	}

	struct wmt_ge_op req = {
		.type		= WMT_GE_OP_BLIT,
		.src_pitch	= info->fix.line_length,
		.dest_pitch	= info->fix.line_length,
		.src_x		= area->sx,
		.src_y		= area->sy,
		.dest_x		= area->dx,
		.dest_y		= area->dy,
		.width		= area->width,
		.height		= area->height,
		.rop		= GE_ROP_SRC_COPY
	};

	mutex_lock(&wmt->ge_mutex);
	if (!wmt_ge_sync(wmt)) {
		wmt_ge_hw_blit(wmt, dma_obj, dma_obj, &req);
		wmt_ge_sync(wmt);
	}
	mutex_unlock(&wmt->ge_mutex);
}

void wmt_fbdev_probe_hook(struct drm_fb_helper *fb_helper)
{
	wmt_accelerated_fb_ops = *fb_helper->info->fbops;
	wmt_accelerated_fb_ops.fb_fillrect = wmt_fb_fillrect;
	wmt_accelerated_fb_ops.fb_copyarea = wmt_fb_copyarea;

	fb_helper->info->fbops = &wmt_accelerated_fb_ops;
	dev_info(fb_helper->dev->dev, "Console rendering offloaded to WMT GE\n");
}
