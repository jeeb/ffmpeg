/*
 * Copyright (c) 2023 Jan Ekström <jeebjp@gmail.com>
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

#include <stdio.h>
#include "libavutil/frame.c"
#include "libavutil/mastering_display_metadata.h"

static void print_clls(const AVFrameSideDataSet set)
{
    for (int i = 0; i < set.nb_sd; i++) {
        AVFrameSideData *sd = set.sd[i];

        printf("sd %d, %s",
               i, av_frame_side_data_name(sd->type));

        if (sd->type != AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) {
            putchar('\n');
            continue;
        }

        printf(": MaxCLL: %u\n",
               ((AVContentLightMetadata *)sd->data)->MaxCLL);
    }
}

int main(void)
{
    AVFrameSideDataSet set = { 0 };

    av_assert0(
        av_frame_side_data_set_new_item(
            &set, AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT, 0, 0));

    // test entries in the middle
    for (int value = 1; value < 4; value++) {
        AVFrameSideData *sd = av_frame_side_data_set_new_item(
            &set, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
            sizeof(AVContentLightMetadata), 0);

        av_assert0(sd);

        ((AVContentLightMetadata *)sd->data)->MaxCLL = value;
    }

    av_assert0(
        av_frame_side_data_set_new_item(
            &set, AV_FRAME_DATA_SPHERICAL, 0, 0));

    // test entries at the end
    for (int value = 1; value < 4; value++) {
        AVFrameSideData *sd = av_frame_side_data_set_new_item(
            &set, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
            sizeof(AVContentLightMetadata), 0);

        av_assert0(sd);

        ((AVContentLightMetadata *)sd->data)->MaxCLL = value + 3;
    }

    puts("Initial addition results with duplicates:");
    print_clls(set);

    {
        AVFrameSideData *sd = av_frame_side_data_set_new_item(
            &set, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
            sizeof(AVContentLightMetadata),
            AV_FRAME_SIDE_DATA_SET_FLAG_NO_DUPLICATES);

        av_assert0(sd);

        ((AVContentLightMetadata *)sd->data)->MaxCLL = 1337;
    }

    puts("\nFinal state after a single 'no-duplicates' addition:");
    print_clls(set);

    {
        AVFrameSideDataSet dst_set = { 0 };
        av_assert0(av_frame_side_data_set_extend(&dst_set, set, 0) >= 0);

        puts("\nState of the copied set:");
        print_clls(dst_set);

        av_frame_side_data_set_uninit(&dst_set);
    }

    {
        int ret = av_frame_side_data_set_extend(&set, set, 0);
        printf("\nResult of trying to extend a set by itself: %s\n",
               av_err2str(ret));
    }

    av_frame_side_data_set_uninit(&set);

    return 0;
}