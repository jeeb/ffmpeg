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

static const char *empty_ttml_document =
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

static void mov_calculate_start_and_end_of_padding_packet(AVFormatContext *s,
                                                          MOVTrack *track,
                                                          int64_t *start_dts,
                                                          int64_t *end_dts)
{
    MOVMuxContext *mov = s->priv_data;
    int64_t max_track_end_dts = (track->start_dts + track->track_duration);
    *start_dts = max_track_end_dts;

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

        // We did not yet have a start_dts, in other words this is the
        // first packet of the stream. Pick the smallest available
        // start point that is not AV_NOPTS_VALUE.
        if (track->start_dts == AV_NOPTS_VALUE) {
            int64_t picked_start_dts = 0;

            if (*start_dts == AV_NOPTS_VALUE) {
                picked_start_dts = av_rescale_q(other_track->start_dts,
                                                other_track->st->time_base,
                                                track->st->time_base);
            } else {
                picked_start_dts = FFMIN(*start_dts,
                                         av_rescale_q(other_track->start_dts,
                                         other_track->st->time_base,
                                         track->st->time_base));
            }

            // Clip us at zero, we don't want things that then get handled
            // by edit list affect us.
            *start_dts = FFMAX(0, picked_start_dts);
        }

        // finally, set the end dts to the end of the track
        // that's furthest in the time line.
        max_track_end_dts = FFMAX(max_track_end_dts, av_rescale_q((other_track->start_dts + other_track->track_duration),
                                  other_track->st->time_base,
                                  track->st->time_base));
    }

    *end_dts = max_track_end_dts;
}

