/*
 * Decode framebuffer attributes from raw vMMIO
 *
 * Copyright(c) 2011-2013 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/connector.h>
#include "vgt.h"
#include "fb_decoder.h"
#include <uapi/drm/drm_fourcc.h>
#include <uapi/drm/i915_drm.h>

/* non-supported format has bpp default to 0 */
static struct pixel_format hsw_pixel_formats[FORMAT_NUM] = {
	[0b0010]  = {DRM_FORMAT_C8, 8, "8-bit Indexed"},
	[0b0101]  = {DRM_FORMAT_RGB565, 16, "16-bit BGRX (5:6:5 MSB-R:G:B)"},
	[0b0110]  = {DRM_FORMAT_XRGB8888, 32, "32-bit BGRX (8:8:8:8 MSB-X:R:G:B)"},
	[0b1000]  = {DRM_FORMAT_XBGR2101010, 32, "32-bit RGBX (2:10:10:10 MSB-X:B:G:R)"},
	[0b1010] = {DRM_FORMAT_XRGB2101010, 32, "32-bit BGRX (2:10:10:10 MSB-X:R:G:B)"},
	[0b1100] = {DRM_FORMAT_XRGB161616_VGT, 64, "64-bit RGBX Floating Point(16:16:16:16 MSB-X:B:G:R)"},
	[0b1110] = {DRM_FORMAT_XBGR8888, 32, "32-bit RGBX (8:8:8:8 MSB-X:B:G:R)"},
};

static struct pixel_format skl_pixel_formats[] = {
	{DRM_FORMAT_YUYV, 16, "16-bit packed YUYV (8:8:8:8 MSB-V:Y2:U:Y1)"},
	{DRM_FORMAT_UYVY, 16, "16-bit packed UYVY (8:8:8:8 MSB-Y2:V:Y1:U)"},
	{DRM_FORMAT_YVYU, 16, "16-bit packed YVYU (8:8:8:8 MSB-U:Y2:V:Y1)"},
	{DRM_FORMAT_VYUY, 16, "16-bit packed VYUY (8:8:8:8 MSB-Y2:U:Y1:V)"},

	{DRM_FORMAT_C8, 8, "8-bit Indexed"},
	{DRM_FORMAT_RGB565, 16, "16-bit BGRX (5:6:5 MSB-R:G:B)"},
	{DRM_FORMAT_ABGR8888, 32, "32-bit RGBA (8:8:8:8 MSB-A:B:G:R)"},
	{DRM_FORMAT_XBGR8888, 32, "32-bit RGBX (8:8:8:8 MSB-X:B:G:R)"},

	{DRM_FORMAT_ARGB8888, 32, "32-bit BGRA (8:8:8:8 MSB-A:R:G:B)"},
	{DRM_FORMAT_XRGB8888, 32, "32-bit BGRX (8:8:8:8 MSB-X:R:G:B)"},
	{DRM_FORMAT_XBGR2101010, 32, "32-bit RGBX (2:10:10:10 MSB-X:B:G:R)"},
	{DRM_FORMAT_XRGB2101010, 32, "32-bit BGRX (2:10:10:10 MSB-X:R:G:B)"},

	{DRM_FORMAT_XRGB161616_VGT, 64, "64-bit XRGB (16:16:16:16 MSB-X:R:G:B)"},
	{DRM_FORMAT_XBGR161616_VGT, 64, "64-bit XBGR (16:16:16:16 MSB-X:B:G:R)"},

	/* non-supported format has bpp default to 0 */
	{0, 0, NULL},
};

#define FORMAT_NUM_SRRITE	(1 << 3)

/* The formats described in the sprite format field are the 1st level of
 * cases RGB and YUV formats are further refined by the color_order and
 * yuv_order fields to cover the full set of possible formats.
 */

