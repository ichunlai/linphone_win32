/*
   mediastreamer2 library - modular sound and video processing and streaming
   Copyright (C) 2006-2010  Belledonne Communications SARL (simon.morlat@linphone.org)

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "mediastreamer2/msvideo.h"
#include "../../Ext/libMemLeakDetection.h"
#if !defined(NO_FFMPEG)
    #include "ffmpeg-priv.h"
#endif

#ifdef WIN32
    #include <malloc.h>
#endif

#ifdef __arm__
    #include "msvideo_neon.h"
#endif

struct _mblk_video_header {
    uint16_t w, h;
    int      pad[3];
};
typedef struct _mblk_video_header mblk_video_header;

static void yuv_buf_init(
    YuvBuf *buf, int w, int h, uint8_t *ptr)
{
    int ysize, usize;
    ysize           = w * h;
    usize           = ysize / 4;
    buf->w          = w;
    buf->h          = h;
    buf->planes[0]  = ptr;
    buf->planes[1]  = buf->planes[0] + ysize;
    buf->planes[2]  = buf->planes[1] + usize;
    buf->planes[3]  = 0;
    buf->strides[0] = w;
    buf->strides[1] = w / 2;
    buf->strides[2] = buf->strides[1];
    buf->strides[3] = 0;
}

int ms_yuv_buf_init_from_mblk(
    YuvBuf *buf, mblk_t *m)
{
    int               w, h;

    // read header
    mblk_video_header *hdr = (mblk_video_header *)m->b_datap->db_base;
    w = hdr->w;
    h = hdr->h;

    if (m->b_cont == NULL)
        yuv_buf_init(buf, w, h, m->b_rptr);
    else
        yuv_buf_init(buf, w, h, m->b_cont->b_rptr);
    return 0;
}

int ms_yuv_buf_init_from_mblk_with_size(
    YuvBuf *buf, mblk_t *m, int w, int h)
{
    if (m->b_cont != NULL) m = m->b_cont; /*skip potential video header */
    yuv_buf_init(buf, w, h, m->b_rptr);
    return 0;
}

int ms_picture_init_from_mblk_with_size(
    MSPicture *buf, mblk_t *m, MSPixFmt fmt, int w, int h)
{
    if (m->b_cont != NULL) m = m->b_cont; /*skip potential video header */
    switch (fmt)
    {
    case MS_YUV420P:
        return ms_yuv_buf_init_from_mblk_with_size(buf, m, w, h);
        break;
    case MS_YUY2:
    case MS_YUYV:
    case MS_UYVY:
        memset(buf, 0, sizeof(*buf));
        buf->w          = w;
        buf->h          = h;
        buf->planes[0]  = m->b_rptr;
        buf->strides[0] = w * 2;
        break;
    case MS_RGB24:
    case MS_RGB24_REV:
        memset(buf, 0, sizeof(*buf));
        buf->w          = w;
        buf->h          = h;
        buf->planes[0]  = m->b_rptr;
        buf->strides[0] = w * 3;
        break;
    default:
        ms_fatal("FIXME: unsupported format %i", fmt);
        return -1;
    }
    return 0;
}

mblk_t *ms_yuv_buf_alloc(
    YuvBuf *buf, int w, int h)
{
    int       size    = (w * h * 3) / 2;
    const int         header_size = sizeof(mblk_video_header);
    const int padding = 16;
    mblk_t            *msg        = allocb(header_size + size + padding, 0);
    // write width/height in header
    mblk_video_header *hdr        = (mblk_video_header *)msg->b_wptr;
    hdr->w       = w;
    hdr->h       = h;
    msg->b_rptr += header_size;
    msg->b_wptr += header_size;
    yuv_buf_init(buf, w, h, msg->b_wptr);
    msg->b_wptr += size;
    return msg;
}

mblk_t *ms_yuv_buf_alloc_from_buffer(
    int w, int h, mblk_t *buffer)
{
    const int         header_size = sizeof(mblk_video_header);
    mblk_t            *msg        = allocb(header_size, 0);
    // write width/height in header
    mblk_video_header *hdr        = (mblk_video_header *)msg->b_wptr;
    hdr->w       = w;
    hdr->h       = h;
    msg->b_rptr += header_size;
    msg->b_wptr += header_size;
    // append real image buffer
    msg->b_cont  = buffer;
    return msg;
}

