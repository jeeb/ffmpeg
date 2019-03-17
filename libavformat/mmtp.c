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

enum TLVTableType {
    TLV_TABLE_NIT_ACTUAL = 0x40,
    TLV_TABLE_NIT_OTHER  = 0x41,
    TLV_TABLE_EXTENDED   = 0xFE, //< table id extension signals the table
};

enum TLVTableExtensionType {
    TLV_TABLE_EXTENDED_AMT = 0x0000,
};

struct TLVSignallingPacket {
    GetBitContext *gb;
    // common parts
    uint8_t table_id;
    uint8_t section_syntax_indicator;
    uint16_t section_length;

    // values re-used by the sub-structures
    uint16_t table_id_extension;
    uint8_t version_number;
    uint8_t current_next_indicator;
    uint8_t section_number;
    uint8_t last_section_number;
};

enum TLVAMTMappingType {
    TLV_AMT_IPV4 = 0,
    TLV_AMT_IPV6 = 1,
};

struct TLVAMTMapping {
    uint16_t service_id;
    enum TLVAMTMappingType type;

    // common
    uint8_t src_address_mask;
    uint8_t dst_address_mask;

    // if ipv4
    uint8_t ipv4_src_address[4];
    uint8_t ipv4_dst_address[4];

    // if ipv6
    uint8_t ipv6_src_address[16];
    uint8_t ipv6_dst_address[16];
};

static int tlv_parse_hcfb_packet(AVFormatContext *ctx, struct TLVPacket *pkt)
{
    GetBitContext gb = { 0 };
    if (pkt->pkt_type != TLV_PACKET_IP_HEADER_COMPRESSED ||
        pkt->pkt_data_size < 3)
        return AVERROR_INVALIDDATA;

    if (init_get_bits8(&gb, pkt->pkt_data, pkt->pkt_data_size) < 0) {
        return AVERROR_INVALIDDATA;
    }

    uint16_t cid             = get_bits(&gb, 12);
    uint8_t  sn              = get_bits(&gb, 4);
    uint8_t  cid_header_type = get_bits(&gb, 8);

    av_log(ctx, AV_LOG_VERBOSE, "HCfB packet with cid: %"PRIu16", "
                                "sn: %"PRIu8", cid header type: 0x%"PRIx8"\n",
           cid, sn, cid_header_type);

    return 0;
}

