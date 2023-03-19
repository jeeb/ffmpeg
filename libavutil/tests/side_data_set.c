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

        if (sd->type != AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)
            continue;

        printf("sd %d, MaxCLL: %u\n",
               i, ((AVContentLightMetadata *)sd->data)->MaxCLL);
    }
}

int main(void)
{
    AVFrameSideDataSet set = { 0 };

    for (int value = 1; value < 4; value++) {
        AVFrameSideData *sd = av_side_data_set_new_item(
            &set, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
            sizeof(AVContentLightMetadata), 1);

        av_assert0(sd);

        ((AVContentLightMetadata *)sd->data)->MaxCLL = value;
    }

    puts("Initial addition results with duplicates:");
    print_clls(set);

    {
        AVFrameSideData *sd = av_side_data_set_new_item(
            &set, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
            sizeof(AVContentLightMetadata), 0);

        av_assert0(sd);

        ((AVContentLightMetadata *)sd->data)->MaxCLL = 1337;
    }

    puts("\nFinal state after a single 'no-duplicates' addition:");
    print_clls(set);

    av_side_data_set_uninit(&set);

    return 0;
}
