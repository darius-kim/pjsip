/* $Id: ffmpeg_vid_codecs.c 4089 2012-04-26 07:27:06Z nanang $ */
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
 * openh264_vid_codecs.c
 *
 * this code derived from ffmpeg_vid_codecs.c
 * you can use freely this code under pjsip and openh264 licences
 */

#include <pjmedia-codec/openh264_vid_codecs.h>
#include <pjmedia-codec/h263_packetizer.h>
#include <pjmedia-codec/h264_packetizer.h>
#include <pjmedia/errno.h>
#include <pjmedia/vid_codec_util.h>
#include <pj/assert.h>
#include <pj/list.h>
#include <pj/log.h>
#include <pj/math.h>
#include <pj/pool.h>
#include <pj/string.h>
#include <pj/os.h>
#include <codec_api.h>
#include <codec_app_def.h>

/*
 * Only build this file if PJMEDIA_HAS_CISCO_OPENH264 != 0 and 
 * PJMEDIA_HAS_VIDEO != 0
 */
#if defined(PJMEDIA_HAS_CISCO_OPENH264) && \
            PJMEDIA_HAS_CISCO_OPENH264 != 0 && \
    defined(PJMEDIA_HAS_VIDEO) && (PJMEDIA_HAS_VIDEO != 0)

#define THIS_FILE   "openh264_vid_codecs.c"
#define USE_OPENH264_SINGLE_NAL

#include <pjmedia/format.h>

/* Prototypes for openh264 codecs factory */
static pj_status_t openh264_test_alloc( pjmedia_vid_codec_factory *factory, 
				      const pjmedia_vid_codec_info *id );
static pj_status_t openh264_default_attr( pjmedia_vid_codec_factory *factory, 
				        const pjmedia_vid_codec_info *info, 
				        pjmedia_vid_codec_param *attr );
static pj_status_t openh264_enum_codecs( pjmedia_vid_codec_factory *factory, 
				       unsigned *count, 
				       pjmedia_vid_codec_info codecs[]);
static pj_status_t openh264_alloc_codec( pjmedia_vid_codec_factory *factory, 
				       const pjmedia_vid_codec_info *info, 
				       pjmedia_vid_codec **p_codec);
static pj_status_t openh264_dealloc_codec( pjmedia_vid_codec_factory *factory, 
				         pjmedia_vid_codec *codec );

/* Prototypes for openh264 codecs implementation. */
static pj_status_t  openh264_codec_init( pjmedia_vid_codec *codec, 
				       pj_pool_t *pool );
static pj_status_t  openh264_codec_open( pjmedia_vid_codec *codec, 
				       pjmedia_vid_codec_param *attr );
static pj_status_t  openh264_codec_close( pjmedia_vid_codec *codec );
static pj_status_t  openh264_codec_modify(pjmedia_vid_codec *codec, 
				        const pjmedia_vid_codec_param *attr );
static pj_status_t  openh264_codec_get_param(pjmedia_vid_codec *codec,
					   pjmedia_vid_codec_param *param);
static pj_status_t openh264_codec_encode_begin(pjmedia_vid_codec *codec,
					     const pjmedia_vid_encode_opt *opt,
                                             const pjmedia_frame *input,
					     unsigned out_size,
					     pjmedia_frame *output,
					     pj_bool_t *has_more);
static pj_status_t openh264_codec_encode_more(pjmedia_vid_codec *codec,
					    unsigned out_size,
					    pjmedia_frame *output,
					    pj_bool_t *has_more);
static pj_status_t openh264_codec_decode( pjmedia_vid_codec *codec,
					pj_size_t pkt_count,
					pjmedia_frame packets[],
					unsigned out_size,
					pjmedia_frame *output);

/* Definition for openh264 codecs operations. */
static pjmedia_vid_codec_op openh264_op = 
{
    &openh264_codec_init,
    &openh264_codec_open,
    &openh264_codec_close,
    &openh264_codec_modify,
    &openh264_codec_get_param,
    &openh264_codec_encode_begin,
    &openh264_codec_encode_more,
    &openh264_codec_decode,
    NULL
};

/* Definition for openh264 codecs factory operations. */
static pjmedia_vid_codec_factory_op openh264_factory_op =
{
    &openh264_test_alloc,
    &openh264_default_attr,
    &openh264_enum_codecs,
    &openh264_alloc_codec,
    &openh264_dealloc_codec
};


/* openh264 codecs factory */
static struct openh264_factory {
    pjmedia_vid_codec_factory    base;
    pjmedia_vid_codec_mgr	*mgr;
    pj_pool_factory             *pf;
    pj_pool_t		        *pool;
    pj_mutex_t		        *mutex;
} openh264_factory;


typedef struct openh264_codec_desc openh264_codec_desc;

/* openh264 codecs private data. */
typedef struct openh264_private
{
    const openh264_codec_desc	    *desc;
    pjmedia_vid_codec_param	     param;	/**< Codec param	    */
    pj_pool_t			    *pool;	/**< Pool for each instance */

    /* Format info and apply format param */
    const pjmedia_video_format_info *enc_vfi;
    pjmedia_video_apply_fmt_param    enc_vafp;
    const pjmedia_video_format_info *dec_vfi;
    pjmedia_video_apply_fmt_param    dec_vafp;

    /* Buffers, only needed for multi-packets */
    pj_bool_t			     whole;
    void			    *enc_buf;
    unsigned			     enc_buf_size;
    pj_bool_t			     enc_buf_is_keyframe;
    unsigned			     enc_frame_len;
    unsigned     		     enc_processed;
	pjmedia_rect_size        enc_expected_size;
	int                      enc_expected_bitrate;
	void                    *enc_resize_buffer;
	int                     *enc_resize_buffer_size;
	void			    *unaligned_dec_buf;
    void			    *dec_buf;
    unsigned			     dec_buf_size;
    pj_timestamp		     last_dec_keyframe_ts; 

	ISVCDecoder			*dec;
	ISVCEncoder			*enc;
	SEncParamExt	enc_param;
	SSourcePicture* srcPic;

	SFrameBSInfo		bsInfo;
	int                 bsInfoIndex;
	int                 bsNalIndex;

	int enc_frame_count;

    /* The openh264 decoder cannot set the output format, so format conversion
     * may be needed for post-decoding.
     */
    enum PixelFormat		     expected_dec_fmt;
						/**< Expected output format of 
						     openh264 decoder	    */

    void			    *data;	/**< Codec specific data    */		    
} openh264_private;


