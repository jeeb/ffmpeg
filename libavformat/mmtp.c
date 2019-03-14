/*
 * MMTP/TLV demuxer
 * Copyright (c) 2019 Jan Ekström
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

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "internal.h"

enum TLVSyncBytes {
    TLV_SYNC_BYTE = 0x7F,
};

static int tlv_resync(AVFormatContext *ctx)
{
    AVIOContext *pb = ctx->pb;
    unsigned int resync_limit = 10*1024*1024;

    for (unsigned int i = 0; i < resync_limit; i++)
    {
        int byte = avio_r8(pb);
        if (avio_feof(pb))
            return AVERROR_EOF;

        if (byte == TLV_SYNC_BYTE) {
            avio_seek(pb, -1, SEEK_CUR);
            return 0;
        }
    }

    av_log(ctx, AV_LOG_ERROR, "TLV resync failed to find TLV sync byte!\n");
    return AVERROR_INVALIDDATA;
}

static int tlv_read_packet(AVFormatContext *ctx)
{
    AVIOContext *pb = ctx->pb;
    unsigned char tlv_header[4 + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    unsigned char packet_type = 0;
    uint16_t packet_length = 0;
    int len = avio_read(pb, tlv_header, 4);
    if (len != 4)
        return len < 0 ? len : AVERROR_EOF;

    if (tlv_header[0] != TLV_SYNC_BYTE) {
        av_log(ctx, AV_LOG_ERROR, "TLV packet sync byte is wrong!\n");
        return AVERROR_INVALIDDATA;
    }

    packet_type = tlv_header[1];

    switch (packet_type) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0xFE:
    case 0xFF:
        break;
    default:
        {
            av_log(ctx, AV_LOG_ERROR, "Unknown TLV packet type: %"PRIu8"\n",
                   packet_type);
            return AVERROR_INVALIDDATA;
        }
    }

    packet_length = AV_RB16(tlv_header + 2);

    av_log(ctx, AV_LOG_VERBOSE, "TLV packet of type %"PRIu8" and size %"PRIu16" found\n",
           packet_type, packet_length);

    return 0;
}

static int mmtp_tlv_probe(AVProbeData *p)
{
  return 0;
}

static int mmtp_tlv_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret = tlv_resync(ctx);
    if (ret < 0)
        return ret;

    return 0;
}

AVInputFormat ff_mmtp_demuxer = {
    .name           = "mmtp",
    .long_name      = NULL_IF_CONFIG_SMALL("MMTP over TLV"),
    .extensions     = "mmts,tlvmmt",
    .read_probe     = mmtp_tlv_probe,
    .read_packet    = mmtp_tlv_read_packet,
};