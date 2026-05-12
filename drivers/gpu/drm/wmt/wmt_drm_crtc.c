// SPDX-License-Identifier: GPL-2.0-only
/*
 * WonderMedia WM8505 DRM/KMS Graphics Driver
 *
 * CRTC and Display Pipeline
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/spinlock.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "wmt_drm.h"

/**
 * wmt_govr_set_timing - Translate DRM modes into GOVR timings
 */
static void wmt_govr_set_timing(struct wmt_drm_device *wmt,
				const struct drm_display_mode *mode)
{
	unsigned long t_rate = mode->clock * 1000;
	unsigned long best_pll = 0;
	unsigned long best_diff = ~0UL;
	struct clk *parent = clk_get_parent(wmt->clk);
	u32 div;
	long pll;
	int h_sync, h_bp, h_fp, h_start, h_end, h_all;
	int v_sync, v_bp, v_fp, v_start, v_end, v_all;

	/* Calculate pixel clock by iterating up to max divisor */
	for (div = 1; div <= 31 && best_diff; div++) {
		pll = clk_round_rate(parent, t_rate * div);
		if (pll > 0) {
			unsigned long diff = abs_diff(pll / div, t_rate);
			if (diff < best_diff) {
				best_diff = diff;
				best_pll = pll;
			}
		}
	}

	clk_set_rate(parent, best_pll);
	clk_set_rate(wmt->clk, t_rate);

	/* Apply the divider */
	writel((max_t(u32, DIV_ROUND_CLOSEST(clk_get_rate(wmt->clk), t_rate), 1) - 1) & GENMASK(6, 0),
	       wmt->govr_regs + WMT_GOVR_READ_CYC);

	/* Calculate display geometry offsets */
	h_sync = mode->hsync_end - mode->hsync_start;
	h_bp = mode->htotal - mode->hsync_end;
	h_fp = mode->hsync_start - mode->hdisplay;

	v_sync = mode->vsync_end - mode->vsync_start;
	v_bp = mode->vtotal - mode->vsync_end;
	v_fp = mode->vsync_start - mode->vdisplay;

	h_start = h_sync + h_bp;
	h_end = h_start + mode->hdisplay;
	h_all = h_end + h_fp;

	v_start = v_sync + v_bp + 1;
	v_end = v_start + mode->vdisplay;
	v_all = v_end + v_fp - 1;

	writel(h_start, wmt->govr_regs + WMT_GOVR_TIMING_H_START);
	writel(h_end, wmt->govr_regs + WMT_GOVR_TIMING_H_END);
	writel(h_all, wmt->govr_regs + WMT_GOVR_TIMING_H_ALL);
	writel(h_sync, wmt->govr_regs + WMT_GOVR_TIMING_H_SYNC);

	writel(v_start, wmt->govr_regs + WMT_GOVR_TIMING_V_START);
	writel(v_end, wmt->govr_regs + WMT_GOVR_TIMING_V_END);
	writel(v_all, wmt->govr_regs + WMT_GOVR_TIMING_V_ALL);

	writel(v_sync + 1, wmt->govr_regs + WMT_GOVR_TIMING_V_SYNC);
	writel(v_sync + 1, wmt->govr_regs + WMT_GOVR_TIMING_VBIE_LINE);
	writel(max(v_fp - 2, 1), wmt->govr_regs + WMT_GOVR_TIMING_PVBI_LINE);
}

/**
 * wmt_drm_irq - VBlank IRQ Handler
 */
irqreturn_t wmt_drm_irq(int irq, void *data)
{
	struct wmt_drm_device *wmt = data;
	u32 status = readl(wmt->vpp_regs + WMT_VPP_INTSTS);
	unsigned long flags;

	if (!(status & WMT_VPP_VBIS_BIT))
		return IRQ_NONE;

	/* Clear interrupt */
	writel(WMT_VPP_VBIS_BIT, wmt->vpp_regs + WMT_VPP_INTSTS);

	spin_lock_irqsave(&wmt->drm.event_lock, flags);
	if (wmt->pending_event) {
		if (wmt->defer_vblank) {
			/* Shadow register latch cycle missed, defer delivery to next interrupt */
			wmt->defer_vblank = false;
		} else {
			drm_crtc_send_vblank_event(&wmt->pipe.crtc, wmt->pending_event);
			drm_crtc_vblank_put(&wmt->pipe.crtc);
			wmt->pending_event = NULL;
		}
	}
	spin_unlock_irqrestore(&wmt->drm.event_lock, flags);

	drm_crtc_handle_vblank(&wmt->pipe.crtc);

	return IRQ_HANDLED;
}

/**
 * wmt_pipe_enable - CRTC enable callback
 */