/* Shortcuts for packetize & unpacketize function declaration,
 * as it has long params and is reused many times!
 */
#define FUNC_PACKETIZE(name) \
    pj_status_t(name)(openh264_private *ff, pj_uint8_t *bits, \
		      pj_size_t bits_len, unsigned *bits_pos, \
		      const pj_uint8_t **payload, pj_size_t *payload_len)

#define FUNC_UNPACKETIZE(name) \
    pj_status_t(name)(openh264_private *ff, const pj_uint8_t *payload, \
		      pj_size_t payload_len, pj_uint8_t *bits, \
		      pj_size_t bits_len, unsigned *bits_pos)

#define FUNC_FMT_MATCH(name) \
    pj_status_t(name)(pj_pool_t *pool, \
		      pjmedia_sdp_media *offer, unsigned o_fmt_idx, \
		      pjmedia_sdp_media *answer, unsigned a_fmt_idx, \
		      unsigned option)


/* Type definition of codec specific functions */
typedef FUNC_PACKETIZE(*func_packetize);
typedef FUNC_UNPACKETIZE(*func_unpacketize);
typedef pj_status_t (*func_preopen)	(openh264_private *ff);
typedef pj_status_t (*func_postopen)	(openh264_private *ff);
typedef FUNC_FMT_MATCH(*func_sdp_fmt_match);


/* openh264 codec info */
struct openh264_codec_desc
{
    /* Predefined info */
    pjmedia_vid_codec_info       info;
    pjmedia_format_id		 base_fmt_id;	/**< Some codecs may be exactly
						     same or compatible with
						     another codec, base format
						     will tell the initializer
						     to copy this codec desc
						     from its base format   */
    pjmedia_rect_size            size;
    pjmedia_ratio                fps;
    pj_uint32_t			 avg_bps;
    pj_uint32_t			 max_bps;
    func_packetize		 packetize;
    func_unpacketize		 unpacketize;
    func_preopen		 preopen;
    func_preopen		 postopen;
    func_sdp_fmt_match		 sdp_fmt_match;
    pjmedia_codec_fmtp		 dec_fmtp;

    /* Init time defined info */
    pj_bool_t			 enabled;
};

/* H264 constants */
#define PROFILE_H264_BASELINE		66
#define PROFILE_H264_MAIN		77

/* Codec specific functions */
static pj_status_t h264_preopen(openh264_private *ff);
static pj_status_t h264_postopen(openh264_private *ff);
static FUNC_PACKETIZE(h264_packetize);
static FUNC_UNPACKETIZE(h264_unpacketize);

#define callWelsDecoderFn(codec) ((const ISVCDecoderVtbl *)*codec)
#define callWelsEncoderFn(codec) ((const ISVCEncoderVtbl *)*codec)

/* Internal codec info */
static openh264_codec_desc codec_desc[] =
{
    {
	{PJMEDIA_FORMAT_H264, PJMEDIA_RTP_PT_H264, {"H264",4},
	 {"Constrained Baseline (level=30, pack=1)", 39}},
	0,
	{640, 480},	{30, 1},	512000, 512000,
	&h264_packetize, &h264_unpacketize, &h264_preopen, &h264_postopen,
	&pjmedia_vid_codec_h264_match_sdp,
	/* Leading space for better compatibility (strange indeed!) */
	{2, { {{"profile-level-id",16},    {"42e01e",6}}, 
	      {{" packetization-mode",19},  {"1",1}}, } },
    },
};

typedef struct h264_data
{
    pjmedia_vid_codec_h264_fmtp	 fmtp;
    pjmedia_h264_packetizer	*pktz;
} h264_data;

static pj_uint8_t* align_buffer_16(pj_uint8_t **unAlignedbuffer)
{
	pj_uint8_t *var = (pj_uint8_t*)(((intptr_t)(unAlignedbuffer) + 16) & ~16);
	return var;
}

static pj_status_t h264_preopen(openh264_private *ff)
{
    h264_data *data;
    pjmedia_h264_packetizer_cfg pktz_cfg;
    pj_status_t status;

    data = PJ_POOL_ZALLOC_T(ff->pool, h264_data);
    ff->data = data;

    /* Parse remote fmtp */
    status = pjmedia_vid_codec_h264_parse_fmtp(&ff->param.enc_fmtp,
					       &data->fmtp);
    if (status != PJ_SUCCESS)
		return status;

    /* Create packetizer */
    pktz_cfg.mtu = ff->param.enc_mtu;
    if (data->fmtp.packetization_mode!=
				PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL &&
		data->fmtp.packetization_mode!=
					PJMEDIA_H264_PACKETIZER_MODE_NON_INTERLEAVED)
    {
		return PJ_ENOTSUP;
    }
    /* Better always send in single NAL mode for better compatibility */
	pktz_cfg.mode = PJMEDIA_H264_PACKETIZER_MODE_SINGLE_NAL;    
    status = pjmedia_h264_packetizer_create(ff->pool, &pktz_cfg, &data->pktz);
    if (status != PJ_SUCCESS)
		return status;

    /* Apply SDP fmtp to format in codec param */
    if (!ff->param.ignore_fmtp) {
		status = pjmedia_vid_codec_h264_apply_fmtp(&ff->param);
		if (status != PJ_SUCCESS)
			return status;
    }

    return PJ_SUCCESS;
}

static pj_status_t h264_postopen(openh264_private *ff)
{
    h264_data *data = (h264_data*)ff->data;
    PJ_UNUSED_ARG(data);
    return PJ_SUCCESS;
}

static FUNC_PACKETIZE(h264_packetize)
{
    h264_data *data = (h264_data*)ff->data;
    return pjmedia_h264_packetize(data->pktz, bits, bits_len, bits_pos,
				  payload, payload_len);
}