static int tlv_parse_nit_packet(AVFormatContext *ctx, struct TLVSignallingPacket *pkt)
{
    // 13 comes from:
    // - 5 bytes for the common signalling structure that we have passed
    // - 2 bytes for this length structure (4+12 bits)
    // - 2 bytes for a similar thing for TLV_stream_loop_length (4+12 bits)
    // - 4 bytes for the CRC32 at the end
    uint32_t minimum_required_length = 13;
    uint16_t network_id = 0;
    uint16_t network_descriptors_length = 0;
    uint16_t tlv_stream_loop_length = 0;
    // for TLV-NIT this is a 10 bit field in 12 bits
    pkt->section_length = (pkt->section_length & 0x3ff);

    if (!pkt->section_syntax_indicator || pkt->section_length > 1021 ||
        pkt->section_length < minimum_required_length)
        return AVERROR_INVALIDDATA;

    network_id = pkt->table_id_extension;

    skip_bits(pkt->gb, 4);

    // another 10 bit field in 12 bits
    network_descriptors_length = (get_bits(pkt->gb, 12) & 0x3ff);

    if (pkt->section_length < minimum_required_length ||
        network_descriptors_length > (pkt->section_length - minimum_required_length))
        return AVERROR_INVALIDDATA;

    // update minimum required length with
    // how long the network descriptors were
    minimum_required_length += network_descriptors_length;

    // TODO: handle network descriptors
    if (network_descriptors_length)
        skip_bits_long(pkt->gb, network_descriptors_length * 8);

    skip_bits(pkt->gb, 4);

    // another 10 bit field in 12 bits
    tlv_stream_loop_length = (get_bits(pkt->gb, 12) & 0x3ff);

    if (pkt->section_length < minimum_required_length ||
        tlv_stream_loop_length > (pkt->section_length - minimum_required_length))
        return AVERROR_INVALIDDATA;

    av_log(ctx, AV_LOG_VERBOSE, "NIT packet for network_id: %"PRIu16". "
           "Size: %"PRIu16", network_descriptors_length: %"PRIu16", "
           "TLV_stream_loop_length: %"PRIu16"\n",
           network_id, pkt->section_length, network_descriptors_length,
           tlv_stream_loop_length);

    for (unsigned int left_length = tlv_stream_loop_length; left_length >= 6;) {
        uint16_t tlv_stream_id = get_bits(pkt->gb, 16);
        uint16_t original_network_id = get_bits(pkt->gb, 16);
        uint16_t tlv_stream_descriptors_length = 0;

        skip_bits(pkt->gb, 4);

        // another 10 bit field in 12 bits
        tlv_stream_descriptors_length = (get_bits(pkt->gb, 12) & 0x3ff);

        av_log(ctx, AV_LOG_VERBOSE, "TLV Stream ID %"PRIu16": "
                                    "original_network_id: %"PRIu16", "
                                    "descriptors_length: %"PRIu16"\n",
               tlv_stream_id, original_network_id,
               tlv_stream_descriptors_length);

        if (left_length < (6 + tlv_stream_descriptors_length))
            return AVERROR_INVALIDDATA;

        // TODO: handle TLV stream descriptors
        if (tlv_stream_descriptors_length)
            skip_bits(pkt->gb, tlv_stream_descriptors_length * 8);

        left_length -= (6 + tlv_stream_descriptors_length);
    }

    return 0;
}

