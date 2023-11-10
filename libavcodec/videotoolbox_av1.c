/*
 * Videotoolbox hardware acceleration for AV1
 * Copyright (c) 2023 Jan Ekström
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

#include "libavformat/avio.h"
#include "libavformat/avio_internal.h"
#define CALLED_FROM_AVCODEC 1
#include "libavformat/av1.c"
#undef CALLED_FROM_AVCODEC

#include "libavutil/avutil.h"
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"

#include "av1dec.h"
#include "codec_id.h"
#include "hwaccel_internal.h"
#include "internal.h"
#include "vt_internal.h"

CFDataRef ff_videotoolbox_av1c_extradata_create(AVCodecContext *avctx)
{
    AVIOContext *pb;
    uint8_t *buf;
    CFDataRef data = NULL;
    int buf_size = 0;
    int ret = avio_open_dyn_buf(&pb);
    if (ret < 0)
        return NULL;

    ret = ff_isom_write_av1c(pb, avctx->extradata, avctx->extradata_size, 1);
    if (ret < 0)
        goto fail;

    buf_size = avio_get_dyn_buf(pb, &buf);
    if (buf_size)
        data = CFDataCreate(kCFAllocatorDefault, buf, buf_size);

fail:
    ffio_free_dyn_buf(&pb);

    return data;
}

static int videotoolbox_av1_start_frame(AVCodecContext *avctx,
                                        const uint8_t *buffer,
                                        uint32_t size)
{
    return 0;
}

static int videotoolbox_av1_decode_slice(AVCodecContext *avctx,
                                         const uint8_t *buffer,
                                         uint32_t size)
{
    VTContext *vtctx = avctx->internal->hwaccel_priv_data;

    return ff_videotoolbox_buffer_copy(vtctx, buffer, size);
}

static int videotoolbox_av1_end_frame(AVCodecContext *avctx)
{
    const AV1DecContext *s = avctx->priv_data;
    AVFrame *frame = s->cur_frame.f;

    return ff_videotoolbox_common_end_frame(avctx, frame);
}

const FFHWAccel ff_av1_videotoolbox_hwaccel = {
    .p.name         = "av1_videotoolbox",
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AV1,
    .p.pix_fmt      = AV_PIX_FMT_VIDEOTOOLBOX,
    .alloc_frame    = ff_videotoolbox_alloc_frame,
    .start_frame    = videotoolbox_av1_start_frame,
    .decode_slice   = videotoolbox_av1_decode_slice,
    .end_frame      = videotoolbox_av1_end_frame,
    .frame_params   = ff_videotoolbox_frame_params,
    .init           = ff_videotoolbox_common_init,
    .uninit         = ff_videotoolbox_uninit,
    .priv_data_size = sizeof(VTContext),
};