static FUNC_UNPACKETIZE(h264_unpacketize)
{
    h264_data *data = (h264_data*)ff->data;
    return pjmedia_h264_unpacketize(data->pktz, payload, payload_len,
				    bits, bits_len, bits_pos);
}

static const openh264_codec_desc* find_codec_desc_by_info(
			const pjmedia_vid_codec_info *info)
{
    int i;

    for (i=0; i<PJ_ARRAY_SIZE(codec_desc); ++i) {
	openh264_codec_desc *desc = &codec_desc[i];

	if (desc->enabled &&
	    (desc->info.fmt_id == info->fmt_id) &&
            ((desc->info.dir & info->dir) == info->dir) &&
	    (desc->info.pt == info->pt) &&
	    (desc->info.packings & info->packings))
        {
            return desc;
        }
    }

    return NULL;
}


static int find_codec_idx_by_fmt_id(pjmedia_format_id fmt_id)
{
    int i;
    for (i=0; i<PJ_ARRAY_SIZE(codec_desc); ++i) {
	if (codec_desc[i].info.fmt_id == fmt_id)
	    return i;
    }

    return -1;
}


/*
 * Initialize and register openh264 codec factory to pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_openh264_vid_init(pjmedia_vid_codec_mgr *mgr,
                                                  pj_pool_factory *pf)
{
    pj_pool_t *pool;
    pj_status_t status;
	openh264_codec_desc *desc = &codec_desc[0];

    if (openh264_factory.pool != NULL) {
		/* Already initialized. */
		return PJ_SUCCESS;
    }

    if (!mgr)
		mgr = pjmedia_vid_codec_mgr_instance();

    PJ_ASSERT_RETURN(mgr, PJ_EINVAL);

    /* Create openh264 codec factory. */
    openh264_factory.base.op = &openh264_factory_op;
    openh264_factory.base.factory_data = NULL;
    openh264_factory.mgr = mgr;
    openh264_factory.pf = pf;

    pool = pj_pool_create(pf, "openh264 codec factory", 256, 256, NULL);
    if (!pool)
		return PJ_ENOMEM;

    /* Create mutex. */
    status = pj_mutex_create_simple(pool, "openh264 codec factory", 
				    &openh264_factory.mutex);
    if (status != PJ_SUCCESS)
		goto on_error;

	desc->info.dec_fmt_id[0] = PJMEDIA_FORMAT_I420;
	desc->info.dec_fmt_id_cnt = 1;
    desc->info.dir |= PJMEDIA_DIR_ENCODING;
    desc->info.dir |= PJMEDIA_DIR_DECODING;
	desc->enabled = PJ_TRUE;

	if (desc->info.clock_rate == 0)
	    desc->info.clock_rate = 90000;

	desc->info.packings |= PJMEDIA_VID_PACKING_WHOLE;
	if (desc->packetize && desc->unpacketize)
	    desc->info.packings |= PJMEDIA_VID_PACKING_PACKETS;

	/* Registering format match for SDP negotiation */
	if (desc->sdp_fmt_match) {
		status = pjmedia_sdp_neg_register_fmt_match_cb(
			&desc->info.encoding_name,
			desc->sdp_fmt_match);
		pj_assert(status == PJ_SUCCESS);
	}

    /* Register codec factory to codec manager. */
    status = pjmedia_vid_codec_mgr_register_factory(mgr, 
						    &openh264_factory.base);
    if (status != PJ_SUCCESS)
		goto on_error;

    openh264_factory.pool = pool;

    /* Done. */
    return PJ_SUCCESS;

on_error:
    pj_pool_release(pool);
    return status;
}

/*
 * Unregister openh264 codecs factory from pjmedia endpoint.
 */
PJ_DEF(pj_status_t) pjmedia_codec_openh264_vid_deinit(void)
{
    pj_status_t status = PJ_SUCCESS;

    if (openh264_factory.pool == NULL) {
		/* Already deinitialized */
		return PJ_SUCCESS;
    }

    pj_mutex_lock(openh264_factory.mutex);

    /* Unregister openh264 codecs factory. */
    status = pjmedia_vid_codec_mgr_unregister_factory(openh264_factory.mgr,
						      &openh264_factory.base);

    /* Destroy mutex. */
    pj_mutex_destroy(openh264_factory.mutex);

    /* Destroy pool. */
    pj_pool_release(openh264_factory.pool);
    openh264_factory.pool = NULL;

    return status;
}


/* 
 * Check if factory can allocate the specified codec. 
 */
static pj_status_t openh264_test_alloc( pjmedia_vid_codec_factory *factory, 
				      const pjmedia_vid_codec_info *info )
{
    const openh264_codec_desc *desc;

    PJ_ASSERT_RETURN(factory==&openh264_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc) {
        return PJMEDIA_CODEC_EUNSUP;
    }

    return PJ_SUCCESS;
}

/*
 * Generate default attribute.
 */
static pj_status_t openh264_default_attr( pjmedia_vid_codec_factory *factory, 
				        const pjmedia_vid_codec_info *info, 
				        pjmedia_vid_codec_param *attr )
{
    const openh264_codec_desc *desc;
    unsigned i;

    PJ_ASSERT_RETURN(factory==&openh264_factory.base, PJ_EINVAL);
    PJ_ASSERT_RETURN(info && attr, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc) {
        return PJMEDIA_CODEC_EUNSUP;
    }

    pj_bzero(attr, sizeof(pjmedia_vid_codec_param));

    /* Scan the requested packings and use the lowest number */
    attr->packing = 0;
    for (i=0; i<15; ++i) {
		unsigned packing = (1 << i);
		if ((desc->info.packings & info->packings) & packing) {
			attr->packing = (pjmedia_vid_packing)packing;
			break;
		}
    }
    if (attr->packing == 0) {
		/* No supported packing in info */
		return PJMEDIA_CODEC_EUNSUP;
    }

    /* Direction */
    attr->dir = desc->info.dir;

    /* Encoded format */
    pjmedia_format_init_video(&attr->enc_fmt, desc->info.fmt_id,
                              desc->size.w, desc->size.h,
			      desc->fps.num, desc->fps.denum);

    /* Decoded format */
    pjmedia_format_init_video(&attr->dec_fmt, desc->info.dec_fmt_id[0],
                              desc->size.w, desc->size.h,
			      desc->fps.num, desc->fps.denum);

    /* Decoding fmtp */
    attr->dec_fmtp = desc->dec_fmtp;

    /* Bitrate */
    attr->enc_fmt.det.vid.avg_bps = desc->avg_bps;
    attr->enc_fmt.det.vid.max_bps = desc->max_bps;

    /* Encoding MTU */
    attr->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    return PJ_SUCCESS;
}