static void wmt_pipe_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct wmt_drm_device *wmt = to_wmt_drm(pipe->crtc.dev);
	struct drm_gem_dma_object *gem;

	if (!plane_state->fb)
		return;

	gem = drm_fb_dma_get_gem_obj(plane_state->fb, 0);

	/* Reset hardware */
	writel(0, wmt->govr_regs + WMT_GOVR_TG);
	writel(0, wmt->govr_regs + WMT_GOVR_CB_ENABLE);
	writel(0, wmt->govr_regs + WMT_GOVR_XPAN);
	writel(0, wmt->govr_regs + WMT_GOVR_YPAN);

	/* Set physical buffer address */
	writel(gem->dma_addr, wmt->govr_regs + WMT_GOVR_FBADDR);
	writel(gem->dma_addr, wmt->govr_regs + WMT_GOVR_FBADDR1);

	/* Set format to ARGB8888 */
	writel(0x1c, wmt->govr_regs + WMT_GOVR_COLORSPACE);
	writel(1, wmt->govr_regs + WMT_GOVR_COLORSPACE1);

	/* Set resolution boundaries */
	writel(plane_state->fb->width, wmt->govr_regs + WMT_GOVR_XRES);
	writel(plane_state->fb->pitches[0] / 4, wmt->govr_regs + WMT_GOVR_XRES_VIRTUAL);

	wmt_govr_set_timing(wmt, &crtc_state->adjusted_mode);

	/* Set contrast */
	writel((0x80 << 16) | (0x80 << 8) | 0x80, wmt->govr_regs + WMT_GOVR_CONTRAST);

	/* Enable hardware */
	writel(0xf, wmt->govr_regs + WMT_GOVR_FHI);
	writel(4, wmt->govr_regs + WMT_GOVR_DVO_SET);
	writel(1, wmt->govr_regs + WMT_GOVR_MIF_ENABLE);
	writel(1, wmt->govr_regs + WMT_GOVR_REG_UPDATE);
	writel(1, wmt->govr_regs + WMT_GOVR_TG);

	msleep(20);

	drm_crtc_vblank_on(&pipe->crtc);
}

/**
 * wmt_pipe_disable - CRTC disable callback
 */
static void wmt_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct wmt_drm_device *wmt = to_wmt_drm(pipe->crtc.dev);
	struct drm_crtc *crtc = &pipe->crtc;
	unsigned long flags;

	drm_crtc_vblank_off(crtc);

	/* Disable hardware */
	writel(0, wmt->govr_regs + WMT_GOVR_TG);
	writel(0, wmt->govr_regs + WMT_GOVR_TIMING_V_SYNC);
	writel(0, wmt->govr_regs + WMT_GOVR_MIF_ENABLE);
	writel(0, wmt->govr_regs + WMT_GOVR_DVO_SET);
	writel(1, wmt->govr_regs + WMT_GOVR_REG_UPDATE);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (wmt->pending_event) {
		drm_crtc_send_vblank_event(crtc, wmt->pending_event);
		drm_crtc_vblank_put(crtc);
		wmt->pending_event = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

/**
 * wmt_pipe_enable_vblank - Vblank hardware interrupt enable
 */
static int wmt_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct wmt_drm_device *wmt = to_wmt_drm(pipe->crtc.dev);
	u32 inten;

	/* Clear stale interrupts */
	writel(WMT_VPP_VBIS_BIT, wmt->vpp_regs + WMT_VPP_INTSTS);

	inten = readl(wmt->vpp_regs + WMT_VPP_INTEN);
	inten |= WMT_VPP_VBIS_BIT;
	writel(inten, wmt->vpp_regs + WMT_VPP_INTEN);

	return 0;
}

/**
 * wmt_pipe_disable_vblank - Vblank hardware interrupt disable
 */
static void wmt_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct wmt_drm_device *wmt = to_wmt_drm(pipe->crtc.dev);
	u32 inten = readl(wmt->vpp_regs + WMT_VPP_INTEN);

	inten &= ~WMT_VPP_VBIS_BIT;
	writel(inten, wmt->vpp_regs + WMT_VPP_INTEN);
}

/**
 * wmt_pipe_update - Plane update callback for atomic page-flips
 */
static void wmt_pipe_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *old_state)
{
	struct wmt_drm_device *wmt = to_wmt_drm(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	unsigned long flags;

	/* Ensure GE operations complete before latching address */
	wmt_ge_sync(wmt);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (state->fb && state->fb != old_state->fb) {
		struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(state->fb, 0);

		writel(gem->dma_addr, wmt->govr_regs + WMT_GOVR_FBADDR);
		writel(gem->dma_addr, wmt->govr_regs + WMT_GOVR_FBADDR1);
		writel(state->src_x >> 16, wmt->govr_regs + WMT_GOVR_XPAN);
		writel(state->src_y >> 16, wmt->govr_regs + WMT_GOVR_YPAN);
	}

	if (crtc->state->event) {
		u32 status = readl(wmt->vpp_regs + WMT_VPP_INTSTS);

		wmt->pending_event = crtc->state->event;
		crtc->state->event = NULL;

		/* Guard against hardware latch misses if actively in the VBI */
		wmt->defer_vblank = status & WMT_VPP_VBIS_BIT;

		if (drm_crtc_vblank_get(crtc) != 0) {
			drm_crtc_send_vblank_event(crtc, wmt->pending_event);
			wmt->pending_event = NULL;
			wmt->defer_vblank = false;
		}
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

const struct drm_simple_display_pipe_funcs wmt_pipe_funcs = {
	.enable			= wmt_pipe_enable,
	.disable		= wmt_pipe_disable,
	.update			= wmt_pipe_update,
	.enable_vblank		= wmt_pipe_enable_vblank,
	.disable_vblank		= wmt_pipe_disable_vblank,
};

const uint32_t wmt_formats[2] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};