static struct pixel_format hsw_pixel_formats_sprite[FORMAT_NUM_SRRITE] = {
	[0b000]  = {DRM_FORMAT_YUV422, 16, "YUV 16-bit 4:2:2 packed"},
	[0b001]  = {DRM_FORMAT_XRGB2101010, 32, "RGB 32-bit 2:10:10:10"},
	[0b010]  = {DRM_FORMAT_XRGB8888, 32, "RGB 32-bit 8:8:8:8"},
	[0b011]  = {DRM_FORMAT_XRGB161616_VGT, 64, "RGB 64-bit 16:16:16:16 Floating Point"},
	[0b100] = {DRM_FORMAT_AYUV, 32, "YUV 32-bit 4:4:4 packed (8:8:8:8 MSB-X:Y:U:V)"},
};

static int skl_format_to_drm(int format, bool rgb_order, bool alpha, int yuv_order)
{
	int skl_pixel_formats_index = 14;

	switch (format) {
	case PLANE_CTL_FORMAT_INDEXED:
		skl_pixel_formats_index = 4;
		break;
	case PLANE_CTL_FORMAT_RGB_565:
		skl_pixel_formats_index = 5;
		break;
	case PLANE_CTL_FORMAT_XRGB_8888:
		if (rgb_order) {
			if (alpha)
				skl_pixel_formats_index = 6;
			else
				skl_pixel_formats_index = 7;
		} else {
			if (alpha)
				skl_pixel_formats_index = 8;
			else
				skl_pixel_formats_index = 9;
		}
		break;
	case PLANE_CTL_FORMAT_XRGB_2101010:
		if (rgb_order)
			skl_pixel_formats_index = 10;
		else
			skl_pixel_formats_index = 11;
		break;

	case PLANE_CTL_FORMAT_XRGB_16161616F:
		if (rgb_order)
			skl_pixel_formats_index = 12;
		else
			skl_pixel_formats_index = 13;
		break;

	case PLANE_CTL_FORMAT_YUV422:
		skl_pixel_formats_index = yuv_order >> 16;
		if (skl_pixel_formats_index > 3)
			return -EINVAL;
		break;
	default:
		break;
	}

	return skl_pixel_formats_index;
}

int vgt_get_pixel_format_skl(u32 plane_ctl,
	struct vgt_common_plane_format *com_plane_fmt, enum vgt_plane_type plane)
{
	com_plane_fmt->tiled = plane_ctl & PLANE_CTL_TILED_MASK;
	com_plane_fmt->fmt_index = skl_format_to_drm(
		plane_ctl & PLANE_CTL_FORMAT_MASK,
		plane_ctl & PLANE_CTL_ORDER_RGBX,
		plane_ctl & PLANE_CTL_ALPHA_MASK,
		plane_ctl & PLANE_CTL_YUV422_ORDER_MASK);

	if (com_plane_fmt->fmt_index < 0)
		return -EINVAL;

	memcpy(&com_plane_fmt->gen_pixel_format,
		&skl_pixel_formats[com_plane_fmt->fmt_index],
		sizeof(struct pixel_format));

	com_plane_fmt->stride_mask = SKL_PLANE_STRIDE_MASK;

	return 0;
}

int vgt_get_pixel_format_preskl(u32 plane_ctl,
	struct vgt_common_plane_format *common_plane, enum vgt_plane_type plane)
{
	u32 color_order, yuv_order;
	int drm_format;

	if (plane != PRIMARY_PLANE && plane != SPRITE_PLANE)
		return -EINVAL;