/*
 * Enum codecs supported by this factory.
 */
static pj_status_t openh264_enum_codecs( pjmedia_vid_codec_factory *factory,
				       unsigned *count, 
				       pjmedia_vid_codec_info codecs[])
{
    unsigned i, max_cnt;

    PJ_ASSERT_RETURN(codecs && *count > 0, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &openh264_factory.base, PJ_EINVAL);

    max_cnt = PJ_MIN(*count, PJ_ARRAY_SIZE(codec_desc));
    *count = 0;

    for (i=0; i<max_cnt; ++i) {
		if (codec_desc[i].enabled) {
			pj_memcpy(&codecs[*count], &codec_desc[i].info, 
				  sizeof(pjmedia_vid_codec_info));
			(*count)++;
		}
    }

    return PJ_SUCCESS;
}

/*
 * Allocate a new codec instance.
 */
static pj_status_t openh264_alloc_codec( pjmedia_vid_codec_factory *factory, 
				       const pjmedia_vid_codec_info *info,
				       pjmedia_vid_codec **p_codec)
{
    openh264_private *ff;
    const openh264_codec_desc *desc;
    pjmedia_vid_codec *codec;
    pj_pool_t *pool = NULL;
    pj_status_t status = PJ_SUCCESS;

    PJ_ASSERT_RETURN(factory && info && p_codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &openh264_factory.base, PJ_EINVAL);

    desc = find_codec_desc_by_info(info);
    if (!desc) {
        return PJMEDIA_CODEC_EUNSUP;
    }

    /* Create pool for codec instance */
    pool = pj_pool_create(openh264_factory.pf, "openh264 codec", 512, 512, NULL);
    codec = PJ_POOL_ZALLOC_T(pool, pjmedia_vid_codec);
    if (!codec) {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec->op = &openh264_op;
    codec->factory = factory;
    ff = PJ_POOL_ZALLOC_T(pool, openh264_private);
    if (!ff) {
        status = PJ_ENOMEM;
        goto on_error;
    }
    codec->codec_data = ff;
    ff->pool = pool;
    ff->desc = desc;
	ff->enc_expected_size.w = 0;
	ff->enc_expected_size.h = 0;
	ff->dec = NULL;
	ff->enc = NULL;

    *p_codec = codec;
    return PJ_SUCCESS;

on_error:
    if (pool)
        pj_pool_release(pool);
    return status;
}

/*
 * Free codec.
 */
static pj_status_t openh264_dealloc_codec( pjmedia_vid_codec_factory *factory, 
				         pjmedia_vid_codec *codec )
{
    openh264_private *ff;
    pj_pool_t *pool;

    PJ_ASSERT_RETURN(factory && codec, PJ_EINVAL);
    PJ_ASSERT_RETURN(factory == &openh264_factory.base, PJ_EINVAL);

    /* Close codec, if it's not closed. */
    ff = (openh264_private*) codec->codec_data;
    pool = ff->pool;
    codec->codec_data = NULL;
    pj_pool_release(pool);

    return PJ_SUCCESS;
}

/*
 * Init codec.
 */
static pj_status_t openh264_codec_init( pjmedia_vid_codec *codec, 
				      pj_pool_t *pool )
{
    PJ_UNUSED_ARG(codec);
    PJ_UNUSED_ARG(pool);
    return PJ_SUCCESS;
}

static pj_status_t open_openh264_codec(openh264_private *ff,
                                     pj_mutex_t *ff_mutex)
{
    pjmedia_video_format_detail *vfd;
    pj_bool_t enc_opened = PJ_FALSE, dec_opened = PJ_FALSE;
    pj_status_t status;

    vfd = pjmedia_format_get_video_format_detail(&ff->param.enc_fmt, 
						 PJ_TRUE);

    /* Override generic params or apply specific params before opening
     * the codec.
     */
    if (ff->desc->preopen) {
		status = (*ff->desc->preopen)(ff);
		if (status != PJ_SUCCESS)
			goto on_error;
    }

    /* Open encoder */
    if (ff->param.dir & PJMEDIA_DIR_ENCODING) {
		int err;
		SEncParamExt *param = &ff->enc_param;
		const openh264_codec_desc *desc = &ff->desc[0];
		bool disable = 0;
		int iIndexLayer = 0;
		SSourcePicture *srcPic;

		pj_mutex_lock(ff_mutex);
		memset(param, 0x00, sizeof(SEncParamExt));
		CreateSVCEncoder(&ff->enc);
		
		/* Test for temporal, spatial, SNR scalability */
		param->fMaxFrameRate = (float)vfd->fps.num;		// input frame rate
		param->iPicWidth	= vfd->size.w;		// width of picture in samples
		param->iPicHeight	= vfd->size.h;		// height of picture in samples
		param->iTargetBitrate = desc->avg_bps;		// target bitrate desired
		param->bEnableRc = PJ_TRUE;           //  rc mode control
		param->iTemporalLayerNum = 3;	// layer number at temporal level
		param->iSpatialLayerNum	= 1;	// layer number at spatial level
		param->bEnableDenoise   = PJ_TRUE;    // denoise control
		param->bEnableBackgroundDetection = PJ_TRUE; // background detection control
		param->bEnableAdaptiveQuant       = PJ_TRUE; // adaptive quantization control
		param->bEnableFrameSkip           = PJ_TRUE; // frame skipping
		param->bEnableLongTermReference   = PJ_FALSE; // long term reference control
		param->bEnableFrameCroppingFlag   = PJ_FALSE;
		param->iLoopFilterDisableIdc = 0;

		param->iInputCsp			= videoFormatI420;			// color space of input sequence
		param->uiIntraPeriod		= 300;		// period of Intra frame
		param->bEnableSpsPpsIdAddition = 0;
		param->bPrefixNalAddingCtrl = 0;
		
		param->sSpatialLayers[iIndexLayer].iVideoWidth	= vfd->size.w;
		param->sSpatialLayers[iIndexLayer].iVideoHeight	= vfd->size.h;
		param->sSpatialLayers[iIndexLayer].fFrameRate	= (float)vfd->fps.num;		
		param->sSpatialLayers[iIndexLayer].iSpatialBitrate	= desc->avg_bps;
// 		param->sSpatialLayers[iIndexLayer].iDLayerQp = 50;
 		param->sSpatialLayers[iIndexLayer].uiProfileIdc = 66;
		param->sSpatialLayers[iIndexLayer].sSliceCfg.uiSliceMode = 4;
		param->sSpatialLayers[iIndexLayer].sSliceCfg.sSliceArgument.uiSliceSizeConstraint = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

		err = callWelsEncoderFn(ff->enc)->InitializeExt(ff->enc, param);
		if (err == cmResultSuccess)
		{			
			callWelsEncoderFn(ff->enc)->SetOption(ff->enc, ENCODER_OPTION_ENABLE_SSEI, &disable);
			enc_opened = PJ_TRUE;
		}

		srcPic = malloc(sizeof(SSourcePicture));
		memset(srcPic, 0x00, sizeof(SSourcePicture));
		srcPic->iColorFormat = param->iInputCsp;
		srcPic->iPicWidth = param->iPicWidth;
		srcPic->iPicHeight = param->iPicHeight;
		srcPic->iStride[0] = param->iPicWidth;
		srcPic->iStride[1] = param->iPicWidth / 2;
		srcPic->iStride[2] = param->iPicWidth / 2;

		ff->srcPic = srcPic;
		pj_mutex_unlock(ff_mutex);				
    }

    /* Open decoder */
    if (ff->param.dir & PJMEDIA_DIR_DECODING) {
		SDecodingParam sDecParam = {0};

		pj_mutex_lock(ff_mutex);
		
		CreateDecoder(&ff->dec);
		sDecParam.iOutputColorFormat	= videoFormatI420;
		sDecParam.uiTargetDqLayer	= (unsigned char)-1;
		sDecParam.uiEcActiveFlag	= 1;
		sDecParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
 		callWelsDecoderFn(ff->dec)->Initialize(ff->dec, &sDecParam);

		pj_mutex_unlock(ff_mutex);
		dec_opened = PJ_TRUE;
    }

    /* Let the codec apply specific params after the codec opened */
    if (ff->desc->postopen) {
		status = (*ff->desc->postopen)(ff);
		if (status != PJ_SUCCESS)
			goto on_error;
    }

    return PJ_SUCCESS;

on_error:
    return status;
}

/*
 * Open codec.
 */
static pj_status_t openh264_codec_open( pjmedia_vid_codec *codec, 
				      pjmedia_vid_codec_param *attr )
{
    openh264_private *ff;
    pj_status_t status;
    pj_mutex_t *ff_mutex;
	pjmedia_rect_size enc_rect = { 176, 144 };	

    PJ_ASSERT_RETURN(codec && attr, PJ_EINVAL);
    ff = (openh264_private*)codec->codec_data;

    pj_memcpy(&ff->param, attr, sizeof(*attr));

	if (ff->param.dir == PJMEDIA_DIR_ENCODING)
	{
		ff->enc_expected_size = enc_rect;
		ff->enc_expected_bitrate = 160 * 1000;
		ff->enc_frame_count = 0;
	}

    /* Normalize encoding MTU in codec param */
    if (attr->enc_mtu > PJMEDIA_MAX_VID_PAYLOAD_SIZE)
		attr->enc_mtu = PJMEDIA_MAX_VID_PAYLOAD_SIZE;

    /* Open the codec */
    ff_mutex = ((struct openh264_factory*)codec->factory)->mutex;
    status = open_openh264_codec(ff, ff_mutex);
    if (status != PJ_SUCCESS)
        goto on_error;

    /* Init format info and apply-param of decoder */
    ff->dec_vfi = pjmedia_get_video_format_info(NULL, ff->param.dec_fmt.id);
    if (!ff->dec_vfi) {
        status = PJ_EINVAL;
        goto on_error;
    }
    pj_bzero(&ff->dec_vafp, sizeof(ff->dec_vafp));
    ff->dec_vafp.size = ff->param.dec_fmt.det.vid.size;
    ff->dec_vafp.buffer = NULL;
    status = (*ff->dec_vfi->apply_fmt)(ff->dec_vfi, &ff->dec_vafp);
    if (status != PJ_SUCCESS) {
        goto on_error;
    }

    /* Init format info and apply-param of encoder */
    ff->enc_vfi = pjmedia_get_video_format_info(NULL, ff->param.dec_fmt.id);
    if (!ff->enc_vfi) {
        status = PJ_EINVAL;
        goto on_error;
    }
    pj_bzero(&ff->enc_vafp, sizeof(ff->enc_vafp));
    ff->enc_vafp.size = ff->param.enc_fmt.det.vid.size;
    ff->enc_vafp.buffer = NULL;
    status = (*ff->enc_vfi->apply_fmt)(ff->enc_vfi, &ff->enc_vafp);
    if (status != PJ_SUCCESS) {
        goto on_error;
    }

    /* Alloc buffers if needed */
    ff->whole = (ff->param.packing == PJMEDIA_VID_PACKING_WHOLE);
    if (!ff->whole) {
		ff->enc_buf_size = ff->enc_vafp.framebytes;
		ff->enc_buf = pj_pool_alloc(ff->pool, ff->enc_buf_size);

		ff->dec_buf_size = ff->dec_vafp.framebytes;
		ff->unaligned_dec_buf = pj_pool_alloc(ff->pool, ff->dec_buf_size);
		ff->dec_buf = align_buffer_16(ff->unaligned_dec_buf);
    }

    /* Update codec attributes, e.g: encoding format may be changed by
     * SDP fmtp negotiation.
     */
    pj_memcpy(attr, &ff->param, sizeof(*attr));
    return PJ_SUCCESS;

on_error:
    openh264_codec_close(codec);
    return status;
}

/*
 * Close codec.
 */
static pj_status_t openh264_codec_close( pjmedia_vid_codec *codec )
{
    openh264_private *ff;
    pj_mutex_t *ff_mutex;

    PJ_ASSERT_RETURN(codec, PJ_EINVAL);
    ff = (openh264_private*)codec->codec_data;
    ff_mutex = ((struct openh264_factory*)codec->factory)->mutex;

    pj_mutex_lock(ff_mutex);

	if ((ff->enc != NULL) && (ff->param.dir & PJMEDIA_DIR_ENCODING))
	{
		DestroySVCEncoder(ff->enc);
		ff->enc = NULL;
	}
	if ((ff->dec != NULL) && (ff->param.dir & PJMEDIA_DIR_DECODING))
	{
		DestroyDecoder(ff->dec);
		ff->dec = NULL;
	}
	if (ff->srcPic != NULL)
	{
		free(ff->srcPic);
		ff->srcPic = NULL;
	}

    pj_mutex_unlock(ff_mutex);

    return PJ_SUCCESS;
}


/*
 * Modify codec settings.
 */
static pj_status_t  openh264_codec_modify( pjmedia_vid_codec *codec, 
				         const pjmedia_vid_codec_param *attr)
{
    PJ_UNUSED_ARG(attr);
	PJ_UNUSED_ARG(codec);
    return PJ_ENOTSUP;
}

static pj_status_t  openh264_codec_get_param(pjmedia_vid_codec *codec,
					   pjmedia_vid_codec_param *param)
{
    openh264_private *ff;

    PJ_ASSERT_RETURN(codec && param, PJ_EINVAL);

    ff = (openh264_private*)codec->codec_data;
    pj_memcpy(param, &ff->param, sizeof(*param));

    return PJ_SUCCESS;
}


static pj_status_t  openh264_packetize ( pjmedia_vid_codec *codec,
                                       pj_uint8_t *bits,
                                       pj_size_t bits_len,
                                       unsigned *bits_pos,
                                       const pj_uint8_t **payload,
                                       pj_size_t *payload_len)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;

    if (ff->desc->packetize) {
	return (*ff->desc->packetize)(ff, bits, bits_len, bits_pos,
                                      payload, payload_len);
    }

    return PJ_ENOTSUP;
}

