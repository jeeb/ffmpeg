/*
 * MP4, ISMV Muxer TTML helpers
 * Copyright (c) 2021 24i
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

#include "avformat.h"
#include "avio_internal.h"
#include "isom.h"
#include "movenc.h"
#include "movenc_ttml.h"
#include "libavcodec/packet_internal.h"

static const unsigned char empty_ttml_document[] =
    "<tt xml:lang=\"\" xmlns=\"http://www.w3.org/ns/ttml\" />";

static int mov_init_ttml_writer(MOVTrack *track, AVFormatContext **out_ctx)
{
    AVStream *movenc_stream = track->st, *ttml_stream = NULL;
    AVFormatContext *ttml_ctx = NULL;
    int ret = AVERROR_BUG;
    if ((ret = avformat_alloc_output_context2(&ttml_ctx, NULL,
                                              "ttml", NULL)) < 0)
        goto fail;

    if ((ret = avio_open_dyn_buf(&ttml_ctx->pb)) < 0)
        goto fail;

    if (!(ttml_stream = avformat_new_stream(ttml_ctx, NULL))) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    if ((ret = avcodec_parameters_copy(ttml_stream->codecpar,
                                       movenc_stream->codecpar)) < 0)
        goto fail;

    ttml_stream->time_base = movenc_stream->time_base;

    *out_ctx = ttml_ctx;

    return 0;

fail:
    if (ttml_ctx) {
        uint8_t *buf = NULL;
        avio_close_dyn_buf(ttml_ctx->pb, &buf);
        av_freep(&buf);
    }

    avformat_free_context(ttml_ctx);

    return ret;
}

static void mov_calculate_start_and_end_based_on_other_tracks(AVFormatContext *s,
                                                              MOVTrack *track,
                                                              int64_t *start_ts,
                                                              int64_t *end_ts)
{
    MOVMuxContext *mov = s->priv_data;

    // initialize the end and start to the current end point of already written
    // packets, or to zero if the track has not yet had any packets written.
    int64_t max_track_end_ts =  track->start_dts == AV_NOPTS_VALUE ?
                                0 : (track->start_dts + track->track_duration);
    *start_ts = max_track_end_ts;

    // Now, go through all the streams and figure out
    // the furthest start/end points in this muxer instance.
    for (unsigned int i = 0; i < s->nb_streams; i++) {
        MOVTrack *other_track = &mov->tracks[i];

        // Skip our own track, any other track that needs squashing,
        // or any track still has its start_dts at NOPTS.
        if (track == other_track ||
            other_track->squash_fragment_samples_to_one ||
            other_track->start_dts == AV_NOPTS_VALUE) {
            continue;
        }

        // finally, set the end timestamp to the end of the track
        // that's furthest in the time line.
        max_track_end_ts = FFMAX(
            max_track_end_ts,
            av_rescale_q((other_track->start_dts + other_track->track_duration),
                         other_track->st->time_base,
                         track->st->time_base));
    }

    *end_ts = max_track_end_ts;
}

static int mov_write_ttml_document_from_queue(AVFormatContext *s,
                                              AVFormatContext *ttml_ctx,
                                              MOVTrack *track,
                                              int64_t calculated_start_ts,
                                              int64_t calculated_end_ts,
                                              int64_t *out_start_ts,
                                              int64_t *out_duration)
{
    int ret = AVERROR_BUG;
    int64_t start_ts = FFMIN(track->packet_queue_start_ts, calculated_start_ts);
    int64_t duration = FFMAX(track->packet_queue_end_ts, calculated_end_ts) - start_ts;
    AVPacket *looped_pkt = av_packet_alloc();
    if (!looped_pkt) {
        av_log(s, AV_LOG_ERROR,
               "Failed to allocate AVPacket for going through packet queue!\n");
        return AVERROR(ENOMEM);
    }

    if ((ret = avformat_write_header(ttml_ctx, NULL)) < 0) {
        return ret;
    }

    while (!avpriv_packet_list_get(&track->squashed_packet_queue,
                                   &track->squashed_packet_queue_end,
                                   looped_pkt)) {
        // in case of the 'dfxp' muxing mode, each written document is offset
        // to its containing sample's beginning.
        if (track->par->codec_tag == MOV_ISMV_TTML_TAG) {
            looped_pkt->dts = looped_pkt->pts = (looped_pkt->pts - start_ts);
        }

        looped_pkt->stream_index = 0;

        av_packet_rescale_ts(looped_pkt, track->st->time_base,
                             ttml_ctx->streams[looped_pkt->stream_index]->time_base);

        if ((ret = av_write_frame(ttml_ctx, looped_pkt)) < 0) {
            goto cleanup;
        }

        av_packet_unref(looped_pkt);
    }

    if ((ret = av_write_trailer(ttml_ctx)) < 0)
        goto cleanup;

    *out_start_ts = start_ts;
    *out_duration = duration;

    ret = 0;

cleanup:
    av_packet_free(&looped_pkt);

    return ret;
}

int ff_mov_generate_squashed_ttml_packet(AVFormatContext *s,
                                         MOVTrack *track, AVPacket *pkt)
{
    AVFormatContext *ttml_ctx = NULL;
    // possible start/end points
    int64_t calculated_start_ts = AV_NOPTS_VALUE;
    int64_t calculated_end_ts = AV_NOPTS_VALUE;
    // values for the generated AVPacket
    int64_t start_ts = 0;
    int64_t duration = 0;

    int ret = AVERROR_BUG;

    // calculate the possible start/end points for this packet
    mov_calculate_start_and_end_based_on_other_tracks(s, track,
                                                      &calculated_start_ts,
                                                      &calculated_end_ts);

    if ((ret = mov_init_ttml_writer(track, &ttml_ctx)) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to initialize the TTML writer: %s\n",
               av_err2str(ret));
        goto cleanup;
    }

    if (!track->squashed_packet_queue) {
        // empty queue, write minimal empty document with calculated values
        // based on other tracks.
        avio_write(ttml_ctx->pb, empty_ttml_document,
                   sizeof(empty_ttml_document) - 1);
        start_ts = calculated_start_ts;
        duration = (calculated_end_ts - calculated_start_ts);
        goto generate_packet;
    }

    if ((ret = mov_write_ttml_document_from_queue(s, ttml_ctx, track,
                                                  calculated_start_ts,
                                                  calculated_end_ts,
                                                  &start_ts,
                                                  &duration)) < 0) {
        av_log(s, AV_LOG_ERROR,
               "Failed to generate a squashed TTML packet from the packet "
               "queue: %s\n",
               av_err2str(ret));
        goto cleanup;
    }

generate_packet:
    {
        // Generate an AVPacket from the data written into the dynamic buffer.
        uint8_t *buf = NULL;
        int buf_len = avio_close_dyn_buf(ttml_ctx->pb, &buf);
        ttml_ctx->pb = NULL;

        if ((ret = av_packet_from_data(pkt, buf, buf_len)) < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Failed to create a TTML AVPacket from AVIO data: %s\n",
                   av_err2str(ret));
            av_freep(&buf);
            goto cleanup;
        }

        pkt->pts = pkt->dts = start_ts;
        pkt->duration = duration;
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    ret = 0;

cleanup:
    if (ttml_ctx && ttml_ctx->pb) {
        uint8_t *buf = NULL;
        avio_close_dyn_buf(ttml_ctx->pb, &buf);
        av_freep(&buf);
    }

    avformat_free_context(ttml_ctx);
    return ret;
}
