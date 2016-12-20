/*************************************************************************/ /*
 avb-mse

 Copyright (C) 2015-2016 Renesas Electronics Corporation

 License        Dual MIT/GPLv2

 The contents of this file are subject to the MIT license as set out below.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 Alternatively, the contents of this file may be used under the terms of
 the GNU General Public License Version 2 ("GPL") in which case the provisions
 of GPL are applicable instead of those above.

 If you wish to allow use of your version of this file only under the terms of
 GPL, and not to allow others to use your version of this file under the terms
 of the MIT license, indicate your decision by deleting the provisions above
 and replace them with the notice and other provisions required by GPL as set
 out in the file called "GPL-COPYING" included in this distribution. If you do
 not delete the provisions above, a recipient may use your version of this file
 under the terms of either the MIT license or GPL.

 This License is also included in this distribution in the file called
 "MIT-COPYING".

 EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
 PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
 COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


 GPLv2:
 If you wish to use this file under the terms of GPL, following terms are
 effective.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/ /*************************************************************************/

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME "/" fmt

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-memops.h>

#include "ravb_mse_kernel.h"

/****************/
/* Return value */
/****************/
#define MSE_ADAPTER_V4L2_RTN_OK				0
#define MSE_ADAPTER_V4L2_RTN_NG				-1

/*********************/
/* Number of devices */
/*********************/
#define MSE_ADAPTER_V4L2_DEVICE_MAX		MSE_INSTANCE_MAX
#define MSE_ADAPTER_V4L2_DEVICE_VIDEO_DEFAULT	2
#define MSE_ADAPTER_V4L2_DEVICE_MPEG2TS_DEFAULT	2

#define NUM_BUFFERS 2
#define NUM_PLANES  1

/*************/
/* Structure */
/*************/
/* Buffer information */
struct v4l2_adapter_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

/* Device information */
struct v4l2_adapter_device {
	struct v4l2_device	v4l2_dev;
	struct video_device	vdev;
	/* mutex lock */
	struct mutex		mutex_vb2;        /* lock for vb2_queue */
	struct v4l2_pix_format	format;
	struct v4l2_fract	frameintervals;
	struct vb2_queue	q_cap;
	struct vb2_queue	q_out;
	/* spin lock */
	spinlock_t		lock_buf_list;    /* lock for buf_list */
	struct list_head	buf_list;
	unsigned int		sequence;
	/* index for register */
	int			index_mse;
	enum MSE_TYPE		type;
	/* index for MSE instance */
	int			index_instance;
	bool                    f_opened;
	bool			f_mse_open;
};

/* Format information */
struct v4l2_adapter_fmt {
	u32			fourcc;
	unsigned int		min_width;
	unsigned int		max_width;
	unsigned int		step_width;
	unsigned int		min_height;
	unsigned int		max_height;
	unsigned int		step_height;
};

static const struct v4l2_fmtdesc g_formats_video[] = {
	{
		.description = "H264 with start codes",
		.pixelformat = V4L2_PIX_FMT_H264,
	},
	{
		.description = "H264 without start codes",
		.pixelformat = V4L2_PIX_FMT_H264_NO_SC,
	},
	{
		.description = "Motion-JPEG",
		.pixelformat = V4L2_PIX_FMT_MJPEG,
	},
};

static const struct v4l2_fmtdesc g_formats_mpeg[] = {
	{
		.description = "MPEG-1/2/4 Multiplexed",
		.pixelformat = V4L2_PIX_FMT_MPEG,
	},
};

/* Playback, capture video format sizes */
static const struct v4l2_adapter_fmt g_mse_adapter_v4l2_fmt_sizes_video[] = {
	/* limited H.264 picture size to range of
	 * R-Car Hardware video decoder (VCP4).
	 */
	{
		.fourcc		= V4L2_PIX_FMT_H264,
		.min_width	= 80,
		.max_width	= 3840,
		.step_width	= 2,
		.min_height	= 80,
		.max_height	= 2160,
		.step_height	= 2,
	},
	/* limited H.264 picture size to range of
	 * R-Car Hardware video decoder (VCP4).
	 */
	{
		.fourcc		= V4L2_PIX_FMT_H264_NO_SC,
		.min_width	= 80,
		.max_width	= 3840,
		.step_width	= 2,
		.min_height	= 80,
		.max_height	= 2160,
		.step_height	= 2,
	},
	/* limited MJPEG picture size to range of
	 * AVTP format(same as RTP), see RFC 2435 3.1.5,3.1.6
	 */
	{
		.fourcc		= V4L2_PIX_FMT_MJPEG,
		.min_width	= 8,
		.max_width	= 2040,
		.step_width	= 8,
		.min_height	= 8,
		.max_height	= 2040,
		.step_height	= 8,
	},
};

static const struct v4l2_adapter_fmt g_mse_adapter_v4l2_fmt_sizes_mpeg[] = {
	/* limited MPEG2-TS picture size to range of
	 * R-Car Hardware video decoder (VCP4).
	 */
	{
		.fourcc		= V4L2_PIX_FMT_MPEG,
		.min_width	= 80,
		.max_width	= 3840,
		.step_width	= 2,
		.min_height	= 80,
		.max_height	= 2160,
		.step_height	= 2,
	},
};

/*******************/
/* global variable */
/*******************/
static int v4l2_video_devices = MSE_ADAPTER_V4L2_DEVICE_VIDEO_DEFAULT;
module_param(v4l2_video_devices, int, 0440);
static int v4l2_mpeg2ts_devices = MSE_ADAPTER_V4L2_DEVICE_MPEG2TS_DEFAULT;
module_param(v4l2_mpeg2ts_devices, int, 0440);
static int v4l2_devices;

