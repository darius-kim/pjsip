/* $Id: converter_libswscale.c 4076 2012-04-24 09:40:35Z bennylp $ */
/*
 * Copyright (C) 2010-2011 Teluu Inc. (http://www.teluu.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/*
 * 2014, march, 3
 * darius <dooitnow@gmail.com>
 *
 * convert_libyuv.c
 *
 * this code derived from converter_libswscale.c
 * you can use freely this code under pjsip and libyuv licences
 */

#include <pjmedia/converter.h>
#include <pj/errno.h>

#if PJMEDIA_HAS_LIBYUV

#include <libyuv.h>

#define CSP_BUFFER_SIZE		4
static pj_status_t factory_create_converter(pjmedia_converter_factory *cf,
					    pj_pool_t *pool,
					    const pjmedia_conversion_param*prm,
					    pjmedia_converter **p_cv);
static void factory_destroy_factory(pjmedia_converter_factory *cf);
static pj_status_t libyuv_conv_convert(pjmedia_converter *converter,
					   pjmedia_frame *src_frame,
					   pjmedia_frame *dst_frame);
static void libyuv_conv_destroy(pjmedia_converter *converter);


struct fmt_info
{
    const pjmedia_video_format_info 	*fmt_info;
    pjmedia_video_apply_fmt_param 	 apply_param;
};

struct libyuv_converter
{
    pjmedia_converter 	base;
    struct fmt_info		src;
	struct fmt_info		dst;

	uint8 *dstBuf[CSP_BUFFER_SIZE];
	uint8 *srcBuf;

	pj_size_t dstBufSize[CSP_BUFFER_SIZE];
	pj_size_t srcBufSize;
};

static pjmedia_converter_factory_op libyuv_factory_op =
{
    &factory_create_converter,
    &factory_destroy_factory
};

static pjmedia_converter_op libyuv_converter_op =
{
    &libyuv_conv_convert,
    &libyuv_conv_destroy
};

uint8* align_alloc(pj_size_t size, pj_size_t align)
{
#ifdef _MSC_VER
	return _aligned_malloc(size, align);
#else
	uint8 *memptr;
	int result = posix_memalign(&memptr, align, size);
	if (result == 0)
		return memptr;
	else
		return NULL;
#endif
}

void align_free(void *memblock)
{
#ifdef _MSC_VER
	_aligned_free(memblock);
#else
	free(memblock);
#endif
}

static pj_status_t factory_create_converter(pjmedia_converter_factory *cf,
					    pj_pool_t *pool,
					    const pjmedia_conversion_param *prm,
					    pjmedia_converter **p_cv)
{
    const pjmedia_video_format_detail *src_detail, *dst_detail;
    const pjmedia_video_format_info *src_fmt_info, *dst_fmt_info;
    struct libyuv_converter *fcv;
	pj_size_t dstStride;
	pj_size_t dstSize;

    PJ_UNUSED_ARG(cf);

    /* Only supports video */
    if (prm->src.type != PJMEDIA_TYPE_VIDEO ||
	prm->dst.type != prm->src.type ||
	prm->src.detail_type != PJMEDIA_FORMAT_DETAIL_VIDEO ||
	prm->dst.detail_type != prm->src.detail_type)
    {
	return PJ_ENOTSUP;
    }

    /* lookup source format info */
    src_fmt_info = pjmedia_get_video_format_info(
		      pjmedia_video_format_mgr_instance(),
		      prm->src.id);
    if (!src_fmt_info)
	return PJ_ENOTSUP;

    /* lookup destination format info */
    dst_fmt_info = pjmedia_get_video_format_info(
		      pjmedia_video_format_mgr_instance(),
		      prm->dst.id);
    if (!dst_fmt_info)
	return PJ_ENOTSUP;

    src_detail = pjmedia_format_get_video_format_detail(&prm->src, PJ_TRUE);
    dst_detail = pjmedia_format_get_video_format_detail(&prm->dst, PJ_TRUE);

	if (dst_fmt_info->id != FOURCC_I420)
		return PJ_ENOTSUP;

    fcv = PJ_POOL_ZALLOC_T(pool, struct libyuv_converter);
    fcv->base.op = &libyuv_converter_op;
    fcv->src.apply_param.size = src_detail->size;
    fcv->src.fmt_info = src_fmt_info;
    fcv->dst.apply_param.size = dst_detail->size;
    fcv->dst.fmt_info = dst_fmt_info;

	memset(fcv->dstBuf, 0x00, sizeof(uint8*) * CSP_BUFFER_SIZE);

	fcv->srcBuf = 0x00;

	fcv->srcBufSize = src_detail->size.w * src_detail->size.h * src_fmt_info->bpp / 8;
	fcv->srcBuf = align_alloc(fcv->srcBufSize, 64);

	dstStride = dst_detail->size.w;
	dstSize = dstStride * dst_detail->size.h;
	fcv->dstBuf[0] = align_alloc(dstSize, 64);
	fcv->dstBufSize[0] = dstSize;

	dstSize = dstSize / 4;
	fcv->dstBuf[1] = align_alloc(dstSize, 64);
	fcv->dstBufSize[1] = dstSize;
	fcv->dstBuf[2] = align_alloc(dstSize, 64);
	fcv->dstBufSize[2] = dstSize;

    *p_cv = &fcv->base;

    return PJ_SUCCESS;
}