static pj_status_t  openh264_unpacketize(pjmedia_vid_codec *codec,
                                       const pj_uint8_t *payload,
                                       pj_size_t   payload_len,
                                       pj_uint8_t *bits,
                                       pj_size_t   bits_len,
				       unsigned   *bits_pos)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;

    if (ff->desc->unpacketize) {
        return (*ff->desc->unpacketize)(ff, payload, payload_len,
                                        bits, bits_len, bits_pos);
    }
    
    return PJ_ENOTSUP;
}


pj_status_t copyBitstream(SLayerBSInfo *bsinfo,
						pjmedia_frame *output,
						const unsigned output_buf_len,
						const pj_size_t offset,
						pj_size_t *writeSize,
						pj_bool_t non_coded_layer)
{
	int i;
	unsigned bs_size = 0;
	unsigned char *bufPtr = output->buf;

	if ((non_coded_layer == PJ_FALSE) && (bsinfo->uiLayerType==0))
		return PJ_FALSE;

	for (i=0;i<bsinfo->iNalCount; i++)
		bs_size += bsinfo->iNalLengthInByte[i];

	if (bs_size > output_buf_len)
		return PJMEDIA_CODEC_EFRMTOOSHORT;

	if (bs_size > 0)
		memcpy(bufPtr + offset, bsinfo->pBsBuf, bs_size);
	*writeSize += bs_size;

	return PJ_SUCCESS;
}

