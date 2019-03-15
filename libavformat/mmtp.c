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
#include "libavcodec/get_bits.h"

#include "avformat.h"
#include "internal.h"

/* TLV definitions as in ITU-R BT.1869 */

// 0b01 + six reserved bits all set to 1
#define TLV_SYNC_BYTE 0x7F

enum TLVPacketType {
    TLV_PACKET_IPV4                 = 0x01,
    TLV_PACKET_IPV6                 = 0x02,
    TLV_PACKET_IP_HEADER_COMPRESSED = 0x03,
    TLV_PACKET_SIGNALLING           = 0xFE,
    TLV_PACKET_NULL                 = 0xFF,
};

struct TLVPacket {
    enum TLVPacketType  pkt_type;
    uint16_t            pkt_data_size;
    uint8_t            *pkt_data;
};

enum TLVHCfBPacketType {
    HCFB_FULL_HEADER_IPV4_AND_UDP = 0x20,
    HCFB_COMP_HEADER_IPV4_AND_UDP = 0x21,
    HCFB_FULL_HEADER_IPV6_AND_UDP = 0x60,
    HCFB_COMP_HEADER_IPV6_AND_UDP = 0x61,
};

static int tlv_parse_hcfb_packet(AVFormatContext *ctx, struct TLVPacket *pkt)
{
    if (pkt->pkt_type != TLV_PACKET_IP_HEADER_COMPRESSED ||
        pkt->pkt_data_size < 3)
        return AVERROR_INVALIDDATA;

    uint16_t cid_and_sn = AV_RB16(pkt->pkt_data);
    uint8_t  cid_header_type = pkt->pkt_data[2];

    uint16_t cid = (cid_and_sn & 0xfff0) >> 4;
    uint8_t  sn  = cid_and_sn & 0x0f;

    av_log(ctx, AV_LOG_VERBOSE, "HCfB packet with cid: %"PRIu16", "
                                "sn: %"PRIu8", cid header type: 0x%"PRIx8"\n",
           cid, sn, cid_header_type);

    return 0;
}

