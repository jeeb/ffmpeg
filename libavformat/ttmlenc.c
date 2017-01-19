/*
 * TTML subtitle muxer
 * Copyright (c) 2020 24i
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

/**
 * @file
 * TTML subtitle muxer
 * @see https://www.w3.org/TR/ttml1/
 * @see https://www.w3.org/TR/ttml2/
 * @see https://www.w3.org/TR/ttml-imsc/rec
 */

#include "avformat.h"
#include "internal.h"

static const char ttml_header_text[] =
"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
"<tt\n"
"  xmlns=\"http://www.w3.org/ns/ttml\"\n"
"  xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\"\n"
"  xmlns:tts=\"http://www.w3.org/ns/ttml#styling\"\n"
"  xml:lang=\"%s\">\n"
"  <body>\n"
"    <div>\n";

static const char ttml_footer_text[] =
"    </div>\n"
"  </body>\n"
"</tt>\n";

static void ttml_write_time(AVIOContext *pb, const char tag[],
                            int64_t millisec)
{
    int64_t sec, min, hour;
    sec = millisec / 1000;
    millisec -= 1000 * sec;
    min = sec / 60;
    sec -= 60 * min;
    hour = min / 60;
    min -= 60 * hour;

    avio_printf(pb, "%s=\"%02"PRId64":%02"PRId64":%02"PRId64".%03"PRId64"\"",
                tag, hour, min, sec, millisec);
}

static int ttml_write_header(AVFormatContext *ctx)
{
    if (ctx->nb_streams != 1 ||
        ctx->streams[0]->codecpar->codec_id != AV_CODEC_ID_TTML) {
        av_log(ctx, AV_LOG_ERROR, "Exactly one TTML stream is required!\n");
        return AVERROR(EINVAL);
    }

    {
        AVStream     *s = ctx->streams[0];
        AVIOContext *pb = ctx->pb;

        AVDictionaryEntry *lang = av_dict_get(s->metadata, "language", NULL, 0);
        const char *printed_lang = (lang && lang->value) ? lang->value : "";

        avpriv_set_pts_info(s, 64, 1, 1000);

        avio_printf(pb, ttml_header_text, printed_lang);

        avio_flush(pb);
    }

    return 0;
}

static int ttml_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    AVIOContext  *pb = ctx->pb;

    avio_printf(pb,     "      <p\n");
    ttml_write_time(pb, "        begin", pkt->pts);
    avio_printf(pb, "\n");
    ttml_write_time(pb, "        end",   pkt->pts + pkt->duration);
    avio_printf(pb, ">");
    avio_write(pb, pkt->data, pkt->size);
    avio_printf(pb, "</p>\n");

    return 0;
}

static int ttml_write_trailer(AVFormatContext *ctx)
{
    AVIOContext  *pb = ctx->pb;

    avio_printf(pb, ttml_footer_text);
    avio_flush(pb);

    return 0;
}

AVOutputFormat ff_ttml_muxer = {
    .name              = "ttml",
    .long_name         = NULL_IF_CONFIG_SMALL("TTML subtitle"),
    .extensions        = "ttml",
    .mime_type         = "text/ttml",
    .flags             = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS | AVFMT_TS_NONSTRICT,
    .subtitle_codec    = AV_CODEC_ID_TTML,
    .write_header      = ttml_write_header,
    .write_packet      = ttml_write_packet,
    .write_trailer     = ttml_write_trailer,
};