/*
 * Encode frames.
 */
static pj_status_t openh264_codec_encode_whole(pjmedia_vid_codec *codec,
					     const pjmedia_vid_encode_opt *opt,
					     const pjmedia_frame *input,
					     unsigned output_buf_len,
					     pjmedia_frame *output)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;
	SFrameBSInfo *bsInfo = &ff->bsInfo;
	int frameType;
	pj_size_t outSize = 0;
	SSourcePicture *srcPic = ff->srcPic;
	SEncParamExt *encParam = &ff->enc_param;
	unsigned char *yAddress = input->buf;
	pj_size_t uOffset;
	pj_size_t vOffset;

    /* Check if encoder has been opened */
    PJ_ASSERT_RETURN(ff->enc, PJ_EINVALIDOP);
	PJ_UNUSED_ARG(output_buf_len);

	ff->bsInfoIndex = 0;
	ff->bsNalIndex = 0;
	output->size = 0;
    /* Force keyframe */
    if ((opt && opt->force_keyframe) || (ff->enc_frame_count < 5))
	{
		callWelsEncoderFn(ff->enc)->ForceIntraFrame(ff->enc, PJ_TRUE);
    }	

	uOffset = encParam->iPicWidth * encParam->iPicHeight;
	vOffset = uOffset / 4;
	srcPic->pData[0] = yAddress;
	srcPic->pData[1] = yAddress + uOffset;
	srcPic->pData[2] = srcPic->pData[1] + vOffset;
	memset(bsInfo, 0x00, sizeof(SFrameBSInfo));
	frameType = callWelsEncoderFn(ff->enc)->EncodeFrame(ff->enc, srcPic, bsInfo);
	if (frameType == videoFrameTypeInvalid)
	{
		printf("videoFrameTypeInvalid : %d\n", frameType);
		return PJMEDIA_CODEC_EFAILED;
	}	
	else if (frameType == videoFrameTypeIDR)
	{
		output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;
	}
	else if (frameType == videoFrameTypeP)
	{
		pj_uint8_t nal_ref_idc = bsInfo->sLayerInfo->pBsBuf[4] & 0x60;
		if (nal_ref_idc == 0)
			return PJMEDIA_CODEC_EFAILED;
	}
	else if (frameType == videoFrameTypeI)
	{
		printf("videoFrameType : %d\n", frameType);
	}
	else
		return PJMEDIA_CODEC_EFAILED;
	
	output->size = outSize;
	ff->enc_frame_count++;

    return PJ_SUCCESS;
}

