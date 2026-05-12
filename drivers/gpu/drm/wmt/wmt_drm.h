// SPDX-License-Identifier: GPL-2.0-only
/*
 * WonderMedia WM8505 DRM/KMS Graphics Driver
 *
 * Copyright (C) 2026 Logan Russell <me@lrussell.net>
 */

#ifndef _WMT_DRM_H_
#define _WMT_DRM_H_

#include <linux/io.h>
#include <linux/irqreturn.h>
#include <drm/drm_device.h>
#include <drm/drm_simple_kms_helper.h>

/*
 * =============================================================================
 * Register Offsets
 * =============================================================================
 */

/* VPP (Video Post-Processing) registers */
#define WMT_VPP_INTSTS			0x04
#define WMT_VPP_INTEN			0x08
#define WMT_VPP_VBIS_BIT		BIT(9)

/* GOVR (Graphics Output) registers */
#define WMT_GOVR_COLORSPACE1		0x030
#define WMT_GOVR_MIF_ENABLE		0x080
#define WMT_GOVR_FBADDR			0x090
#define WMT_GOVR_FBADDR1		0x094
#define WMT_GOVR_XRES			0x098
#define WMT_GOVR_XRES_VIRTUAL		0x09c
#define WMT_GOVR_XPAN			0x0a0
#define WMT_GOVR_YPAN			0x0a4
#define WMT_GOVR_FHI			0x0a8
#define WMT_GOVR_REG_UPDATE		0x0e4
#define WMT_GOVR_TG			0x100
#define WMT_GOVR_READ_CYC		0x104
#define WMT_GOVR_TIMING_H_ALL		0x108
#define WMT_GOVR_TIMING_V_ALL		0x10c
#define WMT_GOVR_TIMING_V_START		0x110
#define WMT_GOVR_TIMING_V_END		0x114
#define WMT_GOVR_TIMING_H_START		0x118
#define WMT_GOVR_TIMING_H_END		0x11c
#define WMT_GOVR_TIMING_VBIE_LINE	0x120
#define WMT_GOVR_TIMING_PVBI_LINE	0x124
#define WMT_GOVR_TIMING_V_SYNC		0x128
#define WMT_GOVR_TIMING_H_SYNC		0x12c
#define WMT_GOVR_DVO_SET		0x148
#define WMT_GOVR_CB_ENABLE		0x150
#define WMT_GOVR_CONTRAST		0x1b8
#define WMT_GOVR_BRIGHTNESS		0x1bc
#define WMT_GOVR_COLORSPACE		0x1e4

/* GE (Graphics Engine) registers */
#define GE_COMMAND_OFF			0x00
#define GE_DEPTH_OFF			0x04
#define GE_HIGHCOLOR_OFF		0x08
#define GE_ROPCODE_OFF			0x14
#define GE_FIRE_OFF			0x18
#define GE_SRCBASE_OFF			0x20
#define GE_SRCDISPW_OFF			0x24
#define GE_SRCDISPH_OFF			0x28
#define GE_SRCAREAX_OFF			0x2c
#define GE_SRCAREAY_OFF			0x30
#define GE_SRCAREAW_OFF			0x34
#define GE_SRCAREAH_OFF			0x38
#define GE_DESTBASE_OFF			0x3c
#define GE_DESTDISPW_OFF		0x40
#define GE_DESTDISPH_OFF		0x44
#define GE_DESTAREAX_OFF		0x48
#define GE_DESTAREAY_OFF		0x4c
#define GE_DESTAREAW_OFF		0x50
#define GE_DESTAREAH_OFF		0x54
#define GE_PAT0C_OFF			0x88
#define GE_DELAY_OFF			0xe8
#define GE_ENABLE_OFF			0xec
#define GE_STATUS_OFF			0xf8

/*
 * =============================================================================
 * GE ROP Codes and Commands
 * =============================================================================
 */

#define GE_ROP_SRC_COPY			0xcc
#define GE_ROP_PAT_COPY			0xf0
#define GE_ROP_XOR			0x5a

#define GE_CMD_BLIT			0x01

/*
 * =============================================================================
 * Hardware Limits
 * =============================================================================
 */

#define WMT_GE_MAX_WIDTH		2048
#define WMT_GE_MAX_HEIGHT		2048

/*
 * =============================================================================
 * Driver Structures
 * =============================================================================
 */

struct wmt_drm_device {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector *connector;

	void __iomem *vpp_regs;
	void __iomem *govr_regs;
	struct clk *clk;

	void __iomem *ge_regs;
	struct mutex ge_mutex;

	struct drm_pending_vblank_event *pending_event;
	bool defer_vblank;
};

#define to_wmt_drm(x) container_of(x, struct wmt_drm_device, drm)

/*
 * =============================================================================
 * Custom Userspace IOCTLs
 * =============================================================================
 */

#define WMT_GE_OP_FILL			0x01
#define WMT_GE_OP_BLIT			0x02

struct wmt_ge_op {
	__u32 type;
	__u32 rop;
	__u32 dest_handle;
	__u32 dest_pitch;
	__u32 dest_x;
	__u32 dest_y;
	__u32 width;
	__u32 height;
	__u32 color;
	__u32 src_handle;
	__u32 src_pitch;
	__u32 src_x;
	__u32 src_y;
};

struct wmt_ge_batch_req {
	struct wmt_ge_op __user *ops;
	__u32 num_ops;
};

#define DRM_WMT_GE_BATCH		0x00
#define DRM_IOCTL_WMT_GE_BATCH		DRM_IOW(DRM_COMMAND_BASE + DRM_WMT_GE_BATCH, struct wmt_ge_batch_req)

/*
 * =============================================================================
 * Internal API
 * =============================================================================
 */

struct drm_gem_dma_object;
struct drm_fb_helper;

/* wmt_drm_crtc.c */
extern const struct drm_simple_display_pipe_funcs wmt_pipe_funcs;
extern const uint32_t wmt_formats[2];
irqreturn_t wmt_drm_irq(int irq, void *data);

/* wmt_drm_ge.c */
int wmt_ge_sync(struct wmt_drm_device *wmt);
int wmt_ge_hw_fill(struct wmt_drm_device *wmt, struct drm_gem_dma_object *gem,
		   struct wmt_ge_op *req);
int wmt_ge_hw_blit(struct wmt_drm_device *wmt, struct drm_gem_dma_object *src_gem,
		   struct drm_gem_dma_object *dest_gem, struct wmt_ge_op *req);
int wmt_drm_ioctl_ge_batch(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);

/* wmt_drm_fb.c */
void wmt_fbdev_probe_hook(struct drm_fb_helper *fb_helper);

#endif /* _WMT_DRM_H_ */