static int tlv_parse_amt_packet(AVFormatContext *ctx, struct TLVSignallingPacket *pkt)
{
    // 11 comes from:
    // - 5 bytes for the common signalling structure that we have passed
    // - 2 bytes for the service_id counter
    // - 4 bytes for the CRC32 at the end
    uint32_t minimum_required_length = 11;
    uint16_t num_of_service_id = 0;

    if (!pkt->section_syntax_indicator ||
        pkt->section_length < minimum_required_length)
        return AVERROR_INVALIDDATA;

    num_of_service_id = get_bits(pkt->gb, 10);
    skip_bits(pkt->gb, 6);

    av_log(ctx, AV_LOG_VERBOSE, "TLV AMT found with %"PRIu16" service IDs\n",
           num_of_service_id);

    for (unsigned int i = 0; i < num_of_service_id; i++) {
        uint16_t service_id = 0;
        uint8_t ip_version = 0;
        uint16_t service_loop_length = 0;
        unsigned int minimum_address_part_length = 0;
        struct TLVAMTMapping amt_mapping = { 0 };
        const uint8_t *buff_location = NULL;

        minimum_required_length += 4;
        if (pkt->section_length < minimum_required_length)
            return AVERROR_INVALIDDATA;

        service_id = get_bits(pkt->gb, 16);
        ip_version = get_bits(pkt->gb, 1);
        skip_bits(pkt->gb, 5);
        service_loop_length = get_bits(pkt->gb, 10);

        av_log(ctx, AV_LOG_VERBOSE, "Service ID %"PRIu16": "
                                    "ip_version: %s\n",
               service_id, ip_version ? "ipv6": "ipv4");

        minimum_required_length += service_loop_length;
        // (128+128+8+8) / 8 => 34 for ipv6
        // (32+32+8+8) / 8 => 10 for ipv4
        minimum_address_part_length = ip_version ? 34 : 10;
        if (pkt->section_length < minimum_required_length ||
            service_loop_length < minimum_address_part_length)
            return AVERROR_INVALIDDATA;

        amt_mapping.type = ip_version ? TLV_AMT_IPV6 : TLV_AMT_IPV4;
        buff_location = (pkt->gb->buffer + (get_bits_count(pkt->gb) / 8));

        if (ip_version) {
            AV_COPY128U(amt_mapping.ipv6_src_address, buff_location);
            buff_location += 16;
            amt_mapping.src_address_mask = buff_location[0];
            buff_location += 1;
            AV_COPY128U(amt_mapping.ipv6_dst_address, buff_location);
            buff_location += 16;
            amt_mapping.dst_address_mask = buff_location[0];

            av_log(ctx, AV_LOG_VERBOSE, "FFFUU ipv6 - "
                   "src: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x/%u, "
                   "dst: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x/%u\n",
                   amt_mapping.ipv6_src_address[0],  amt_mapping.ipv6_src_address[1],
                   amt_mapping.ipv6_src_address[2],  amt_mapping.ipv6_src_address[3],
                   amt_mapping.ipv6_src_address[4],  amt_mapping.ipv6_src_address[5],
                   amt_mapping.ipv6_src_address[6],  amt_mapping.ipv6_src_address[7],
                   amt_mapping.ipv6_src_address[8],  amt_mapping.ipv6_src_address[9],
                   amt_mapping.ipv6_src_address[10], amt_mapping.ipv6_src_address[11],
                   amt_mapping.ipv6_src_address[12], amt_mapping.ipv6_src_address[13],
                   amt_mapping.ipv6_src_address[14], amt_mapping.ipv6_src_address[15],
                   amt_mapping.src_address_mask,
                   amt_mapping.ipv6_dst_address[0],  amt_mapping.ipv6_dst_address[1],
                   amt_mapping.ipv6_dst_address[2],  amt_mapping.ipv6_dst_address[3],
                   amt_mapping.ipv6_dst_address[4],  amt_mapping.ipv6_dst_address[5],
                   amt_mapping.ipv6_dst_address[6],  amt_mapping.ipv6_dst_address[7],
                   amt_mapping.ipv6_dst_address[8],  amt_mapping.ipv6_dst_address[9],
                   amt_mapping.ipv6_dst_address[10], amt_mapping.ipv6_dst_address[11],
                   amt_mapping.ipv6_dst_address[12], amt_mapping.ipv6_dst_address[13],
                   amt_mapping.ipv6_dst_address[14], amt_mapping.ipv6_dst_address[15],
                   amt_mapping.dst_address_mask);
        } else {
            AV_COPY32(amt_mapping.ipv4_src_address, buff_location);
            buff_location += 4;
            amt_mapping.src_address_mask = buff_location[0];
            buff_location += 1;
            AV_COPY32(amt_mapping.ipv4_dst_address, buff_location);
            amt_mapping.dst_address_mask = buff_location[0];

            av_log(ctx, AV_LOG_VERBOSE, "FFFUU ipv4 - "
                   "src: %u.%u.%u.%u/%u, dst: %u.%u.%u.%u/%u\n",
                   amt_mapping.ipv4_src_address[0],  amt_mapping.ipv4_src_address[1],
                   amt_mapping.ipv4_src_address[2],  amt_mapping.ipv4_src_address[3],
                   amt_mapping.src_address_mask,
                   amt_mapping.ipv4_dst_address[0],  amt_mapping.ipv4_dst_address[1],
                   amt_mapping.ipv4_dst_address[2],  amt_mapping.ipv4_dst_address[3],
                   amt_mapping.dst_address_mask);
        }

        skip_bits_long(pkt->gb, service_loop_length * 8);
    }

    return 0;
}