	if (plane == PRIMARY_PLANE) {
		common_plane->tiled = !!(plane_ctl & DISPPLANE_TILED);
		common_plane->fmt_index = (plane_ctl & DISPPLANE_PIXFORMAT_MASK)
						>> _PRI_PLANE_FMT_SHIFT;

		memcpy(&common_plane->gen_pixel_format,
			&hsw_pixel_formats[common_plane->fmt_index],
			sizeof(struct pixel_format));

		common_plane->stride_mask = _PRI_PLANE_STRIDE_MASK;
	} else {
		common_plane->tiled = !!(plane_ctl & SPRITE_TILED);
		common_plane->fmt_index = (plane_ctl & SPRITE_PIXFORMAT_MASK)
						>> _SPRITE_FMT_SHIFT;
		color_order = !!(plane_ctl & SPRITE_RGB_ORDER_RGBX);
		yuv_order = (plane_ctl & SPRITE_YUV_BYTE_ORDER_MASK)
				>> _SPRITE_YUV_ORDER_SHIFT;

		memcpy(&common_plane->gen_pixel_format,
			&hsw_pixel_formats_sprite[common_plane->fmt_index],
			sizeof(struct pixel_format));
		common_plane->stride_mask = _SPRITE_STRIDE_MASK;

		drm_format = common_plane->gen_pixel_format.drm_format;
		if (!color_order) {
			if (drm_format == DRM_FORMAT_XRGB2101010)
				drm_format = DRM_FORMAT_XBGR2101010;
			else if (drm_format == DRM_FORMAT_XRGB8888)
				drm_format = DRM_FORMAT_XBGR8888;
			else if (drm_format == DRM_FORMAT_XRGB161616_VGT)
				drm_format = DRM_FORMAT_XBGR161616_VGT;
		}

		if (drm_format == DRM_FORMAT_YUV422) {
			switch (yuv_order) {
			case	0:
				drm_format = DRM_FORMAT_YUYV;
				break;
			case	1:
				drm_format = DRM_FORMAT_UYVY;
				break;
			case	2:
				drm_format = DRM_FORMAT_YVYU;
				break;
			case	3:
				drm_format = DRM_FORMAT_VYUY;
				break;
			default:
				/* yuv_order has only 2 bits */
				return -EINVAL;
			}
		}
		common_plane->gen_pixel_format.drm_format = drm_format;
	}

	return 0;
}

static u32 vgt_get_stride(struct vgt_device *vgt, enum vgt_plane_type plane, int pipe, u32 tiled,
			int stride_mask, int bpp)
{
	u32 stride_reg = 0;
	u32 stride = 0;

	if(plane == PRIMARY_PLANE)
		stride_reg = __vreg(vgt, VGT_DSPSTRIDE(pipe)) & stride_mask;
	else if(plane == SPRITE_PLANE)
		stride_reg = __vreg(vgt, VGT_SPRSTRIDE(pipe)) & stride_mask;
	else
		vgt_warn("vgt_get_stride: unsupported plane:%d\n", plane);

	stride = stride_reg;

	if (IS_SKLPLUS(vgt->pdev)) {
		switch (tiled) {
		case PLANE_CTL_TILED_LINEAR:
			stride = stride_reg * 64;
			break;
		case PLANE_CTL_TILED_X:
			stride = stride_reg * 512;
			break;
		case PLANE_CTL_TILED_Y:
			stride = stride_reg * 128;
			break;
		case PLANE_CTL_TILED_YF:
			if (bpp == 8)
				stride = stride_reg * 64;
			else if (bpp == 16 || bpp == 32 || bpp == 64)
				stride = stride_reg * 128;
			else
				vgt_warn("skl: unsupported bpp:%d\n", bpp);
			break;
		default:
			vgt_warn("skl: unsupported tile format:%x\n", tiled);
		}
	}

        return stride;
}