static pj_status_t openh264_codec_encode_begin(pjmedia_vid_codec *codec,
					     const pjmedia_vid_encode_opt *opt,
					     const pjmedia_frame *input,
					     unsigned out_size,
					     pjmedia_frame *output,
					     pj_bool_t *has_more)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;
    pj_status_t status;

    *has_more = PJ_FALSE;

    if (ff->whole) {
		status = openh264_codec_encode_whole(codec, opt, input, out_size,
						   output);
    } else {
		pjmedia_frame whole_frm;
        const pj_uint8_t *payload;
        pj_size_t payload_len;
		SLayerBSInfo *layerInfo;

		pj_bzero(&whole_frm, sizeof(whole_frm));
		whole_frm.buf = ff->enc_buf;
		whole_frm.size = ff->enc_buf_size;
		status = openh264_codec_encode_whole(codec, opt, input,
										   whole_frm.size, &whole_frm);
		if (status != PJ_SUCCESS)
			return status;

		ff->enc_buf_is_keyframe = (whole_frm.bit_info & PJMEDIA_VID_FRM_KEYFRAME);
 		ff->enc_processed = 0;

		layerInfo = &ff->bsInfo.sLayerInfo[ff->bsInfoIndex];
		payload_len = layerInfo->iNalLengthInByte[ff->bsNalIndex];
		payload = layerInfo->pBsBuf + ff->enc_processed;		
		ff->enc_processed += payload_len;
		ff->bsNalIndex++;

		if (ff->bsNalIndex >= layerInfo->iNalCount)
		{
			ff->bsInfoIndex++;
			ff->bsNalIndex = 0;
			ff->enc_processed = 0;
		}

		if (ff->bsInfoIndex < ff->bsInfo.iLayerNum)
			*has_more = PJ_TRUE;

		payload += 4;
		payload_len -= 4;

		if (out_size < payload_len)
			return PJMEDIA_CODEC_EFRMTOOSHORT;

		output->type = PJMEDIA_FRAME_TYPE_VIDEO;
		pj_memcpy(output->buf, payload, payload_len);
		output->size = payload_len;

		if (ff->enc_buf_is_keyframe)
			output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;
    }

    return status;
}

static pj_status_t openh264_codec_encode_more(pjmedia_vid_codec *codec,
					    unsigned out_size,
					    pjmedia_frame *output,
					    pj_bool_t *has_more)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;
    const pj_uint8_t *payload;
    pj_size_t payload_len;
	SLayerBSInfo *layerInfo = &ff->bsInfo.sLayerInfo[ff->bsInfoIndex];

    *has_more = PJ_FALSE;

	if (ff->bsInfoIndex >= ff->bsInfo.iLayerNum)
		return PJ_EEOF;

	payload_len = layerInfo->iNalLengthInByte[ff->bsNalIndex];
	payload = layerInfo->pBsBuf + ff->enc_processed;		
	ff->enc_processed += payload_len;
	ff->bsNalIndex++;

	if (ff->bsNalIndex >= layerInfo->iNalCount)
	{
		ff->bsInfoIndex++;
		ff->bsNalIndex = 0;
		ff->enc_processed = 0;
	}

	if (ff->bsInfoIndex < ff->bsInfo.iLayerNum)
		*has_more = PJ_TRUE;

	payload += 4;
	payload_len -= 4;

	if (out_size < payload_len)
		return PJMEDIA_CODEC_EFRMTOOSHORT;

	pj_assert(payload_len < PJMEDIA_MAX_VID_PAYLOAD_SIZE);
    output->type = PJMEDIA_FRAME_TYPE_VIDEO;
    pj_memcpy(output->buf, payload, payload_len);
    output->size = payload_len;

    if (ff->enc_buf_is_keyframe)
		output->bit_info |= PJMEDIA_VID_FRM_KEYFRAME;

    return PJ_SUCCESS;
}

static pj_status_t check_new_decode_result(pjmedia_vid_codec *codec,
				       SBufferInfo *bufferInfo,
					   const pj_timestamp *ts,
				       pj_bool_t got_keyframe)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;
    pjmedia_video_apply_fmt_param *vafp = &ff->dec_vafp;
    pjmedia_event event;

    /* Check for format change.
     * Decoder output format is set by libavcodec, in case it is different
     * to the configured param.
     */
    if (bufferInfo->UsrData.sSystemBuffer.iWidth != (int)vafp->size.w ||
		bufferInfo->UsrData.sSystemBuffer.iHeight != (int)vafp->size.h)
	{
		pj_status_t status;

		/* Update decoder format in param */
		ff->param.dec_fmt.id = PJMEDIA_FORMAT_I420;
		ff->param.dec_fmt.det.vid.size.w = bufferInfo->UsrData.sSystemBuffer.iWidth;
		ff->param.dec_fmt.det.vid.size.h = bufferInfo->UsrData.sSystemBuffer.iHeight;

		/* Re-init format info and apply-param of decoder */
		ff->dec_vfi = pjmedia_get_video_format_info(NULL, ff->param.dec_fmt.id);
		if (!ff->dec_vfi)
			return PJ_ENOTSUP;
		pj_bzero(&ff->dec_vafp, sizeof(ff->dec_vafp));
		ff->dec_vafp.size = ff->param.dec_fmt.det.vid.size;
		ff->dec_vafp.buffer = NULL;
		status = (*ff->dec_vfi->apply_fmt)(ff->dec_vfi, &ff->dec_vafp);
		if (status != PJ_SUCCESS)
			return status;

		/* Realloc buffer if necessary */
		if (ff->dec_vafp.framebytes > ff->dec_buf_size) {
			PJ_LOG(5,(THIS_FILE, "Reallocating decoding buffer %u --> %u",
				   (unsigned)ff->dec_buf_size,
				   (unsigned)ff->dec_vafp.framebytes));
			ff->dec_buf_size = ff->dec_vafp.framebytes;
			ff->unaligned_dec_buf = pj_pool_alloc(ff->pool, ff->dec_buf_size);
			ff->dec_buf = align_buffer_16(ff->unaligned_dec_buf);
		}

		/* Broadcast format changed event */
		pjmedia_event_init(&event, PJMEDIA_EVENT_FMT_CHANGED, ts, codec);
		event.data.fmt_changed.dir = PJMEDIA_DIR_DECODING;
		pj_memcpy(&event.data.fmt_changed.new_fmt, &ff->param.dec_fmt,
			  sizeof(ff->param.dec_fmt));
		pjmedia_event_publish(NULL, codec, &event, 0);
    }

    /* Check for missing/found keyframe */
    if (got_keyframe) {
		pj_get_timestamp(&ff->last_dec_keyframe_ts);

	/* Broadcast keyframe event */
        pjmedia_event_init(&event, PJMEDIA_EVENT_KEYFRAME_FOUND, ts, codec);
        pjmedia_event_publish(NULL, codec, &event, 0);
    } else if (ff->last_dec_keyframe_ts.u64 == 0) {
	/* Broadcast missing keyframe event */
// 	pjmedia_event_init(&event, PJMEDIA_EVENT_KEYFRAME_MISSING, ts, codec);
// 	pjmedia_event_publish(NULL, codec, &event, 0);
    }

    return PJ_SUCCESS;
}