static void factory_destroy_factory(pjmedia_converter_factory *cf)
{
    PJ_UNUSED_ARG(cf);
}

static pj_status_t libyuv_conv_convert(pjmedia_converter *converter,
					   pjmedia_frame *src_frame,
					   pjmedia_frame *dst_frame)
{
    struct libyuv_converter *fcv = (struct libyuv_converter*)converter;
    struct fmt_info *src = &fcv->src;
	struct fmt_info *dst = &fcv->dst;
	int h = fcv->dst.apply_param.size.h;
	int w = fcv->dst.apply_param.size.w;
	int r;
	uint32 src_fourcc = src->fmt_info->id;
	
	if (src_fourcc == PJMEDIA_FORMAT_RGB24)
		src_fourcc = FOURCC_24BG;

    src->apply_param.buffer = src_frame->buf;
    (*src->fmt_info->apply_fmt)(src->fmt_info, &src->apply_param);

    dst->apply_param.buffer = dst_frame->buf;
    (*dst->fmt_info->apply_fmt)(dst->fmt_info, &dst->apply_param);

	if (dst->fmt_info->id == FOURCC_I420)
	{
		memcpy(fcv->srcBuf, src->apply_param.buffer, src->apply_param.framebytes);

		r = ConvertToI420(fcv->srcBuf, src->apply_param.framebytes,
			fcv->dstBuf[0], w,
			fcv->dstBuf[1], w/2,
			fcv->dstBuf[2], w/2,
			0, 0,
			src->apply_param.size.w, src->apply_param.size.h,
			w, h,
			kRotate0,
			src_fourcc);
		if (r < 0)
			return PJ_EUNKNOWN;

		memcpy(dst->apply_param.planes[0], fcv->dstBuf[0], fcv->dstBufSize[0]);
		memcpy(dst->apply_param.planes[1], fcv->dstBuf[1], fcv->dstBufSize[1]);
		memcpy(dst->apply_param.planes[2], fcv->dstBuf[2], fcv->dstBufSize[2]);

		return PJ_SUCCESS;
	}
	else
	    PJ_UNUSED_ARG(h);

    return PJ_EUNKNOWN;
}

static void libyuv_conv_destroy(pjmedia_converter *converter)
{
	struct libyuv_converter *fcv = (struct libyuv_converter*)converter;
	int i = 0;

	if (fcv->srcBuf != NULL)
	{
		align_free(fcv->srcBuf);
		fcv->srcBuf = NULL;
	}

	for (i=0;i<CSP_BUFFER_SIZE;i++)
	{
		if (fcv->dstBuf[i] != NULL)
		{
			align_free(fcv->dstBuf[i]);
			fcv->dstBuf[i] = NULL;
		}
	}
}

static pjmedia_converter_factory libyuv_factory =
{
    NULL, NULL,					/* list */
    "libyuv",				/* name */
    PJMEDIA_CONVERTER_PRIORITY_NORMAL+1,	/* priority */
    NULL					/* op will be init-ed later  */
};

PJ_DEF(pj_status_t)
pjmedia_libyuv_converter_init(pjmedia_converter_mgr *mgr)
{
    libyuv_factory.op = &libyuv_factory_op;
    return pjmedia_converter_mgr_register_factory(mgr, &libyuv_factory);
}


PJ_DEF(pj_status_t)
pjmedia_libyuv_converter_shutdown(pjmedia_converter_mgr *mgr,
				      pj_pool_t *pool)
{
    PJ_UNUSED_ARG(pool);
    return pjmedia_converter_mgr_unregister_factory(mgr, &libyuv_factory,
						    PJ_TRUE);
}

#ifdef _MSC_VER
#   pragma comment( lib, "libyuv.lib")
#endif

#endif /* #if PJMEDIA_HAS_LIBYUV */