int vgt_decode_primary_plane_format(struct vgt_device *vgt,
	int pipe, struct vgt_primary_plane_format *plane)
{
	struct vgt_common_plane_format com_plane_fmt;
	u32 val;

	val = __vreg(vgt, VGT_DSPCNTR(pipe));
	plane->enabled = !!(val & DISPLAY_PLANE_ENABLE);
	if (!plane->enabled)
		return 0;

	if (!vgt->pdev->vgt_get_pixel_format) {
		vgt_err("no vgt_get_pixel_format initialized\n");
		return -EINVAL;
	}
	if (vgt->pdev->vgt_get_pixel_format(val, &com_plane_fmt, PRIMARY_PLANE)) {
		vgt_err("get pixel format error\n");
		return -EINVAL;
	}

	if (!com_plane_fmt.gen_pixel_format.bpp) {
		vgt_err("Non-supported pixel format (0x%x)\n", com_plane_fmt.fmt_index);
		return -EINVAL;
	}

	plane->tiled = com_plane_fmt.tiled;
	plane->hw_format = com_plane_fmt.fmt_index;
	plane->bpp = com_plane_fmt.gen_pixel_format.bpp;
	plane->drm_format = com_plane_fmt.gen_pixel_format.drm_format;
	memcpy(plane->drm_fmt_desc, com_plane_fmt.gen_pixel_format.desc, MAX_DRM_STR_SZ);

	plane->base = __vreg(vgt, VGT_DSPSURF(pipe)) & GTT_PAGE_MASK;
	plane->stride = vgt_get_stride(vgt, PRIMARY_PLANE, pipe, plane->tiled,
					com_plane_fmt.stride_mask, plane->bpp);

	plane->width = (__vreg(vgt, VGT_PIPESRC(pipe)) & _PIPE_H_SRCSZ_MASK) >>
				_PIPE_H_SRCSZ_SHIFT;
	plane->width += 1;
	plane->height = (__vreg(vgt, VGT_PIPESRC(pipe)) &
			 _PIPE_V_SRCSZ_MASK) >> _PIPE_V_SRCSZ_SHIFT;
	plane->height += 1;	/* raw height is one minus the real value */

	val = __vreg(vgt, VGT_DSPTILEOFF(pipe));
	plane->x_offset = (val & _PRI_PLANE_X_OFF_MASK) >>
			   _PRI_PLANE_X_OFF_SHIFT;
	plane->y_offset = (val & _PRI_PLANE_Y_OFF_MASK) >>
			   _PRI_PLANE_Y_OFF_SHIFT;
	return 0;
}

#define CURSOR_MODE_NUM	(1 << 6)
struct cursor_mode_format {
	int	drm_format;	/* Pixel format in DRM definition */
	u8	bpp;		/* Bits per pixel; 0 indicates invalid */
	u32	width;		/* In pixel */
	u32	height;		/* In lines */
	char	*desc;		/* The description */
};

/* non-supported format has bpp default to 0 */
static struct cursor_mode_format hsw_cursor_mode_formats[CURSOR_MODE_NUM] = {
	[0b100010]  = {DRM_FORMAT_ARGB8888, 32, 128, 128,"128x128 32bpp ARGB"},
	[0b100011]  = {DRM_FORMAT_ARGB8888, 32, 256, 256, "256x256 32bpp ARGB"},
	[0b100111]  = {DRM_FORMAT_ARGB8888, 32, 64, 64, "64x64 32bpp ARGB"},
	[0b000111]  = {DRM_FORMAT_ARGB8888, 32, 64, 64, "64x64 32bpp ARGB"},//actually inverted... figure this out later
};
int vgt_decode_cursor_plane_format(struct vgt_device *vgt,
	int pipe, struct vgt_cursor_plane_format *plane)
{
	u32 val, mode;
	u32 alpha_plane, alpha_force;

	val = __vreg(vgt, VGT_CURCNTR(pipe));
	mode = val & _CURSOR_MODE;
	plane->enabled = (mode != _CURSOR_MODE_DISABLE);
	if (!plane->enabled)
		return 0;

	if (!hsw_cursor_mode_formats[mode].bpp) {
		vgt_err("Non-supported cursor mode (0x%x)\n", mode);
		return -EINVAL;
	}
	plane->mode = mode;
	plane->bpp = hsw_cursor_mode_formats[mode].bpp;
	plane->drm_format = hsw_cursor_mode_formats[mode].drm_format;
	plane->width = hsw_cursor_mode_formats[mode].width;
	plane->height = hsw_cursor_mode_formats[mode].height;

	alpha_plane = (val & _CURSOR_ALPHA_PLANE_MASK) >>
				_CURSOR_ALPHA_PLANE_SHIFT;
	alpha_force = (val & _CURSOR_ALPHA_FORCE_MASK) >>
				_CURSOR_ALPHA_FORCE_SHIFT;
	if (alpha_plane || alpha_force)
		vgt_warn("alpha_plane=0x%x, alpha_force=0x%x\n",
			alpha_plane, alpha_force);

	plane->base = __vreg(vgt, VGT_CURBASE(pipe)) & GTT_PAGE_MASK;

	val = __vreg(vgt, VGT_CURPOS(pipe));
	plane->x_pos = (val & _CURSOR_POS_X_MASK) >> _CURSOR_POS_X_SHIFT;
	plane->x_sign = (val & _CURSOR_SIGN_X_MASK) >> _CURSOR_SIGN_X_SHIFT;
	plane->y_pos = (val & _CURSOR_POS_Y_MASK) >> _CURSOR_POS_Y_SHIFT;
	plane->y_sign = (val & _CURSOR_SIGN_Y_MASK) >> _CURSOR_SIGN_Y_SHIFT;
	plane->x_hot = __vreg(vgt, vgt_info_off(xhot));
	plane->y_hot = __vreg(vgt, vgt_info_off(xhot));

	return 0;
}

