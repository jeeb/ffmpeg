/*
 * AV1 decoding utilizing the libdav1d library.
 *
 * Copyright (C) 2018
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "avcodec.h"
#include "libavutil/imgutils.h"
#include <dav1d/dav1d.h>

typedef struct Dav1dDecContext {
    void *decoder;


    AVFrame *frame;
    Dav1dContext *dec_ctx;
    Dav1dSettings dec_settings;
} Dav1dDecContext;

static int dav1d_to_avframe_params(AVCodecContext *avctx, AVFrame *frame, Dav1dPicture *picture)
{
    Dav1dPictureParameters params = picture->p;

    switch (params.layout) {
    case DAV1D_PIXEL_LAYOUT_I400:
        if (params.bpc == 8)
            frame->format = AV_PIX_FMT_GRAY8;
        else if (params.bpc == 10)
            frame->format = AV_PIX_FMT_GRAY10;
        else if (params.bpc == 12)
            frame->format = AV_PIX_FMT_GRAY12;
        else
            return -1;
        break;
    case DAV1D_PIXEL_LAYOUT_I420:
        if (params.bpc == 8)
            frame->format = AV_PIX_FMT_YUV420P;
        else if (params.bpc == 10)
            frame->format = AV_PIX_FMT_YUV420P10;
        else if (params.bpc == 12)
            frame->format = AV_PIX_FMT_YUV420P12;
        else
            return -1;
        break;
    case DAV1D_PIXEL_LAYOUT_I422:
        if (params.bpc == 8)
            frame->format = AV_PIX_FMT_YUV422P;
        else if (params.bpc == 10)
            frame->format = AV_PIX_FMT_YUV422P10;
        else if (params.bpc == 12)
            frame->format = AV_PIX_FMT_YUV422P12;
        else
            return -1;
        break;
    case DAV1D_PIXEL_LAYOUT_I444:
        if (params.bpc == 8)
            frame->format = params.trc == DAV1D_TRC_SRGB ? AV_PIX_FMT_GBRP : AV_PIX_FMT_YUV444P;
        else if (params.bpc == 10)
            frame->format = params.trc == DAV1D_TRC_SRGB ? AV_PIX_FMT_GBRP10 : AV_PIX_FMT_YUV444P10;
        else if (params.bpc == 12)
            frame->format = params.trc == DAV1D_TRC_SRGB ? AV_PIX_FMT_GBRP12 : AV_PIX_FMT_YUV444P12;
        else
            return -1;

        break;
    };

    avctx->pix_fmt = frame->format;

    frame->width  = params.w;
    frame->height = params.h;

    avctx->color_range = frame->color_range = params.fullrange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

    return 0;
}

static av_cold int libdav1d_init(AVCodecContext *avctx)
{
    Dav1dDecContext *ctx = avctx->priv_data;
    int ret = -1;

    dav1d_init();

    ctx->dec_settings.n_frame_threads = 2;
    ctx->dec_settings.n_tile_threads = 2;

    if ((ret = dav1d_open(&ctx->dec_ctx, &ctx->dec_settings)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to open dav1d decoder (err=%d)\n", ret);
        return AVERROR_EXTERNAL;
    }

    return 0;
}

static int libdav1d_decode(AVCodecContext *avctx, void *data,
                           int *got_frame, AVPacket *avpkt)
{
    uint8_t *planes[4];
    int linesizes[4];

    /*
    typedef struct Dav1dData {
        uint8_t *data; ///< data pointer
        size_t sz; ///< data size
        struct Dav1dRef *ref; ///< allocation origin
    } Dav1dData;
    */

    // dav1d_data_create(Dav1dData *const buf, const size_t sz)
    Dav1dData data_pkt;
    Dav1dPicture *picture;
    AVFrame *frame = data;
    int ret = -1;

    Dav1dDecContext *ctx = avctx->priv_data;
    if ((ret = dav1d_data_create(&data_pkt, avpkt->size)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failure in creating packet (ret=%d)\n", ret);
        return AVERROR_EOF;
    }

    picture = av_mallocz(sizeof(Dav1dPicture));

    memcpy(data_pkt.data, avpkt->data, avpkt->size);

    // dav1d_decode(Dav1dContext *c, Dav1dData *in, Dav1dPicture *out)
    ret = dav1d_decode(ctx->dec_ctx, &data_pkt, picture);
    if (ret == -EAGAIN)
        return AVERROR(EAGAIN);
    else if (ret < 0) {
        return AVERROR_EXTERNAL;
    }

    ret = dav1d_to_avframe_params(avctx, frame, picture);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failure in converting dav1d params to AVFrame params (ret=%d)\n", ret);
        return AVERROR_INVALIDDATA;
    }

    if ((ret = av_frame_get_buffer(frame, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failure in allocating AVFrame buffers (ret=%d)\n", ret);
        return AVERROR(ENOMEM);
    }

    planes[0] = picture->data[0];
    planes[1] = picture->data[1];
    planes[2] = picture->data[2];
    planes[3] = NULL;
    linesizes[0] = picture->stride[0];
    linesizes[1] = picture->stride[1];
    linesizes[2] = picture->stride[1];
    linesizes[3] = 0;

    av_image_copy(frame->data, frame->linesize, (const uint8_t**)planes,
                  linesizes, frame->format, frame->width, frame->height);
    *got_frame           = 1;

    return avpkt->size;
}

static av_cold int libdav1d_close(AVCodecContext *avctx)
{
    Dav1dDecContext *ctx = avctx->priv_data;

    dav1d_close(ctx->dec_ctx);

    return 0;
}

AVCodec ff_libdav1d_decoder = {
    .name           = "libdav1d",
    .long_name      = NULL_IF_CONFIG_SMALL("libdav1d AV1 decoder"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_AV1,
    .priv_data_size = sizeof(Dav1dDecContext),
    .init           = libdav1d_init,
    .close          = libdav1d_close,
    .decode         = libdav1d_decode,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10,
                                                     AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV422P10,
                                                     AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10,
                                                     AV_PIX_FMT_GRAY8,   AV_PIX_FMT_GRAY10,
                                                     AV_PIX_FMT_NONE },
    .wrapper_name   = "libdav1d",
};