static void plane_copy(
    const uint8_t *src_plane, int src_stride,
    uint8_t *dst_plane, int dst_stride, MSVideoSize roi)
{
    int i;
    if ((roi.width == src_stride) && (roi.width == dst_stride))
    {
        memcpy(dst_plane, src_plane, roi.width * roi.height);
        return;
    }
    for (i = 0; i < roi.height; ++i)
    {
        memcpy(dst_plane, src_plane, roi.width);
        src_plane += src_stride;
        dst_plane += dst_stride;
    }
}

void ms_yuv_buf_copy(
    uint8_t *src_planes[], const int src_strides[],
    uint8_t *dst_planes[], const int dst_strides[3], MSVideoSize roi)
{
    plane_copy(src_planes[0], src_strides[0], dst_planes[0], dst_strides[0], roi);
    roi.width  = roi.width / 2;
    roi.height = roi.height / 2;
    plane_copy(src_planes[1], src_strides[1], dst_planes[1], dst_strides[1], roi);
    plane_copy(src_planes[2], src_strides[2], dst_planes[2], dst_strides[2], roi);
}

static void plane_horizontal_mirror(
    uint8_t *p, int linesize, int w, int h)
{
    int     i, j;
    uint8_t tmp;
    for (j = 0; j < h; ++j)
    {
        for (i = 0; i < w / 2; ++i)
        {
            const int idx_target_pixel = w - 1 - i;
            tmp                 = p[i];
            p[i]                = p[idx_target_pixel];
            p[idx_target_pixel] = tmp;
        }
        p += linesize;
    }
}

static void plane_central_mirror(
    uint8_t *p, int linesize, int w, int h)
{
    int     i, j;
    uint8_t tmp;
    uint8_t *end_of_image = p + (h - 1) * linesize + w - 1;
    uint8_t *image_center = p + (h / 2) * linesize + w / 2;
    for (j = 0; j < h / 2; ++j)
    {
        for (i = 0; i < w && p < image_center; ++i)
        {
            tmp           = *p;
            *p            = *end_of_image;
            *end_of_image = tmp;
            ++p;
            --end_of_image;
        }
        p            += linesize - w;
        end_of_image -= linesize - w;
    }
}

static void plane_vertical_mirror(
    uint8_t *p, int linesize, int w, int h)
{
    int     j;
    uint8_t *tmp         = (uint8_t *)alloca(w * sizeof(int));
    uint8_t *bottom_line = p + (h - 1) * linesize;
    for (j = 0; j < h / 2; ++j)
    {
        memcpy(tmp,         p,           w);
        memcpy(p,           bottom_line, w);
        memcpy(bottom_line, tmp,         w);
        p           += linesize;
        bottom_line -= linesize;
    }
}

static void plane_mirror(
    MSMirrorType type, uint8_t *p, int linesize, int w, int h)
{
    switch (type)
    {
    case MS_HORIZONTAL_MIRROR:
        plane_horizontal_mirror(p, linesize, w, h);
        break;
    case MS_VERTICAL_MIRROR:
        plane_vertical_mirror(p, linesize, w, h);
        break;
    case MS_CENTRAL_MIRROR:
        plane_central_mirror(p, linesize, w, h);
        break;
    case MS_NO_MIRROR:
        break;
    }
}

/*in place horizontal mirroring*/
void ms_yuv_buf_mirror(
    YuvBuf *buf)
{
    ms_yuv_buf_mirrors(buf, MS_HORIZONTAL_MIRROR);
}

/*in place mirroring*/
void ms_yuv_buf_mirrors(
    YuvBuf *buf, MSMirrorType type)
{
    plane_mirror(type, buf->planes[0], buf->strides[0], buf->w,     buf->h);
    plane_mirror(type, buf->planes[1], buf->strides[1], buf->w / 2, buf->h / 2);
    plane_mirror(type, buf->planes[2], buf->strides[2], buf->w / 2, buf->h / 2);
}

#ifndef MAKEFOURCC
    #define MAKEFOURCC(a, b, c, d) ((d) << 24 | (c) << 16 | (b) << 8 | (a))