/* Non-supported format has bpp default to 0 */
int vgt_decode_sprite_plane_format(struct vgt_device *vgt,
	int pipe, struct vgt_sprite_plane_format *plane)
{
	struct vgt_common_plane_format com_plane_fmt;
	u32 val;

	val = __vreg(vgt, VGT_SPRCTL(pipe));
	plane->enabled = !!(val & SPRITE_ENABLE);
	if (!plane->enabled)
		return 0;

	if (!vgt->pdev->vgt_get_pixel_format) {
		vgt_err("no vgt_get_pixel_format initialized\n");
		return -EINVAL;
	}
	if (vgt->pdev->vgt_get_pixel_format(val, &com_plane_fmt, SPRITE_PLANE)) {
		vgt_err("get pixel format error\n");
		return -EINVAL;
	}

	if (!com_plane_fmt.gen_pixel_format.bpp) {
		vgt_err("Non-supported pixel format (0x%x)\n", com_plane_fmt.fmt_index);
		return -EINVAL;
	}

	plane->tiled = com_plane_fmt.tiled;
	plane->hw_format = com_plane_fmt.fmt_index;
	plane->bpp = com_plane_fmt.gen_pixel_format.bpp;
	plane->drm_format = com_plane_fmt.gen_pixel_format.drm_format;
	memcpy(plane->drm_fmt_desc, com_plane_fmt.gen_pixel_format.desc, MAX_DRM_STR_SZ);

	plane->base = __vreg(vgt, VGT_SPRSURF(pipe)) & GTT_PAGE_MASK;
	plane->stride = vgt_get_stride(vgt, SPRITE_PLANE, pipe, plane->tiled,
					com_plane_fmt.stride_mask, plane->bpp);

	val = __vreg(vgt, VGT_SPRSIZE(pipe));
	plane->height = (val & _SPRITE_SIZE_HEIGHT_MASK) >>
		_SPRITE_SIZE_HEIGHT_SHIFT;
	plane->width = (val & _SPRITE_SIZE_WIDTH_MASK) >> _SPRITE_SIZE_WIDTH_SHIFT;
	plane->height += 1;	/* raw height is one minus the real value */
	plane->width += 1;		/* raw width is one minus the real value */

	val = __vreg(vgt, VGT_SPRPOS(pipe));
	plane->x_pos = (val & _SPRITE_POS_X_MASK) >> _SPRITE_POS_X_SHIFT;
	plane->y_pos = (val & _SPRITE_POS_Y_MASK) >> _SPRITE_POS_Y_SHIFT;

	val = __vreg(vgt, VGT_SPROFFSET(pipe));
	plane->x_offset = (val & _SPRITE_OFFSET_START_X_MASK) >>
			   _SPRITE_OFFSET_START_X_SHIFT;
	plane->y_offset = (val & _SPRITE_OFFSET_START_Y_MASK) >>
			   _SPRITE_OFFSET_START_Y_SHIFT;
	return 0;
}