static int tlv_parse_signalling_packet(AVFormatContext *ctx, struct TLVPacket *pkt)
{
    GetBitContext gb = { 0 };
    if (pkt->pkt_type != TLV_PACKET_SIGNALLING ||
        pkt->pkt_data_size < (8 + 4))
        return AVERROR_INVALIDDATA;

    if (init_get_bits8(&gb, pkt->pkt_data, pkt->pkt_data_size) < 0) {
        return AVERROR_INVALIDDATA;
    }

    // first three bytes contain the basics and the length is calculated
    // from them on.
    uint8_t table_id = get_bits(&gb, 8);
    uint8_t section_syntax_indicator = get_bits(&gb, 1);
    skip_bits(&gb, 3);
    uint16_t section_length = get_bits(&gb, 12);
    if (section_length > (pkt->pkt_data_size - 3)) {
        av_log(ctx, AV_LOG_ERROR,
               "A signalling packet of size %"PRIu16" (+ 3) cannot "
               "fit a TLV packet of size %"PRIu16"!\n",
               section_length, pkt->pkt_data_size);
        return AVERROR_INVALIDDATA;
    }

    uint16_t table_id_extension = get_bits(&gb, 16);
    skip_bits(&gb, 2);

    uint8_t version_number = get_bits(&gb, 5);
    uint8_t current_next_indicator = get_bits(&gb, 1);

    uint8_t section_number = get_bits(&gb, 8);
    uint8_t last_section_number = get_bits(&gb, 8);

    av_log(ctx, AV_LOG_VERBOSE,
           "Signalling packet with table_id: 0x%"PRIx8", section_syntax: %s, "
           "section_length: %"PRIu16", table_id_extension: %"PRIu16", "
           "version_number: %"PRIu8", %s in use, "
           "last_section_number: %"PRIu8"\n",
           table_id, section_syntax_indicator ? "yes" : "no",
           section_length, table_id_extension, version_number,
           current_next_indicator ? "currently" : "next",
           last_section_number);

    return 0;
}

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
    int ret = -1;
    uint16_t packet_length = 0;
    struct TLVPacket *tlv_packet = NULL;
    int(* parser_func)(AVFormatContext *ctx, struct TLVPacket *pkt) = NULL;
    int len = avio_read(pb, tlv_header, 4);
    if (len != 4)
        return len < 0 ? len : AVERROR_EOF;

    if (tlv_header[0] != TLV_SYNC_BYTE) {
        av_log(ctx, AV_LOG_ERROR, "TLV packet sync byte is wrong!\n");
        return AVERROR_INVALIDDATA;
    }

    packet_type = tlv_header[1];

    switch (packet_type) {
    case TLV_PACKET_IP_HEADER_COMPRESSED:
        parser_func = tlv_parse_hcfb_packet;
        break;
    case TLV_PACKET_SIGNALLING:
        parser_func = tlv_parse_signalling_packet;
        break;
    case TLV_PACKET_IPV4:
    case TLV_PACKET_IPV6:
    case TLV_PACKET_NULL:
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

    if (!packet_length)
        return 0;

    // No parser implemented yet, skip packet size
    if (!parser_func) {
        av_log(ctx, AV_LOG_VERBOSE, "Skipping packet...\n");
        int64_t curr_pos      = avio_tell(pb);
        int64_t post_skip_pos = avio_skip(pb, packet_length);
        if (post_skip_pos != (packet_length + curr_pos)){
            av_log(ctx, AV_LOG_ERROR, "Skipping packet failed! %s",
                   av_err2str(post_skip_pos < 0 ? post_skip_pos : AVERROR_EOF));
            return post_skip_pos < 0 ? post_skip_pos : AVERROR_EOF;
        }

        return 0;
    }

    if (!(tlv_packet = av_mallocz(sizeof(struct TLVPacket)))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate TLVPacket!\n");
        return AVERROR(ENOMEM);
    }

    tlv_packet->pkt_type      = packet_type;
    tlv_packet->pkt_data_size = packet_length;
    if (!(tlv_packet->pkt_data =
            av_mallocz(packet_length + AV_INPUT_BUFFER_PADDING_SIZE))) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate TLVPacket buffer!\n");
        ret = AVERROR(ENOMEM);
        goto tlv_packet_read_error;
    }

    len = avio_read(pb, tlv_packet->pkt_data, packet_length);
    if (len != packet_length) {
        ret = len < 0 ? len : AVERROR_EOF;
        goto tlv_packet_read_error;
    }

    // feed the packet to a parser if alles gut
    if ((ret = parser_func(ctx, tlv_packet)) < 0) {
        goto tlv_packet_read_error;
    }

    ret = 0;

tlv_packet_read_error:
    if (tlv_packet) {
        if (tlv_packet->pkt_data)
            av_free(tlv_packet->pkt_data);

        av_free(tlv_packet);
    }

    return ret;
}

static int mmtp_tlv_probe(AVProbeData *data)
{
    if (data->buf_size < 2 ||
        data->buf[0] != TLV_SYNC_BYTE)
        return 0;

    switch (data->buf[1]) {
    case TLV_PACKET_IPV4:
    case TLV_PACKET_IPV6:
    case TLV_PACKET_IP_HEADER_COMPRESSED:
    case TLV_PACKET_SIGNALLING:
    case TLV_PACKET_NULL:
        return AVPROBE_SCORE_MAX;
    default:
        {
            av_log(NULL, AV_LOG_ERROR, "Unknown TLV packet type: %"PRIu8"\n",
                   data->buf[1]);
            return 0;
        }
    }
}

static int mmtp_tlv_read_header(AVFormatContext *ctx)
{
    int ret = -1;
    do {
        ret = tlv_resync(ctx);
        if (ret < 0)
            return ret;

        ret = tlv_read_packet(ctx);
        if (ret < 0)
            return ret;
    } while (ret >= 0);

    return 0;
}

static int mmtp_tlv_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret = tlv_resync(ctx);
    if (ret < 0)
        return ret;

    ret = tlv_read_packet(ctx);
    if (ret < 0)
        return ret;

    return 0;
}

AVInputFormat ff_mmtp_demuxer = {
    .name           = "mmtp",
    .long_name      = NULL_IF_CONFIG_SMALL("MMTP over TLV"),
    .extensions     = "mmts,tlvmmt",
    .read_probe     = mmtp_tlv_probe,
    .read_header    = mmtp_tlv_read_header,
    .read_packet    = mmtp_tlv_read_packet,
};