static int tlv_parse_extended_packet(AVFormatContext *ctx, struct TLVSignallingPacket *pkt)
{
    int(* parser_func)(AVFormatContext *ctx, struct TLVSignallingPacket *pkt) = NULL;

    switch (pkt->table_id_extension) {
    case TLV_TABLE_EXTENDED_AMT:
        parser_func = tlv_parse_amt_packet;
        break;
    default:
        {
            av_log(ctx, AV_LOG_ERROR, "Unknown extension identifier: "
                                      "0x%"PRIx16"!\n",
                   pkt->table_id_extension);
            return AVERROR_INVALIDDATA;
        }
    }

    if (parser_func)
        return parser_func(ctx, pkt);

    return 0;
}

static int tlv_parse_signalling_packet(AVFormatContext *ctx, struct TLVPacket *pkt)
{
    struct TLVSignallingPacket sig_pkt = { 0 };
    int(* parser_func)(AVFormatContext *ctx, struct TLVSignallingPacket *pkt) = NULL;
    GetBitContext gb = { 0 };
    if (pkt->pkt_type != TLV_PACKET_SIGNALLING ||
        pkt->pkt_data_size < (8 + 4))
        return AVERROR_INVALIDDATA;

    if (init_get_bits8(&gb, pkt->pkt_data, pkt->pkt_data_size) < 0) {
        return AVERROR_INVALIDDATA;
    }

    sig_pkt.gb = &gb;

    // first three bytes contain the basics and the length is calculated
    // from them on.
    sig_pkt.table_id = get_bits(&gb, 8);
    sig_pkt.section_syntax_indicator = get_bits(&gb, 1);
    skip_bits(&gb, 3);
    sig_pkt.section_length = get_bits(&gb, 12);
    if (sig_pkt.section_length > (pkt->pkt_data_size - 3)) {
        av_log(ctx, AV_LOG_ERROR,
               "A signalling packet of size %"PRIu16" (+ 3) cannot "
               "fit a TLV packet of size %"PRIu16"!\n",
               sig_pkt.section_length, pkt->pkt_data_size);
        return AVERROR_INVALIDDATA;
    }

    switch (sig_pkt.table_id) {
    case TLV_TABLE_NIT_ACTUAL:
    case TLV_TABLE_NIT_OTHER:
        parser_func = tlv_parse_nit_packet;
        break;
    case TLV_TABLE_EXTENDED:
        parser_func = tlv_parse_extended_packet;
        break;
    default:
        {
            av_log(ctx, AV_LOG_ERROR, "Unknown TLV signalling table id: 0x%"PRIx8"\n",
                   sig_pkt.table_id);
            return AVERROR_INVALIDDATA;
        }
    }

    sig_pkt.table_id_extension = get_bits(&gb, 16);
    skip_bits(&gb, 2);

    // if (table_id == TLV_TABLE_EXTENDED &&
    //    table_id_extension == TLV_TABLE_EXTENDED_AMT)
    //    parser_func = tlv_parse_amt_packet;

    sig_pkt.version_number = get_bits(&gb, 5);
    sig_pkt.current_next_indicator = get_bits(&gb, 1);

    sig_pkt.section_number = get_bits(&gb, 8);
    sig_pkt.last_section_number = get_bits(&gb, 8);

    av_log(ctx, AV_LOG_VERBOSE,
           "Signalling packet with table_id: 0x%"PRIx8", %s format, "
           "section_length: %"PRIu16",  table_id_extension: 0x%"PRIx16", "
           "version_number: %"PRIu8", %s in use, "
           "section number: %"PRIu8", last_section_number: %"PRIu8"\n",
           sig_pkt.table_id, sig_pkt.section_syntax_indicator ? "extension" : "normal",
           sig_pkt.section_length, sig_pkt.table_id_extension, sig_pkt.version_number,
           sig_pkt.current_next_indicator ? "currently" : "next",
           sig_pkt.section_number, sig_pkt.last_section_number);

    if (parser_func)
        return parser_func(ctx, &sig_pkt);

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

    av_log(ctx, AV_LOG_VERBOSE, "TLV packet of type 0x%"PRIx8" and size %"PRIu16" found\n",
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