/************/
/* Function */
/************/
static inline struct v4l2_adapter_buffer *to_v4l2_adapter_buffer(
						struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct v4l2_adapter_buffer, vb);
}

static int try_mse_open(struct v4l2_adapter_device *vadp_dev,
			enum v4l2_buf_type i)
{
	bool tx = V4L2_TYPE_IS_OUTPUT(i);
	int index;

	if (vadp_dev->f_mse_open)
		return MSE_ADAPTER_V4L2_RTN_OK;

	/* probe is not finish yet */
	if (vadp_dev->index_mse == MSE_INDEX_UNDEFINED) {
		pr_info("[%s]probe is not finish yet\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_OK;
	}

	index = mse_open(vadp_dev->index_mse, tx);
	if (index < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	vadp_dev->index_instance = index;
	vadp_dev->f_mse_open = true;

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int try_mse_close(struct v4l2_adapter_device *vadp_dev)
{
	int err;

	if (!vadp_dev->f_mse_open)
		return MSE_ADAPTER_V4L2_RTN_OK;

	/* probe is not finish yet */
	if (vadp_dev->index_mse == MSE_INDEX_UNDEFINED) {
		pr_info("[%s]probe is not finish yet\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_OK;
	}

	if (vadp_dev->index_instance == MSE_INDEX_UNDEFINED) {
		pr_info("[%s]mse_start is not finish yet\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_OK;
	}

	err = mse_close(vadp_dev->index_instance);
	if (err < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	vadp_dev->f_mse_open = false;

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_fop_open(struct file *filp)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vadp_dev->f_opened) {
		pr_err("[%s] v4l2 device is opened \n", __func__);
		return -EPERM;
	}

	if (vadp_dev->f_mse_open) {
		pr_err("[%s] using mse device \n", __func__);
		return -EPERM;
	}

	err = v4l2_fh_open(filp);
	if (err) {
		pr_err("[%s]Failed v4l2_fh_open()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	vadp_dev->f_opened = true;
	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_fop_release(struct file *filp)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (!vadp_dev->f_opened) {
		pr_err("[%s] v4l2 device is not opened \n", __func__);
		return -EPERM;
	}

	if (vadp_dev->f_mse_open) {
		err = try_mse_close(vadp_dev);
		if (err) {
			pr_err("[%s]Failed mse_close()\n", __func__);
			return MSE_ADAPTER_V4L2_RTN_NG;
		}
	}

	err = v4l2_fh_release(filp);
	if (err) {
		pr_err("[%s]Failed v4l2_fh_release()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	vadp_dev->f_opened = false;
	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_querycap(struct file *filp,
				     void *priv,
				     struct v4l2_capability *vcap)
{
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	strlcpy(vcap->driver, "renesas-mse", sizeof(vcap->driver));
	strlcpy(vcap->card, vadp_dev->vdev.name, sizeof(vcap->card));
	snprintf(vcap->bus_info, sizeof(vcap->bus_info), "platform:%s",
		 vadp_dev->v4l2_dev.name);
	vcap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT |
			    V4L2_CAP_STREAMING;
	vcap->capabilities = vcap->device_caps | V4L2_CAP_DEVICE_CAPS;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_enum_fmt_vid_cap(struct file *filp,
					     void *priv,
					     struct v4l2_fmtdesc *fmt)
{
	unsigned int index;
	const struct v4l2_fmtdesc *fmtdesc;
	const struct v4l2_fmtdesc *fmtbase;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	int fmt_size;

	pr_debug("[%s]START fmt->index=%d\n", __func__, fmt->index);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vadp_dev->type == MSE_TYPE_ADAPTER_VIDEO) {
		fmtbase = g_formats_video;
		fmt_size = ARRAY_SIZE(g_formats_video);
	} else if (vadp_dev->type == MSE_TYPE_ADAPTER_MPEG2TS) {
		fmtbase = g_formats_mpeg;
		fmt_size = ARRAY_SIZE(g_formats_mpeg);
	} else {
		pr_err("[%s]Failed vdev type=%d\n", __func__, vadp_dev->type);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (fmt->index >= fmt_size) {
		pr_info("[%s]fmt->index(%d) is equal or bigger than %d\n",
			__func__, fmt->index, fmt_size);
		return -EINVAL;
	}

	index = fmt->index;
	memset(fmt, 0, sizeof(*fmt));

	fmtdesc = &fmtbase[index];

	fmt->index = index;
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	strcpy(fmt->description, fmtdesc->description);
	fmt->pixelformat = fmtdesc->pixelformat;

	pr_debug("[%s]END format: %s\n", __func__, fmtdesc->description);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_enum_fmt_vid_out(struct file *filp,
					     void *priv,
					     struct v4l2_fmtdesc *fmt)
{
	unsigned int index;
	const struct v4l2_fmtdesc *fmtdesc;
	const struct v4l2_fmtdesc *fmtbase;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	int fmt_size;

	pr_debug("[%s]START fmt->index=%d\n", __func__, fmt->index);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vadp_dev->type == MSE_TYPE_ADAPTER_VIDEO) {
		fmtbase = g_formats_video;
		fmt_size = ARRAY_SIZE(g_formats_video);
	} else if (vadp_dev->type == MSE_TYPE_ADAPTER_MPEG2TS) {
		fmtbase = g_formats_mpeg;
		fmt_size = ARRAY_SIZE(g_formats_mpeg);
	} else {
		pr_err("[%s]Failed vdev type=%d\n", __func__, vadp_dev->type);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (fmt->index >= fmt_size) {
		pr_info("[%s]fmt->index(%d) is equal or bigger than %d\n",
			__func__, fmt->index, fmt_size);
		return -EINVAL;
	}

	index = fmt->index;
	memset(fmt, 0, sizeof(*fmt));

	fmtdesc = &fmtbase[index];

	fmt->index = index;
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	strcpy(fmt->description, fmtdesc->description);
	fmt->pixelformat = fmtdesc->pixelformat;

	pr_debug("[%s]END format: %s\n", __func__, fmtdesc->description);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static const struct v4l2_fmtdesc *get_default_fmtdesc(
					const struct v4l2_fmtdesc fmtdescs[],
					int dflt_format)
{
	return &fmtdescs[dflt_format];
}

static const struct v4l2_fmtdesc *get_fmtdesc(
					const struct v4l2_fmtdesc *fmtdescs,
					int arr_size,
					struct v4l2_format *fmt)
{
	int i;
	const struct v4l2_fmtdesc *fmtdesc;
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	for (i = 0; i < arr_size; i++) {
		fmtdesc = &fmtdescs[i];
		if (fmtdesc->pixelformat == pix->pixelformat)
			return fmtdesc;
	}

	return NULL;
}

static void get_fmt_sizes(const struct v4l2_adapter_fmt *dflt_fmts,
			  int arr_size,
			  struct v4l2_format *fmt)
{
	int i;
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	for (i = 0; i < arr_size; i++) {
		if (pix->pixelformat != dflt_fmts[i].fourcc)
			continue;

		if (pix->width > dflt_fmts[i].max_width)
			pix->width = dflt_fmts[i].max_width;
		else if (pix->width < dflt_fmts[i].min_width)
			pix->width = dflt_fmts[i].min_width;
		else
			pix->width -= (pix->width % dflt_fmts[i].step_width);

		if (pix->height > dflt_fmts[i].max_height)
			pix->height = dflt_fmts[i].max_height;
		else if (pix->height < dflt_fmts[i].min_height)
			pix->height = dflt_fmts[i].min_height;
		else
			pix->height -= (pix->height % dflt_fmts[i].step_height);
		break;
	}
}

static int mse_adapter_v4l2_try_fmt_vid(struct file *filp,
					void *priv,
					struct v4l2_format *fmt)
{
	const struct v4l2_fmtdesc *fmtdesc;
	const struct v4l2_fmtdesc *fmtbase;
	const struct v4l2_adapter_fmt *vadp_fmt;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_pix_format *pix = &fmt->fmt.pix;
	struct video_device *vdev;
	int fmt_size, vadp_fmt_size;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}
	vdev = &vadp_dev->vdev;

	if (V4L2_TYPE_IS_OUTPUT(fmt->type))
		vdev->queue = &vadp_dev->q_out;
	else
		vdev->queue = &vadp_dev->q_cap;

	if (vadp_dev->type == MSE_TYPE_ADAPTER_VIDEO) {
		fmtbase = g_formats_video;
		fmt_size = ARRAY_SIZE(g_formats_video);
		vadp_fmt = g_mse_adapter_v4l2_fmt_sizes_video;
		vadp_fmt_size = ARRAY_SIZE(g_mse_adapter_v4l2_fmt_sizes_video);
	} else if (vadp_dev->type == MSE_TYPE_ADAPTER_MPEG2TS) {
		fmtbase = g_formats_mpeg;
		fmt_size = ARRAY_SIZE(g_formats_mpeg);
		vadp_fmt = g_mse_adapter_v4l2_fmt_sizes_mpeg;
		vadp_fmt_size = ARRAY_SIZE(g_mse_adapter_v4l2_fmt_sizes_mpeg);
	} else {
		pr_err("[%s]Failed vdev type=%d\n", __func__, vadp_dev->type);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	fmtdesc = get_fmtdesc(fmtbase, fmt_size, fmt);
	if (!fmtdesc) {
		pr_info("[%s]Unknown fourcc format=(0x%08x)\n",
			__func__, pix->pixelformat);
		fmtdesc = get_default_fmtdesc(fmtbase, 0);
		pix->pixelformat = fmtdesc->pixelformat;
	}

	if (pix->field != V4L2_FIELD_NONE &&
	    pix->field != V4L2_FIELD_INTERLACED)
		pix->field = V4L2_FIELD_NONE;

	get_fmt_sizes(vadp_fmt, vadp_fmt_size, fmt);

	pix->bytesperline = pix->width * 2;
	pix->sizeimage = pix->bytesperline * pix->height;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_g_fmt_vid_cap(struct file *filp,
					  void *priv,
					  struct v4l2_format *fmt)
{
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	*pix = vadp_dev->format;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_g_fmt_vid_out(struct file *filp,
					  void *priv,
					  struct v4l2_format *fmt)
{
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	*pix = vadp_dev->format;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_s_fmt_vid_cap(struct file *filp,
					  void *priv,
					  struct v4l2_format *fmt)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (V4L2_TYPE_IS_OUTPUT(fmt->type)) {
		pr_err("[%s]Failed wrong buffer type\n", __func__);
		return -EINVAL;
	}

	err = mse_adapter_v4l2_try_fmt_vid(filp, priv, fmt);
	if (err) {
		pr_err("[%s]Failed capture_try_fmt_vid_cap()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vb2_is_busy(&vadp_dev->q_cap)) {
		pr_err("[%s]Failed vb2 is busy\n", __func__);
		return -EBUSY;
	}

	vadp_dev->format = *pix;

	pr_info("[%s]END format=%c%c%c%c, width=%d, height=%d\n",
		__func__,
		pix->pixelformat >> 0,
		pix->pixelformat >> 8,
		pix->pixelformat >> 16,
		pix->pixelformat >> 24,
		pix->width,
		pix->height);

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_s_fmt_vid_out(struct file *filp,
					  void *priv,
					  struct v4l2_format *fmt)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_pix_format *pix = &fmt->fmt.pix;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (!V4L2_TYPE_IS_OUTPUT(fmt->type)) {
		pr_err("[%s]Failed wrong buffer type\n", __func__);
		return -EINVAL;
	}

	err = mse_adapter_v4l2_try_fmt_vid(filp, priv, fmt);
	if (err) {
		pr_err("[%s]Failed playback_try_fmt_vid_out()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vb2_is_busy(&vadp_dev->q_out)) {
		pr_err("[%s]Failed vb2 is busy\n", __func__);
		return -EBUSY;
	}

	vadp_dev->format = *pix;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_streamon(struct file *filp,
				     void *priv,
				     enum v4l2_buf_type i)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct mse_video_config config;
	struct mse_mpeg2ts_config config_ts;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	err = try_mse_open(vadp_dev, i);
	if (err) {
		pr_err("[%s]Failed mse_open()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vadp_dev->format.pixelformat == V4L2_PIX_FMT_MPEG) {
		err = mse_get_mpeg2ts_config(vadp_dev->index_instance,
					     &config_ts);
		if (err < 0) {
			pr_err("[%s]Failed mse_get_mpeg2ts_config()\n",
			       __func__);
			return MSE_ADAPTER_V4L2_RTN_NG;
		}

		/* nothing to set at current version */

		err = mse_set_mpeg2ts_config(vadp_dev->index_instance,
					     &config_ts);
		if (err < 0) {
			pr_err("[%s]Failed mse_set_mpeg2ts_config()\n",
			       __func__);
			return MSE_ADAPTER_V4L2_RTN_NG;
		}
	} else {
		/* video config */
		err = mse_get_video_config(vadp_dev->index_instance, &config);
		if (err < MSE_ADAPTER_V4L2_RTN_OK) {
			pr_err("[%s]Failed mse_get_video_config()\n",
			       __func__);
			return MSE_ADAPTER_V4L2_RTN_NG;
		}

		switch (vadp_dev->format.pixelformat) {
		case V4L2_PIX_FMT_H264:
			config.format = MSE_VIDEO_FORMAT_H264_BYTE_STREAM;
			break;
		case V4L2_PIX_FMT_H264_NO_SC:
			config.format = MSE_VIDEO_FORMAT_H264_AVC;
			break;
		case V4L2_PIX_FMT_MJPEG:
			config.format = MSE_VIDEO_FORMAT_MJPEG;
			break;
		default:
			pr_err("[%s] invalid format=%c%c%c%c\n", __func__,
			       vadp_dev->format.pixelformat >> 0,
			       vadp_dev->format.pixelformat >> 8,
			       vadp_dev->format.pixelformat >> 16,
			       vadp_dev->format.pixelformat >> 24);
			return -EINVAL;
		}

		if (vadp_dev->frameintervals.numerator > 0 &&
		    vadp_dev->frameintervals.denominator > 0) {
			config.fps.n = vadp_dev->frameintervals.numerator;
			config.fps.m = vadp_dev->frameintervals.denominator;
		}

		err = mse_set_video_config(vadp_dev->index_instance, &config);
		if (err < MSE_ADAPTER_V4L2_RTN_OK) {
			pr_err("[%s]Failed mse_set_video_config()\n",
			       __func__);
			return MSE_ADAPTER_V4L2_RTN_NG;
		}
	}

	pr_debug("[%s]END\n", __func__);
	return vb2_ioctl_streamon(filp, priv, i);
}

static int mse_adapter_v4l2_g_parm(struct file *filp,
				   void *priv,
				   struct v4l2_streamparm *sp)
{
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_fract *fract;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (V4L2_TYPE_IS_OUTPUT(sp->type)) {
		fract = &sp->parm.output.timeperframe;
		sp->parm.output.outputmode = 0;
	} else {
		fract = &sp->parm.capture.timeperframe;
		sp->parm.capture.capturemode = 0;
		sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	}

	*fract = vadp_dev->frameintervals;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_s_parm(struct file *filp,
				   void *priv,
				   struct v4l2_streamparm *sp)
{
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	struct v4l2_fract *fract;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (V4L2_TYPE_IS_OUTPUT(sp->type))
		fract = &sp->parm.output.timeperframe;
	else
		fract = &sp->parm.capture.timeperframe;

	vadp_dev->frameintervals = *fract;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_enum_framesizes(
					struct file *filp,
					void *priv,
					struct v4l2_frmsizeenum *fsize)
{
	const struct v4l2_adapter_fmt *vadp_fmt = NULL;
	const struct v4l2_adapter_fmt *vadp_fmtbase;
	struct v4l2_adapter_device *vadp_dev = video_drvdata(filp);
	int i, vadp_fmt_size;

	pr_debug("[%s]START fsize->index=%d\n", __func__, fsize->index);

	if (!vadp_dev) {
		pr_err("[%s]Failed video_drvdata()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (vadp_dev->type == MSE_TYPE_ADAPTER_VIDEO) {
		vadp_fmtbase = g_mse_adapter_v4l2_fmt_sizes_video;
		vadp_fmt_size = ARRAY_SIZE(g_mse_adapter_v4l2_fmt_sizes_video);
	} else if (vadp_dev->type == MSE_TYPE_ADAPTER_MPEG2TS) {
		vadp_fmtbase = g_mse_adapter_v4l2_fmt_sizes_mpeg;
		vadp_fmt_size = ARRAY_SIZE(g_mse_adapter_v4l2_fmt_sizes_mpeg);
	} else {
		pr_err("[%s]Failed vdev type=%d\n", __func__, vadp_dev->type);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	/* get frame sizes */
	for (i = 0; i < vadp_fmt_size; i++) {
		if (vadp_fmtbase[i].fourcc == fsize->pixel_format) {
			vadp_fmt = &vadp_fmtbase[i];
			break;
		}
	}

	if (fsize->index > 0 || !vadp_fmt) {
		pr_info("[%s]fsize->index(%d)\n", __func__, fsize->index);
		return -EINVAL;
	}

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = vadp_fmt->min_width;
	fsize->stepwise.max_width = vadp_fmt->max_width;
	fsize->stepwise.step_width = vadp_fmt->step_width;
	fsize->stepwise.min_height = vadp_fmt->min_height;
	fsize->stepwise.max_height = vadp_fmt->max_height;
	fsize->stepwise.step_height = vadp_fmt->step_height;

	pr_debug("[%s]END\n  format=%c%c%c%c, min_width=%d, min_height=%d\n"
		 "  max_width=%d, max_height=%d\n",
		 __func__,
		 vadp_fmt->fourcc >> 0,
		 vadp_fmt->fourcc >> 8,
		 vadp_fmt->fourcc >> 16,
		 vadp_fmt->fourcc >> 24,
		 fsize->stepwise.min_width,
		 fsize->stepwise.min_height,
		 fsize->stepwise.max_width,
		 fsize->stepwise.max_height);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_playback_callback(void *priv, int size);
static int mse_adapter_v4l2_capture_callback(void *priv, int size);

#if KERNEL_VERSION(4, 7, 0) <= LINUX_VERSION_CODE
static int mse_adapter_v4l2_queue_setup(struct vb2_queue *vq,
					unsigned int *nbuffers,
					unsigned int *nplanes,
					unsigned int sizes[],
					struct device *alloc_devs[])
#else
static int mse_adapter_v4l2_queue_setup(struct vb2_queue *vq,
					unsigned int *nbuffers,
					unsigned int *nplanes,
					unsigned int sizes[],
					void *alloc_ctxs[])
#endif
{
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vq);

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	pr_debug("[%s]vq->num_buffers=%d, nbuffers=%d",
		 __func__, vq->num_buffers, *nbuffers);
	if (vq->num_buffers + *nbuffers < NUM_BUFFERS)
		*nbuffers = NUM_BUFFERS - vq->num_buffers;

	if (*nplanes && sizes[0] < vadp_dev->format.sizeimage) {
		pr_err("[%s]sizeimage too small (%d < %d)\n",
		       __func__, sizes[0], vadp_dev->format.sizeimage);
		return -EINVAL;
	}

	if (!*nplanes)
		sizes[0] = vadp_dev->format.sizeimage;
	*nplanes = NUM_PLANES;

	pr_debug("[%s]END nbuffers=%d\n", __func__, *nbuffers);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_buf_prepare(struct vb2_buffer *vb)
{
	unsigned long plane_size;
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vb->vb2_queue);

	pr_debug("[%s]START vb=%p\n", __func__, vb2_plane_vaddr(vb, 0));

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	plane_size = vb2_plane_size(&vbuf->vb2_buf, 0);
	if (plane_size < vadp_dev->format.sizeimage) {
		pr_err("[%s]buffer too small (%lu < %u)\n",
		       __func__, plane_size, vadp_dev->format.sizeimage);
		return -EINVAL;
	}

	vbuf->vb2_buf.planes[0].bytesused =
				vb2_get_plane_payload(&vbuf->vb2_buf, 0);

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static void return_all_buffers(struct v4l2_adapter_device *vadp_dev,
			       enum vb2_buffer_state state)
{
	unsigned long flags;
	struct v4l2_adapter_buffer *buf, *node;

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	list_for_each_entry_safe(buf, node, &vadp_dev->buf_list, list) {
		vb2_buffer_done(&buf->vb.vb2_buf, state);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);
}

static void mse_adapter_v4l2_stop_streaming(struct vb2_queue *vq)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vq);

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return;
	}

	return_all_buffers(vadp_dev, VB2_BUF_STATE_ERROR);

	err = mse_stop_streaming(vadp_dev->index_instance);
	if (err < MSE_ADAPTER_V4L2_RTN_OK) {
		pr_err("[%s]Failed mse_stop_streaming()\n", __func__);
		return;
	}

	pr_debug("[%s]END\n", __func__);
}

static int playback_send_first_buffer(struct v4l2_adapter_device *vadp_dev)
{
	struct v4l2_adapter_buffer *new_buf = NULL;
	void *buf_to_send;
	unsigned long flags;
	long new_buf_size;
	int err;

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	if (!list_empty(&vadp_dev->buf_list)) {
		new_buf = list_first_entry(&vadp_dev->buf_list,
					   struct v4l2_adapter_buffer,
					   list);
	}
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);

	if (!new_buf) {
		pr_debug("[%s]new_buf is NULL\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_OK;
	}

	buf_to_send = vb2_plane_vaddr(&new_buf->vb.vb2_buf, 0);
	pr_debug("[%s]buf_to_send=%p\n", __func__, buf_to_send);

	new_buf_size = vb2_get_plane_payload(&new_buf->vb.vb2_buf, 0);
	new_buf->vb.vb2_buf.timestamp = ktime_get_ns();
	new_buf->vb.sequence = vadp_dev->sequence++;
	new_buf->vb.field = vadp_dev->format.field;

	err = mse_start_transmission(vadp_dev->index_instance,
				     buf_to_send,
				     new_buf_size,
				     vadp_dev,
				     mse_adapter_v4l2_playback_callback);
	if (err < MSE_ADAPTER_V4L2_RTN_OK) {
		pr_err("[%s]Failed mse_start_transmission()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_playback_callback(void *priv, int size)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = priv;
	unsigned long flags;
	struct v4l2_adapter_buffer *buf = NULL;

	if (!vadp_dev) {
		pr_err("[%s]Private data is NULL\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	pr_debug("[%s]START\n", __func__);

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	if (!list_empty(&vadp_dev->buf_list)) {
		buf = list_first_entry(&vadp_dev->buf_list,
				       struct v4l2_adapter_buffer,
				       list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);

	if (!buf) {
		pr_debug("[%s]buf is NULL\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_OK;
	}

	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	err = playback_send_first_buffer(vadp_dev);
	if (err < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static void mse_adapter_v4l2_playback_buf_queue(struct vb2_buffer *vb)
{
	unsigned long flags;
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct v4l2_adapter_buffer *buf = to_v4l2_adapter_buffer(vbuf);
	int is_need_send = 0;

	pr_debug("[%s]START vb=%p\n", __func__, vb2_plane_vaddr(vb, 0));

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return;
	}

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	if (list_empty(&vadp_dev->buf_list))
		is_need_send = 1;
	list_add_tail(&buf->list, &vadp_dev->buf_list);
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);

	/* start_streaming is not called yet */
	if (!vb2_start_streaming_called(&vadp_dev->q_out)) {
		pr_debug("[%s]start_streaming is not called yet\n", __func__);
		return;
	}
	/* no need to send anything */
	if (!is_need_send) {
		pr_debug("[%s]no need to send anything\n", __func__);
		return;
	}
	playback_send_first_buffer(vadp_dev);

	pr_debug("[%s]END\n", __func__);
}

static int playback_start_streaming(struct v4l2_adapter_device *vadp_dev,
				    unsigned int count)
{
	int err;
	int index = vadp_dev->index_instance;

	vadp_dev->sequence = 0;

	err = mse_start_streaming(index);
	if (err < MSE_ADAPTER_V4L2_RTN_OK) {
		pr_err("[%s]Failed mse_start_streaming()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	err = playback_send_first_buffer(vadp_dev);
	if (err < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_playback_start_streaming(struct vb2_queue *vq,
						     unsigned int count)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vq);

	pr_debug("[%s]START count=%d\n", __func__, count);

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	err = playback_start_streaming(vadp_dev, count);
	if (err) {
		pr_err("[%s]Failed start streaming\n", __func__);
		return_all_buffers(vadp_dev, VB2_BUF_STATE_QUEUED);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int capture_send_first_buffer(struct v4l2_adapter_device *vadp_dev)
{
	struct v4l2_adapter_buffer *new_buf = NULL;
	void *buf_to_send;
	unsigned long flags;
	long new_buf_size;
	int err;

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	if (!list_empty(&vadp_dev->buf_list))
		new_buf = list_first_entry(&vadp_dev->buf_list,
					   struct v4l2_adapter_buffer,
					   list);
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);

	if (!new_buf) {
		pr_debug("[%s]new_buf is NULL\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_OK;
	}

	buf_to_send = vb2_plane_vaddr(&new_buf->vb.vb2_buf, 0);
	pr_debug("[%s]buf_to_send=%p\n", __func__, buf_to_send);
	new_buf_size = vb2_plane_size(&new_buf->vb.vb2_buf, 0);

	err = mse_start_transmission(vadp_dev->index_instance,
				     buf_to_send,
				     new_buf_size,
				     vadp_dev,
				     mse_adapter_v4l2_capture_callback);
	if (err < MSE_ADAPTER_V4L2_RTN_OK) {
		pr_err("[%s]Failed mse_start_transmission()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_capture_callback(void *priv, int size)
{
	int err;
	unsigned long flags;
	struct v4l2_adapter_buffer *buf = NULL;
	struct v4l2_adapter_device *vadp_dev = priv;
	enum vb2_buffer_state buf_state = VB2_BUF_STATE_DONE;

	if (!vadp_dev) {
		pr_err("[%s]Private data is NULL\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	pr_debug("[%s]START size=%d\n", __func__, size);

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	if (!list_empty(&vadp_dev->buf_list)) {
		buf = list_first_entry(&vadp_dev->buf_list,
				       struct v4l2_adapter_buffer,
				       list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);

	if (!buf) {
		pr_debug("[%s]buf is NULL\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	if (size == 0)
		buf_state = VB2_BUF_STATE_ERROR;

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, size);
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence = vadp_dev->sequence++;
	buf->vb.field = vadp_dev->format.field;
	vb2_buffer_done(&buf->vb.vb2_buf, buf_state);

	err = capture_send_first_buffer(vadp_dev);
	if (err < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	pr_debug("[%s]END\n", __func__);
	return MSE_ADAPTER_V4L2_RTN_OK;
}

static void mse_adapter_v4l2_capture_buf_queue(struct vb2_buffer *vb)
{
	unsigned long flags;
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct v4l2_adapter_buffer *buf = to_v4l2_adapter_buffer(vbuf);
	int is_need_send = 0;

	pr_debug("[%s]START\n", __func__);

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return;
	}

	spin_lock_irqsave(&vadp_dev->lock_buf_list, flags);
	if (list_empty(&vadp_dev->buf_list))
		is_need_send = 1;
	list_add_tail(&buf->list, &vadp_dev->buf_list);
	spin_unlock_irqrestore(&vadp_dev->lock_buf_list, flags);

	/* start_streaming is not called yet */
	if (!vb2_start_streaming_called(&vadp_dev->q_cap)) {
		pr_debug("[%s]start_streaming is not called yet\n", __func__);
		return;
	}
	/* no need to send anything */
	if (!is_need_send) {
		pr_debug("[%s]no need to send anything\n", __func__);
		return;
	}
	capture_send_first_buffer(vadp_dev);

	pr_debug("[%s]END\n", __func__);
}

static int capture_start_streaming(struct v4l2_adapter_device *vadp_dev,
				   unsigned int count)
{
	int err;
	int index = vadp_dev->index_instance;

	vadp_dev->sequence = 0;

	err = mse_start_streaming(index);
	if (err < MSE_ADAPTER_V4L2_RTN_OK) {
		pr_err("[%s]Failed mse_start_streaming()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	err = capture_send_first_buffer(vadp_dev);
	if (err < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int mse_adapter_v4l2_capture_start_streaming(struct vb2_queue *vq,
						    unsigned int count)
{
	int err;
	struct v4l2_adapter_device *vadp_dev = vb2_get_drv_priv(vq);

	pr_debug("[%s]START count=%d\n", __func__, count);

	if (!vadp_dev) {
		pr_err("[%s]Failed vb2_get_drv_priv()\n", __func__);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	err = capture_start_streaming(vadp_dev, count);
	if (err) {
		pr_err("[%s]Failed start streaming\n", __func__);
		return_all_buffers(vadp_dev, VB2_BUF_STATE_QUEUED);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static const struct vb2_ops g_mse_adapter_v4l2_capture_queue_ops = {
	.queue_setup		= mse_adapter_v4l2_queue_setup,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
	.buf_prepare		= mse_adapter_v4l2_buf_prepare,
	.start_streaming	= mse_adapter_v4l2_capture_start_streaming,
	.stop_streaming		= mse_adapter_v4l2_stop_streaming,
	.buf_queue		= mse_adapter_v4l2_capture_buf_queue,
};

static const struct vb2_ops g_mse_adapter_v4l2_playback_queue_ops = {
	.queue_setup		= mse_adapter_v4l2_queue_setup,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
	.buf_prepare		= mse_adapter_v4l2_buf_prepare,
	.start_streaming	= mse_adapter_v4l2_playback_start_streaming,
	.stop_streaming		= mse_adapter_v4l2_stop_streaming,
	.buf_queue		= mse_adapter_v4l2_playback_buf_queue,
};

static const struct v4l2_ioctl_ops g_mse_adapter_v4l2_ioctl_ops = {
	.vidioc_querycap		= mse_adapter_v4l2_querycap,
	.vidioc_enum_fmt_vid_cap	= mse_adapter_v4l2_enum_fmt_vid_cap,
	.vidioc_enum_fmt_vid_out	= mse_adapter_v4l2_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_cap		= mse_adapter_v4l2_g_fmt_vid_cap,
	.vidioc_g_fmt_vid_out		= mse_adapter_v4l2_g_fmt_vid_out,
	.vidioc_s_fmt_vid_cap		= mse_adapter_v4l2_s_fmt_vid_cap,
	.vidioc_s_fmt_vid_out		= mse_adapter_v4l2_s_fmt_vid_out,
	.vidioc_try_fmt_vid_cap		= mse_adapter_v4l2_try_fmt_vid,
	.vidioc_try_fmt_vid_out		= mse_adapter_v4l2_try_fmt_vid,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_streamon		= mse_adapter_v4l2_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_g_parm			= mse_adapter_v4l2_g_parm,
	.vidioc_s_parm			= mse_adapter_v4l2_s_parm,
	.vidioc_enum_framesizes		= mse_adapter_v4l2_enum_framesizes
,
};

static struct v4l2_file_operations g_mse_adapter_v4l2_fops = {
	.owner		= THIS_MODULE,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= vb2_fop_mmap,
	.open		= mse_adapter_v4l2_fop_open,
	.release	= mse_adapter_v4l2_fop_release,
};

static int register_mse_core(struct v4l2_adapter_device *vadp_dev,
			     enum MSE_TYPE type)
{
	int index_mse;
	struct video_device *vdev = &vadp_dev->vdev;
	char device_name[MSE_NAME_LEN_MAX];

	sprintf(device_name, "/dev/%s", video_device_node_name(vdev));

	index_mse = mse_register_adapter_media(type,
					       vdev->name,
					       device_name);
	if (index_mse < MSE_ADAPTER_V4L2_RTN_OK)
		return MSE_ADAPTER_V4L2_RTN_NG;

	vadp_dev->index_mse = index_mse;

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static struct v4l2_adapter_device *g_v4l2_adapter;

static int mse_adapter_v4l2_probe(int dev_num, enum MSE_TYPE type)
{
	int err;
	struct v4l2_adapter_device *vadp_dev;
	struct video_device *vdev;
	struct v4l2_device *v4l2_dev;
	struct vb2_queue *q;

	pr_debug("[%s]START device number=%d\n", __func__, dev_num);

	vadp_dev = &g_v4l2_adapter[dev_num];
	vadp_dev->index_mse = MSE_INDEX_UNDEFINED;
	vadp_dev->type = type;
	vadp_dev->index_instance = MSE_INDEX_UNDEFINED;

	vdev = &vadp_dev->vdev;
	snprintf(vdev->name, sizeof(vdev->name),
		 "Renesas MSE Adapter %d", dev_num);
	vdev->release = video_device_release_empty;
	vdev->fops = &g_mse_adapter_v4l2_fops;
	vdev->vfl_type = VFL_TYPE_GRABBER;
	vdev->ioctl_ops = &g_mse_adapter_v4l2_ioctl_ops;
	vdev->vfl_dir = VFL_DIR_M2M;

	mutex_init(&vadp_dev->mutex_vb2);

	q = &vadp_dev->q_cap;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP;
	q->drv_priv = vadp_dev;
	q->buf_struct_size = sizeof(struct v4l2_adapter_buffer);
	q->ops = &g_mse_adapter_v4l2_capture_queue_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &vadp_dev->mutex_vb2;
	q->gfp_flags = GFP_DMA32;
	q->min_buffers_needed = 2;

	err = vb2_queue_init(q);
	if (err) {
		pr_err("[%s]Failed vb2_queue_init() Rtn=%d\n", __func__, err);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	q = &vadp_dev->q_out;
	q->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	q->io_modes = VB2_MMAP;
	q->drv_priv = vadp_dev;
	q->buf_struct_size = sizeof(struct v4l2_adapter_buffer);
	q->ops = &g_mse_adapter_v4l2_playback_queue_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &vadp_dev->mutex_vb2;
	q->gfp_flags = GFP_DMA32;
	q->min_buffers_needed = 2;

	err = vb2_queue_init(q);
	if (err) {
		pr_err("[%s]Failed vb2_queue_init() Rtn=%d\n", __func__, err);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	INIT_LIST_HEAD(&vadp_dev->buf_list);
	spin_lock_init(&vadp_dev->lock_buf_list);

	vdev->lock = &vadp_dev->mutex_vb2;

	video_set_drvdata(vdev, vadp_dev);

	v4l2_dev = &vadp_dev->v4l2_dev;
	snprintf(v4l2_dev->name, sizeof(v4l2_dev->name),
		 "Renesas MSE Device %d", dev_num);
	err = v4l2_device_register(NULL, v4l2_dev);
	if (err) {
		pr_err("[%s]Failed v4l2_device_register() Rtn=%d\n",
		       __func__, err);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	vdev->v4l2_dev = v4l2_dev;
	err = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
	if (err) {
		pr_err("[%s]Failed video_register_device() Rtn=%d\n",
		       __func__, err);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	pr_debug("[%s]video device was registered as (%s)",
		 __func__, video_device_node_name(vdev));

	err = register_mse_core(vadp_dev, type);
	if (err < MSE_ADAPTER_V4L2_RTN_OK) {
		pr_err("[%s]Failed register_mse_core() Rtn=%d\n",
		       __func__, err);
		return MSE_ADAPTER_V4L2_RTN_NG;
	}

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static void mse_adapter_v4l2_cleanup(struct v4l2_adapter_device *vadp_dev)
{
	v4l2_device_unregister(&vadp_dev->v4l2_dev);
	video_unregister_device(&vadp_dev->vdev);
}

static void unregister_mse_core(struct v4l2_adapter_device *vadp_dev)
{
	int err;
	int index = vadp_dev->index_mse;

	if (index == MSE_INDEX_UNDEFINED) {
		pr_info("[%s]already unregistered(%d)\n", __func__, index);
		return;
	}

	err = mse_unregister_adapter_media(index);
	if (err < MSE_ADAPTER_V4L2_RTN_OK)
		pr_err("[%s]Failed mse_unregister_adapter_media()\n",
		       __func__);
}

static int mse_adapter_v4l2_free(int dev_num)
{
	pr_debug("[%s]START device number=%d\n", __func__, dev_num);

	unregister_mse_core(&g_v4l2_adapter[dev_num]);
	mse_adapter_v4l2_cleanup(&g_v4l2_adapter[dev_num]);

	pr_debug("[%s]END\n", __func__);

	return MSE_ADAPTER_V4L2_RTN_OK;
}

static int __init mse_adapter_v4l2_init(void)
{
	int err, i, type;

	pr_debug("Start v4l2 adapter\n");

	if (v4l2_video_devices < 0) {
		pr_err("[%s] Invalid devices video=%d\n",
			__func__, v4l2_video_devices);
		return -EINVAL;
	}

	if (v4l2_mpeg2ts_devices < 0) {
		pr_err("[%s] Invalid devices mpeg2ts=%d\n",
			__func__, v4l2_video_devices);
		return -EINVAL;
	}

	v4l2_devices = v4l2_video_devices + v4l2_mpeg2ts_devices;
	if (v4l2_devices > MSE_ADAPTER_V4L2_DEVICE_MAX) {
		pr_err("[%s] Too many devices, %d (video=%d mpeg2ts=%d)\n",
		       __func__, v4l2_devices, v4l2_video_devices,
		       v4l2_mpeg2ts_devices);
		return -EINVAL;
	} else if (v4l2_devices <= 0) {
		pr_err("[%s] Invalid devices, %d (video=%d mpeg2ts=%d)\n",
		       __func__, v4l2_devices, v4l2_video_devices,
		       v4l2_mpeg2ts_devices);
		return -EINVAL;
	} else {
		;
	}

	g_v4l2_adapter = kcalloc(v4l2_devices, sizeof(*g_v4l2_adapter),
				 GFP_KERNEL);
	if (!g_v4l2_adapter)
		return MSE_ADAPTER_V4L2_RTN_NG;

	for (i = 0; i < v4l2_devices; i++) {
		if (i < v4l2_video_devices)
			type = MSE_TYPE_ADAPTER_VIDEO;
		else
			type = MSE_TYPE_ADAPTER_MPEG2TS;

		err = mse_adapter_v4l2_probe(i, type);
		if (err) {
			pr_err("Failed creating device=%d Rtn=%d\n", i, err);
			goto init_fail;
		}
	}

	return MSE_ADAPTER_V4L2_RTN_OK;

init_fail:
	for (i = 0; i < v4l2_devices; i++)
		mse_adapter_v4l2_free(i);

	kfree(g_v4l2_adapter);

	return MSE_ADAPTER_V4L2_RTN_NG;
}

/* module clean up */
static void __exit mse_adapter_v4l2_exit(void)
{
	int i;

	pr_debug("Stop v4l2 adapter\n");

	for (i = 0; i < v4l2_devices; i++)
		mse_adapter_v4l2_free(i);

	kfree(g_v4l2_adapter);
}

module_init(mse_adapter_v4l2_init)
module_exit(mse_adapter_v4l2_exit)

MODULE_AUTHOR("Jose Luis HIRANO");
MODULE_DESCRIPTION("Renesas Media Streaming Engine");
MODULE_LICENSE("Dual MIT/GPL");