static void vgt_dump_primary_plane_format(struct dump_buffer *buf,
	struct vgt_primary_plane_format *plane)
{
	dump_string(buf, "Primary Plane: [%s]\n",
		plane->enabled ? "Enabled" : "Disabled");
	if (!plane->enabled)
		return;

	dump_string(buf, "  tiled: %s\n", plane->tiled ? "yes" : "no");
	if (!plane->bpp) {
		dump_string(buf, "  BROKEN FORMAT (ZERO bpp)\n");
		return;
	}

	dump_string(buf, "  bpp: %d\n", plane->bpp);
	dump_string(buf, "  drm_format: 0x%08x: %s\n", plane->drm_format,
		plane->drm_fmt_desc);
	dump_string(buf, "  base: 0x%x\n", plane->base);
	dump_string(buf, "  x-off: %d\n", plane->x_offset);
	dump_string(buf, "  y-off: %d\n", plane->y_offset);
	dump_string(buf, "  width: %d\n", plane->width);
	dump_string(buf, "  height: %d\n", plane->height);
}

static void vgt_dump_cursor_plane_format(struct dump_buffer *buf,
	struct vgt_cursor_plane_format *plane)
{
	dump_string(buf, "Cursor Plane: [%s]\n",
		plane->enabled ? "Enabled" : "Disabled");
	if (!plane->enabled)
		return;

	if (!plane->bpp) {
		dump_string(buf, "  BROKEN FORMAT (ZERO bpp)\n");
		return;
	}

	dump_string(buf, "  bpp: %d\n", plane->bpp);
	dump_string(buf, "  mode: 0x%08x: %s\n", plane->mode,
		hsw_cursor_mode_formats[plane->mode].desc);
	dump_string(buf, "  drm_format: 0x%08x\n", plane->drm_format);
	dump_string(buf, "  base: 0x%x\n", plane->base);
	dump_string(buf, "  x-pos: %d\n", plane->x_pos);
	dump_string(buf, "  y-pos: %d\n", plane->y_pos);
	dump_string(buf, "  x-sign: %d\n", plane->x_sign);
	dump_string(buf, "  y-sign: %d\n", plane->y_sign);
	dump_string(buf, "  width: %d\n", plane->width);
	dump_string(buf, "  height: %d\n", plane->height);
}

static void vgt_dump_sprite_plane_format(struct dump_buffer *buf,
	struct vgt_sprite_plane_format *plane)
{
	dump_string(buf, "Sprite Plane: [%s]\n",
		plane->enabled ? "Enabled" : "Disabled");
	if (!plane->enabled)
		return;

	dump_string(buf, "  tiled: %s\n", plane->tiled ? "yes" : "no");
	if (!plane->bpp) {
		dump_string(buf, "  BROKEN FORMAT (ZERO bpp)\n");
		return;
	}

	dump_string(buf, "  bpp: %d\n", plane->bpp);
	dump_string(buf, "  drm_format: 0x%08x: %s\n",
		plane->drm_format, plane->drm_fmt_desc);
	dump_string(buf, "  base: 0x%x\n", plane->base);
	dump_string(buf, "  x-off: %d\n", plane->x_offset);
	dump_string(buf, "  y-off: %d\n", plane->y_offset);
	dump_string(buf, "  x-pos: %d\n", plane->x_pos);
	dump_string(buf, "  y-pos: %d\n", plane->y_pos);
	dump_string(buf, "  width: %d\n", plane->width);
	dump_string(buf, "  height: %d\n", plane->height);
}