static int mov_write_ttml_document_from_queue(AVFormatContext *s,
                                              AVFormatContext *ttml_ctx,
                                              MOVTrack *track,
                                              int64_t calculated_start_dts,
                                              int64_t calculated_end_dts,
                                              int64_t *out_start_dts,
                                              int64_t *out_duration)
{
    int64_t last_packets_end_time = AV_NOPTS_VALUE;
    unsigned int stop_at_current_sample = 0, packet_written = 0;
    int64_t start_dts = track->packet_queue_start_dts;
    int64_t duration  = (track->packet_queue_end_dts - track->packet_queue_start_dts);
    int ret = AVERROR_BUG;

    // If this is the first sample (track start_dts == AV_NOPTS_VALUE),
    // we have the initial DTS nonzero, are using the 'stpp' profile and
    // do not have a discontinuity happening: we start the
    // packet at DTS zero.
    if (track->start_dts == AV_NOPTS_VALUE &&
        start_dts > 0 &&
        track->par->codec_tag == MOV_MP4_TTML_TAG &&
        !track->frag_discont) {
        duration += start_dts;
        start_dts = 0;
    }

    if (calculated_start_dts != AV_NOPTS_VALUE &&
        calculated_start_dts < start_dts) {
        av_log(s, AV_LOG_VERBOSE,
               "calculated squashed packet start dts (%"PRId64") < start dts (%"PRId64"), using former\n",
               calculated_start_dts, start_dts);
        start_dts = calculated_start_dts;
        duration += (start_dts - calculated_start_dts);
    }

    while (track->squashed_packet_queue &&
           !stop_at_current_sample) {
        AVPacket looped_over_packet;

        avpriv_packet_list_get(&track->squashed_packet_queue,
                               &track->squashed_packet_queue_end,
                               &looped_over_packet);

        av_log(s, AV_LOG_VERBOSE,
               "Looping over packet: dts: %"PRId64", pts: %"PRId64", duration: %"PRId64". "
               "Calculated fragment start: %"PRId64", end: %"PRId64"\n",
               looped_over_packet.dts, looped_over_packet.pts,
               looped_over_packet.duration, calculated_start_dts,
               calculated_end_dts);

        // case 1: the following subtitle in queue is at or beyond
        //         the maximum end point of streams. We put the
        //         subtitle right back into queue, bump the packet
        //         queue start DTS and break.
        if (looped_over_packet.dts >= calculated_end_dts) {
            avpriv_packet_list_put(&track->squashed_packet_queue,
                                   &track->squashed_packet_queue_end,
                                   &looped_over_packet,
                                   av_packet_ref,
                                   FF_PACKETLIST_FLAG_PREPEND);

            av_log(s, AV_LOG_VERBOSE,
                   "Packet pushed back to queue as it is too far in the future! Squashed packet duration updated: %"PRId64" -> %"PRId64", queue start updated: %"PRId64" -> %"PRId64"\n",
                   duration, (calculated_end_dts - start_dts),
                   track->packet_queue_start_dts, calculated_end_dts);

            duration = (calculated_end_dts - start_dts);
            track->packet_queue_start_dts = calculated_end_dts;
            break;
        // case 2: the following subtitle in queue continues over
        //         the calculated end DTS. We create a duplicate
        //         AVPacket with the same data, and then stick it
        //         back to the queue.
        } else if (looped_over_packet.dts + looped_over_packet.duration >
                   calculated_end_dts) {
            AVPacket *duplicate_packet = av_packet_clone(&looped_over_packet);
            if (!duplicate_packet) {
                return AVERROR(ENOMEM);
            }

            // first, make the following subtitle packet start from
            // the end_dts and update duration to match
            duplicate_packet->dts = duplicate_packet->pts = calculated_end_dts;
            duplicate_packet->duration -= (calculated_end_dts - looped_over_packet.dts);
            avpriv_packet_list_put(&track->squashed_packet_queue,
                                   &track->squashed_packet_queue_end,
                                   duplicate_packet,
                                   av_packet_ref,
                                   FF_PACKETLIST_FLAG_PREPEND);

            av_log(s, AV_LOG_VERBOSE,
                   "Packet split as it continues between fragments (pushed back duplicate's dts: %"PRId64", pts: %"PRId64", duration: %"PRId64")! "
                   "Updating duration of the packet to output in current fragment: %"PRId64" -> %"PRId64"\n",
                   duplicate_packet->dts, duplicate_packet->pts,
                   duplicate_packet->duration,
                   looped_over_packet.duration,
                   (calculated_end_dts - looped_over_packet.dts));

            // update duration of the current packet
            looped_over_packet.duration = (calculated_end_dts - looped_over_packet.dts);

            // update the state of the current packet and track.
            duration = (calculated_end_dts - start_dts);
            track->packet_queue_start_dts = calculated_end_dts;

            // finally, we would like to stop at this iteration
            stop_at_current_sample = 1;
        }

        if (looped_over_packet.dts < last_packets_end_time) {
            int64_t diff = (last_packets_end_time - looped_over_packet.dts);

            av_log(s, AV_LOG_VERBOSE,
                   "Packet's DTS (%"PRId64") is smaller than the previous packet's end time (%"PRId64"), "
                   "adjusting dts: %"PRId64" -> %"PRId64", duration: %"PRId64" -> %"PRId64".\n",
                   looped_over_packet.dts, last_packets_end_time,
                   looped_over_packet.dts, looped_over_packet.dts + diff,
                   looped_over_packet.duration, looped_over_packet.duration - diff);

            looped_over_packet.dts = looped_over_packet.pts = looped_over_packet.dts + diff;
            looped_over_packet.duration -= diff;
        }

        last_packets_end_time = (looped_over_packet.dts + looped_over_packet.duration);

        // in case of the 'dfxp' profile each fragment is offset to its
        // beginning (or - to be exact - to the beginning of its sample).
        if (track->par->codec_tag == MOV_ISMV_TTML_TAG) {
            looped_over_packet.dts = looped_over_packet.pts = (looped_over_packet.dts - start_dts);
        }

        if (!packet_written) {
            if ((ret = avformat_write_header(ttml_ctx, NULL)) < 0) {
                av_packet_unref(&looped_over_packet);
                return ret;
            }
        }

        looped_over_packet.stream_index = 0;
        av_packet_rescale_ts(&looped_over_packet, track->st->time_base,
                             ttml_ctx->streams[looped_over_packet.stream_index]->time_base);
        if ((ret = av_write_frame(ttml_ctx, &looped_over_packet)) < 0) {
            av_packet_unref(&looped_over_packet);
            return ret;
        }

        packet_written = 1;

        av_packet_unref(&looped_over_packet);
    }

    if (calculated_end_dts > last_packets_end_time) {
        // If we are splitting squashed subtitles, we wish to
        // have fragmentation-defined durations. Force the end of
        // this squashed packet to be at the furthest point in
        // this output (which is most likely the point which caused
        // fragmentation).
        duration += (calculated_end_dts - last_packets_end_time);
    }

    if (packet_written) {
        if ((ret = av_write_trailer(ttml_ctx)) < 0)
            return ret;
    } else {
        // if nothing was written, write an empty document
        avio_write(ttml_ctx->pb, (const unsigned char*)empty_ttml_document,
                   sizeof(empty_ttml_document) - 1);
    }

    *out_start_dts = start_dts;
    *out_duration  = duration;

    return 0;
}