#endif

MSPixFmt ms_fourcc_to_pix_fmt(
    uint32_t fourcc)
{
    MSPixFmt ret;
    switch (fourcc)
    {
    case MAKEFOURCC('I', '4', '2', '0'):
        ret = MS_YUV420P;
        break;
    case MAKEFOURCC('Y', 'U', 'Y', '2'):
        ret = MS_YUY2;
        break;
    case MAKEFOURCC('Y', 'U', 'Y', 'V'):
        ret = MS_YUYV;
        break;
    case MAKEFOURCC('U', 'Y', 'V', 'Y'):
        ret = MS_UYVY;
        break;
    case MAKEFOURCC('M', 'J', 'P', 'G'):
        ret = MS_MJPEG;
        break;
    case 0:     /*BI_RGB on windows*/
        ret = MS_RGB24;
        break;
    default:
        ret = MS_PIX_FMT_UNKNOWN;
    }
    return ret;
}

void rgb24_mirror(
    uint8_t *buf, int w, int h, int linesize)
{
    int i, j;
    int r, g, b;
    int end = w * 3;
    for (i = 0; i < h; ++i)
    {
        for (j = 0; j < end / 2; j += 3)
        {
            r                = buf[j];
            g                = buf[j + 1];
            b                = buf[j + 2];
            buf[j]           = buf[end - j - 3];
            buf[j + 1]       = buf[end - j - 2];
            buf[j + 2]       = buf[end - j - 1];
            buf[end - j - 3] = r;
            buf[end - j - 2] = g;
            buf[end - j - 1] = b;
        }
        buf += linesize;
    }
}

void rgb24_revert(
    uint8_t *buf, int w, int h, int linesize)
{
    uint8_t *p, *pe;
    int     i, j;
    uint8_t *end = buf + ((h - 1) * linesize);
    uint8_t exch;
    p  = buf;
    pe = end - 1;
    for (i = 0; i < h / 2; ++i)
    {
        for (j = 0; j < w * 3; ++j)
        {
            exch   = p[i];
            p[i]   = pe[-i];
            pe[-i] = exch;
        }
        p  += linesize;
        pe -= linesize;
    }
}

void rgb24_copy_revert(
    uint8_t *dstbuf, int dstlsz,
    const uint8_t *srcbuf, int srclsz, MSVideoSize roi)
{
    int           i, j;
    const uint8_t *psrc;
    uint8_t       *pdst;
    psrc = srcbuf;
    pdst = dstbuf + (dstlsz * (roi.height - 1));
    for (i = 0; i < roi.height; ++i)
    {
        for (j = 0; j < roi.width * 3; ++j)
        {
            pdst[(roi.width * 3) - 1 - j] = psrc[j];
        }
        pdst -= dstlsz;
        psrc += srclsz;
    }
}

static MSVideoSize _ordered_vsizes[] = {
    {MS_VIDEO_SIZE_QCIF_W, MS_VIDEO_SIZE_QCIF_H},
    {MS_VIDEO_SIZE_QVGA_W, MS_VIDEO_SIZE_QVGA_H},
    {MS_VIDEO_SIZE_CIF_W,  MS_VIDEO_SIZE_CIF_H },
    {MS_VIDEO_SIZE_VGA_W,  MS_VIDEO_SIZE_VGA_H },
    {MS_VIDEO_SIZE_4CIF_W, MS_VIDEO_SIZE_4CIF_H},
    {MS_VIDEO_SIZE_720P_W, MS_VIDEO_SIZE_720P_H},
    {                   0,                    0}
};

MSVideoSize ms_video_size_get_just_lower_than(
    MSVideoSize vs)
{
    MSVideoSize *p;
    MSVideoSize ret;
    ret.width  = 0;
    ret.height = 0;
    for (p = _ordered_vsizes; p->width != 0; ++p)
    {
        if (ms_video_size_greater_than(vs, *p) && !ms_video_size_equal(vs, *p))
        {
            ret = *p;
        }
        else return ret;
    }
    return ret;
}