int vgt_dump_fb_format(struct dump_buffer *buf, struct vgt_fb_format *fb)
{
	int i;


	for (i = 0; i < MAX_INTEL_PIPES; i++) {
		struct vgt_pipe_format *pipe = &fb->pipes[i];
		dump_string(buf, "[PIPE-%d]:\n", i);
		vgt_dump_primary_plane_format(buf, &pipe->primary);
		vgt_dump_cursor_plane_format(buf, &pipe->cursor);
		vgt_dump_sprite_plane_format(buf, &pipe->sprite);
		dump_string(buf, "Pipe remapping\n");
		if (pipe->ddi_port == DDI_PORT_NONE) {
			dump_string(buf, "  no mapping available for this pipe\n");
		} else {
			char port_id;
			switch (pipe->ddi_port) {
			case DDI_PORT_B:
				port_id = 'B'; break;
			case DDI_PORT_C:
				port_id = 'C'; break;
			case DDI_PORT_D:
				port_id = 'D'; break;
			case DDI_PORT_E:
			default:
				port_id = 'E'; break;
			}
			dump_string(buf, "  virtual pipe:%d -> DDI PORT:%c\n",
				 i, port_id);
		}
	}
	dump_string(buf, "\n");
	return 0;
}

/* Debug facility */

static void vgt_show_fb_format(int vmid, struct vgt_fb_format *fb)
{
	struct dump_buffer buf;
	if (create_dump_buffer(&buf, 2048) < 0)
		return;

	vgt_dump_fb_format(&buf, fb);
	printk("-----------FB format (VM-%d)--------\n", vmid);
	printk("%s", buf.buffer);
	destroy_dump_buffer(&buf);
}

u8 vgt_get_tiling_mode(struct drm_device *dev, u32 tiling)
{
        u8 tiling_mode = I915_TILING_NONE;

        if (IS_HASWELL(dev) || IS_BROADWELL(dev))
                tiling_mode = (tiling ? I915_TILING_X : I915_TILING_NONE);
        else if (IS_SKYLAKE(dev) || IS_KABYLAKE(dev)) {
                switch (tiling) {
                case PLANE_CTL_TILED_LINEAR:
                        tiling_mode = I915_TILING_NONE;
                        break;
                case PLANE_CTL_TILED_X:
                        tiling_mode = I915_TILING_X;
                        break;
                case PLANE_CTL_TILED_Y:
                        tiling_mode = I915_TILING_Y;
                        break;
                case PLANE_CTL_TILED_YF:
                        tiling_mode = I915_TILING_YF;
                        break;
                default:
                        DRM_DEBUG_DRIVER("skl: unsupported tile format:%x\n", tiling);
                }
        } else
                DRM_DEBUG_DRIVER("unsupported platform!\n");

        return tiling_mode;
}

/*
 * Decode framebuffer information from raw vMMIO
 *
 * INPUT:
 *   [domid] - specify the VM
 * OUTPUT:
 *   [format] - contain the decoded format info
 *
 * NOTE: The caller is expected to poll this interface, and reconstruct
 * previous reference to the new format information
 */