int ff_mov_generate_squashed_ttml_packet(AVFormatContext *s,
                                         MOVTrack *track, AVPacket *pkt)
{
    AVStream *stream = track->st;
    const AVPacket *next_pkt = ff_interleaved_peek(s, stream->index);
    AVFormatContext *ttml_ctx = NULL;
    int64_t start_dts = AV_NOPTS_VALUE;
    // check if we have a following packet available in the lavf queue
    // and utilize that information if so.
    int64_t next_packet_dts = next_pkt ? next_pkt->dts : AV_NOPTS_VALUE;
    // possible start/end points
    int64_t calculated_start_dts = AV_NOPTS_VALUE;
    int64_t calculated_end_dts = AV_NOPTS_VALUE;
    int64_t duration = 0;
    unsigned int skip_limiting_end_point = 0;
    int ret = AVERROR_BUG;

    // calculate the possible start/end points for this packet
    mov_calculate_start_and_end_of_padding_packet(s, track,
                                                  &calculated_start_dts,
                                                  &calculated_end_dts);

    if ((ret = mov_init_ttml_writer(track, &ttml_ctx)) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to initialize the TTML writer: %s\n",
               av_err2str(ret));
        goto fail;
    }

    av_log(s, AV_LOG_DEBUG,
           "Initial state when generating a %s TTML packet for track %d: "
           "start_dts: %"PRId64", end_pts: %"PRId64", duration: %"PRId64"\n",
           track->squashed_packet_queue ? "squashed" : "padding",
           track->track_id, track->start_dts,
           track->end_pts, track->track_duration);

    if (next_packet_dts != AV_NOPTS_VALUE &&
        calculated_end_dts > next_packet_dts) {
        // We have a subtitle packet in queue that overlaps with our preferred
        // fragmentation point. Duplicate it and hope that the following thing
        // doesn't get too angry.
        AVPacket *duplicate_packet = av_packet_clone(next_pkt);
        if (!duplicate_packet) {
            return AVERROR(ENOMEM);
        }

        // since the DTS is OK, just modify the duration to match our
        // requested fragment duration.
        duplicate_packet->duration = (calculated_end_dts - duplicate_packet->dts);

        avpriv_packet_list_put(&track->squashed_packet_queue,
                               &track->squashed_packet_queue_end,
                               duplicate_packet,
                               av_packet_ref, 0);

        track->packet_queue_end_dts = calculated_end_dts;
        skip_limiting_end_point = 1;

        av_log(s, AV_LOG_DEBUG,
               "An incoming packet within our fragmentation target was found "
               "in the muxer's buffer, utilizing. dts: %"PRId64", "
               "duration: %"PRId64"\n",
               duplicate_packet->dts, duplicate_packet->duration);
    }

    if (track->squashed_packet_queue) {
        if ((ret = mov_write_ttml_document_from_queue(s, ttml_ctx, track,
                                                      calculated_start_dts,
                                                      calculated_end_dts,
                                                      &start_dts,
                                                      &duration)) < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Failed to generate a squashed TTML packet from the packet "
                   "queue: %s\n",
                   av_err2str(ret));
            goto fail;
        }
    } else {
        // to have a valid comparison for the duration calculation, set it
        // to the picked start_dts if our track's start_dts is yet unset.
        // otherwise properly set it to the current track's end point.
        int64_t reference_end_dts = track->start_dts == AV_NOPTS_VALUE ?
                    calculated_start_dts :
                    (track->start_dts + track->track_duration);
        start_dts = calculated_start_dts;

        if (calculated_end_dts > reference_end_dts) {
            duration = calculated_end_dts - reference_end_dts;
            av_log(s, AV_LOG_VERBOSE,
                   "Found that the furthest track end point is further than "
                   "our current comparable value: %"PRId64" > %"PRId64" (%s). "
                   "New duration calculated: %"PRId64"\n",
                   calculated_end_dts, reference_end_dts,
                   track->start_dts == AV_NOPTS_VALUE ?
                   "found value due to AV_NOPTS_VALUE" : "actual value",
                   duration);
        }

        // if nothing was written, write an empty document
        avio_write(ttml_ctx->pb, (const unsigned char*)empty_ttml_document,
                   sizeof(empty_ttml_document) - 1);
    }

    {
        // Generate an AVPacket from the data written into the dynamic buffer.
        uint8_t *buf = NULL;
        int buf_len = avio_close_dyn_buf(ttml_ctx->pb, &buf);
        avformat_free_context(ttml_ctx);

        if ((ret = av_packet_from_data(pkt, buf, buf_len)) < 0) {
            av_log(s, AV_LOG_ERROR,
                   "Failed to create a TTML AVPacket from AVIO data: %s\n",
                   av_err2str(ret));
            goto fail;
        }

        pkt->pts = pkt->dts = start_dts;
        pkt->duration = duration;
        pkt->flags |= AV_PKT_FLAG_KEY;

        if (!skip_limiting_end_point &&
            next_packet_dts != AV_NOPTS_VALUE &&
            (pkt->dts + pkt->duration) > next_packet_dts) {
            int64_t diff = (pkt->dts + pkt->duration) - next_packet_dts;
            pkt->duration -= diff;
        }
    }

    av_log(s, AV_LOG_VERBOSE,
           "Squashed packet generated: dts: %"PRId64", pts: %"PRId64", "
           "duration: %"PRId64"\n",
           pkt->dts, pkt->pts, pkt->duration);

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