void ms_rgb_to_yuv(
    const uint8_t rgb[3], uint8_t yuv[3])
{
    yuv[0] = (uint8_t)(0.257 * rgb[0] + 0.504 * rgb[1] + 0.098 * rgb[2] + 16);
    yuv[1] = (uint8_t)(-0.148 * rgb[0] - 0.291 * rgb[1] + 0.439 * rgb[2] + 128);
    yuv[2] = (uint8_t)(0.439 * rgb[0] - 0.368 * rgb[1] - 0.071 * rgb[2] + 128);
}

#if !defined(NO_FFMPEG)

int ms_pix_fmt_to_ffmpeg(
    MSPixFmt fmt)
{
    switch (fmt)
    {
    case MS_RGBA32:
        return PIX_FMT_RGBA;
    case MS_RGB24:
        return PIX_FMT_RGB24;
    case MS_RGB24_REV:
        return PIX_FMT_BGR24;
    case MS_YUV420P:
        return PIX_FMT_YUV420P;
    case MS_YUYV:
        return PIX_FMT_YUYV422;
    case MS_UYVY:
        return PIX_FMT_UYVY422;
    case MS_YUY2:
        return PIX_FMT_YUYV422;       /* <- same as MS_YUYV */
    case MS_RGB565:
        return PIX_FMT_RGB565;
    default:
        ms_fatal("format not supported.");
        return -1;
    }
    return -1;
}

MSPixFmt ffmpeg_pix_fmt_to_ms(
    int fmt)
{
    switch (fmt)
    {
    case PIX_FMT_RGB24:
        return MS_RGB24;
    case PIX_FMT_BGR24:
        return MS_RGB24_REV;
    case PIX_FMT_YUV420P:
        return MS_YUV420P;
    case PIX_FMT_YUYV422:
        return MS_YUYV;         /* same as MS_YUY2 */
    case PIX_FMT_UYVY422:
        return MS_UYVY;
    case PIX_FMT_RGBA:
        return MS_RGBA32;
    case PIX_FMT_RGB565:
        return MS_RGB565;
    default:
        ms_fatal("format not supported.");
        return MS_YUV420P; /* default */
    }
    return MS_YUV420P;     /* default */
}

struct _MSFFScalerContext {
    struct SwsContext *ctx;
    int               src_h;
};

struct ms_swscaleDesc ms_swscale_desc = {
    NULL,
    NULL,
    NULL,
    NULL
};

struct ms_SwsContext  *ms_sws_getContext(
    int srcW, int srcH, int srcFormat,
    int dstW, int dstH, int dstFormat,
    int flags, struct _SwsFilter *srcFilter,
    struct _SwsFilter *dstFilter, double *param)
{
    if (ms_swscale_desc.sws_getContext == NULL)
    {
        ms_swscale_desc.sws_getContext  = (sws_getContextFunc)sws_getContext;
        ms_swscale_desc.sws_freeContext = (sws_freeContextFunc)sws_freeContext;
        ms_swscale_desc.sws_scale       = (sws_scaleFunc)sws_scale;
    }
    return (struct ms_SwsContext *)ms_swscale_desc.sws_getContext(srcW, srcH, srcFormat, dstW, dstH, dstFormat,
                                                                  flags, srcFilter, dstFilter, param);
}

void ms_sws_freeContext(
    struct ms_SwsContext *swsContext)
{
    ms_swscale_desc.sws_freeContext(swsContext);
}

int ms_sws_scale(
    struct ms_SwsContext *context, uint8_t *srcSlice[], int srcStride[],
    int srcSliceY, int srcSliceH, uint8_t *dst[], int dstStride[])
{
    return ms_swscale_desc.sws_scale(context, srcSlice, srcStride, srcSliceY, srcSliceH, dst, dstStride);
}


void ms_video_set_video_func(
    struct ms_swscaleDesc *_ms_swscale_desc)
{
    ms_swscale_desc.sws_getContext  = _ms_swscale_desc->sws_getContext;
    ms_swscale_desc.sws_freeContext = _ms_swscale_desc->sws_freeContext;
    ms_swscale_desc.sws_scale       = _ms_swscale_desc->sws_scale;
    ms_swscale_desc.yuv_buf_mirror  = _ms_swscale_desc->yuv_buf_mirror;
}

#endif