int vgt_decode_fb_format(int vmid, struct vgt_fb_format *fb)
{
	int i;
	struct vgt_device *vgt;
	struct pgt_device *pdev = &default_device;
	unsigned long flags;
	int cpu;
	int ret = 0;

	if (!fb)
		return -EINVAL;

	if (!IS_PRESKL(pdev) && !IS_SKL(pdev) && !IS_KBL(pdev)) {
		vgt_err("fb decode Supported until KBL\n");
		return -EINVAL;
	}

	/* TODO: use fine-grained refcnt later */
	vgt_lock_dev_flags(pdev, cpu, flags);

	vgt = vmid_2_vgt_device(vmid);
	if (!vgt) {
		vgt_err("Invalid domain ID (%d)\n", vmid);
		vgt_unlock_dev_flags(pdev, cpu, flags);
		return -ENODEV;
	}

	for (i = 0; i < MAX_INTEL_PIPES; i++) {
		struct vgt_pipe_format *pipe = &fb->pipes[i];
		vgt_reg_t ddi_func_ctl = __vreg(vgt, _VGT_TRANS_DDI_FUNC_CTL(i));

		if (!(ddi_func_ctl & TRANS_DDI_PORT_SHIFT)) {
			pipe->ddi_port = DDI_PORT_NONE;
		} else {
			vgt_reg_t port = (ddi_func_ctl & TRANS_DDI_PORT_MASK) >>
						TRANS_DDI_PORT_SHIFT;
			if (port <= DDI_PORT_E)
				pipe->ddi_port = port;
			else
				pipe->ddi_port = DDI_PORT_NONE;
		}

		ret |= vgt_decode_primary_plane_format(vgt, i, &pipe->primary);
		ret |= vgt_decode_sprite_plane_format(vgt, i, &pipe->sprite);
		ret |= vgt_decode_cursor_plane_format(vgt, i, &pipe->cursor);

		if (ret) {
			vgt_err("Decode format error for pipe(%d)\n", i);
			ret = -EINVAL;
			break;
		}
	}

	vgt_unlock_dev_flags(pdev, cpu, flags);

	if(vgt_debug & VGT_DBG_GENERIC)
	  vgt_show_fb_format(vmid, fb);
	return ret;
}

static ATOMIC_NOTIFIER_HEAD(vgt_fb_notifier_list);

int vgt_register_fb_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&vgt_fb_notifier_list, nb);
}

int vgt_unregister_fb_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&vgt_fb_notifier_list, nb);
}

int vgt_fb_notifier_call_chain(unsigned long val, void *data)
{
	return atomic_notifier_call_chain(&vgt_fb_notifier_list, val, data);
}

static int vgt_plane_to_i915_plane(unsigned vgt_plane)
{
	int ret = -ENOENT;
	switch (vgt_plane) {
		case PRIMARY_PLANE:
			ret = I915_VGT_PLANE_PRIMARY;
			break;
		case CURSOR_PLANE:
			ret = I915_VGT_PLANE_CURSOR;
			break;
		case SPRITE_PLANE:
			ret = I915_VGT_PLANE_SPRITE;
			break;
		default:
			vgt_err("invalid plane: %d\n", vgt_plane);
			break;
	}
	return (ret);
}

/*
 * A notifier API for userspace processes
 * By opening a netlink socket of id CN_IDX_VGT
 * userspace may get notifications of framebuffer events
 */
static int vgt_fb_event(struct notifier_block *nb,
			unsigned long val, void *data)
{
	int ret;
	static int seq = 0;
	struct fb_notify_msg *msg = data;
	struct cn_msg *m;
	int data_sz;
	struct vgt_device *vgt;

	/* Don't notify for dom0 */
	if (msg->vm_id == 0)
		return (0);

	vgt = vmid_2_vgt_device(msg->vm_id);
	if (!vgt)
		return (-EINVAL);

	data_sz = sizeof(*msg);
	m = kzalloc(sizeof(*m) + data_sz, GFP_ATOMIC);
	if (!m)
		return (-ENOMEM);

	m->id.idx = CN_IDX_VGT;
	m->id.val = msg->pipe_id;

	/*
	 * vgt plane ids are not exposed to userspace.
	 * Swap it out for drm's concept before sending it along.
	 * A plane without a mapping (MAX_PLANE) is not interesting, so
	 * drop it.
	 */
	msg->plane_id = vgt_plane_to_i915_plane(msg->plane_id);

	m->seq = seq++;
	m->len = data_sz;
	memcpy(m + 1, msg, data_sz);

	ret = cn_netlink_send(m, 0, CN_IDX_VGT, GFP_ATOMIC);

	kfree(m);
	return (ret);
}

static struct notifier_block vgt_fb_notifier = {
	.notifier_call = vgt_fb_event,
};

void vgt_init_fb_notify(void)
{
	vgt_register_fb_notifier(&vgt_fb_notifier);
}