/*
 * Decode frame.
 */
static pj_status_t openh264_codec_decode_whole(pjmedia_vid_codec *codec,
					     const pjmedia_frame *input,
					     unsigned output_buf_len,
					     pjmedia_frame *output)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;
	void* pData[3] = {NULL};
	SBufferInfo pDstInfo;
	static pj_bool_t got_keyframe = PJ_FALSE;
	DECODING_STATE decodingState;

    /* Check if decoder has been opened */
//    PJ_ASSERT_RETURN(ff->dec_ctx, PJ_EINVALIDOP);
	PJ_ASSERT_RETURN(ff->dec, PJ_EINVALIDOP);

    /* Reset output frame bit info */
    output->bit_info = 0;
    output->timestamp = input->timestamp;

	pData[0] = NULL;
	pData[1] = NULL;
	pData[2] = NULL;
	memset(&pDstInfo, 0x00, sizeof(SBufferInfo));
	decodingState = callWelsDecoderFn(ff->dec)->DecodeFrame2(ff->dec, input->buf, input->size, pData, &pDstInfo);
	if (pDstInfo.iBufferStatus == 1) {
		pjmedia_video_apply_fmt_param *vafp = &ff->dec_vafp;
        pj_uint8_t *q = (pj_uint8_t*)output->buf;
		unsigned i;
		pj_status_t status;

		/* Check decoding result, e.g: see if the format got changed,
		 * keyframe found/missing.
		 */
		got_keyframe = PJ_TRUE;
		status = check_new_decode_result(codec, &pDstInfo, &input->timestamp, got_keyframe);
		if (status != PJ_SUCCESS)
			return status;

		/* Check provided buffer size */
		if (vafp->framebytes > output_buf_len)
			return PJ_ETOOSMALL;

		/* Get the decoded data */
		for (i = 0; i < ff->dec_vfi->plane_cnt; ++i) {
			pj_uint8_t *p = pData[i];

			/* The decoded data may contain padding */
			if (pDstInfo.UsrData.sSystemBuffer.iStride[(i==0)?0:1]!=vafp->strides[i]) {
				/* Padding exists, copy line by line */
				pj_uint8_t *q_end;
		                    
				q_end = q+vafp->plane_bytes[i];
				while(q < q_end) {
					pj_memcpy(q, p, vafp->strides[i]);
					q += vafp->strides[i];
					p += pDstInfo.UsrData.sSystemBuffer.iStride[(i==0)?0:1];
				}
			} else {
				/* No padding, copy the whole plane */
				pj_memcpy(q, p, vafp->plane_bytes[i]);
				q += vafp->plane_bytes[i];
			}
		}

		output->type = PJMEDIA_FRAME_TYPE_VIDEO;
        output->size = vafp->framebytes;		
	} else {
		output->type = PJMEDIA_FRAME_TYPE_NONE;
		output->size = 0;
		got_keyframe = PJ_FALSE;

		if (decodingState == dsNoParamSets) {
			pjmedia_event event;

			/* Broadcast missing keyframe event */
			pjmedia_event_init(&event, PJMEDIA_EVENT_KEYFRAME_MISSING,
				&input->timestamp, codec);
			pjmedia_event_publish(NULL, codec, &event, 0);
		}
		return PJMEDIA_CODEC_EBADBITSTREAM;
    }

    return PJ_SUCCESS;
}

static pj_status_t openh264_codec_decode( pjmedia_vid_codec *codec,
					pj_size_t pkt_count,
					pjmedia_frame packets[],
					unsigned out_size,
					pjmedia_frame *output)
{
    openh264_private *ff = (openh264_private*)codec->codec_data;
    pj_status_t status;

    PJ_ASSERT_RETURN(codec && pkt_count > 0 && packets && output,
                     PJ_EINVAL);

    if (ff->whole) {
		pj_assert(pkt_count==1);
		return openh264_codec_decode_whole(codec, &packets[0], out_size, output);
    } else {
		pjmedia_frame whole_frm;
		unsigned whole_len = 0;
		unsigned i;

		for (i=0; i<pkt_count; ++i) {
			if (whole_len + packets[i].size > ff->dec_buf_size) {
				PJ_LOG(5,(THIS_FILE, "Decoding buffer overflow"));
				break;
			}

			status = openh264_unpacketize(codec, packets[i].buf, packets[i].size,
										ff->dec_buf, ff->dec_buf_size,
										&whole_len);
			if (status != PJ_SUCCESS) {
				PJ_PERROR(5,(THIS_FILE, status, "Unpacketize error"));
				continue;
			}
		}

		whole_frm.buf = ff->dec_buf;
		whole_frm.size = whole_len;
		whole_frm.timestamp = output->timestamp = packets[i].timestamp;
		whole_frm.bit_info = 0;

		return openh264_codec_decode_whole(codec, &whole_frm, out_size, output);
    }
}

#ifdef _MSC_VER
#   pragma comment( lib, "welsdec.lib")
#   pragma comment( lib, "welsenc.lib")
#endif

#endif	/* PJMEDIA_HAS_CISCO_OPENH264 */

