/*
 * Copyright (c) 2014 Vittorio Giovara <vittorio.giovara@gmail.com>
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "buffer.h"
#include "dict.h"
#include "display.h"
#include "error.h"
#include "eval.h"
#include "libm.h"
#include "log.h"
#include "mathematics.h"

// fixed point to double
#define CONV_FP(x) ((double) (x)) / (1 << 16)

// double to fixed point
#define CONV_DB(x) (int32_t) ((x) * (1 << 16))

double av_display_rotation_get(const int32_t matrix[9])
{
    double rotation, scale[2];

    scale[0] = hypot(CONV_FP(matrix[0]), CONV_FP(matrix[3]));
    scale[1] = hypot(CONV_FP(matrix[1]), CONV_FP(matrix[4]));

    if (scale[0] == 0.0 || scale[1] == 0.0)
        return NAN;

    rotation = atan2(CONV_FP(matrix[1]) / scale[1],
                     CONV_FP(matrix[0]) / scale[0]) * 180 / M_PI;

    return -rotation;
}

void av_display_rotation_set(int32_t matrix[9], double angle)
{
    double radians = -angle * M_PI / 180.0f;
    double c = cos(radians);
    double s = sin(radians);

    memset(matrix, 0, 9 * sizeof(int32_t));

    matrix[0] = CONV_DB(c);
    matrix[1] = CONV_DB(-s);
    matrix[3] = CONV_DB(s);
    matrix[4] = CONV_DB(c);
    matrix[8] = 1 << 30;
}

void av_display_matrix_flip(int32_t matrix[9], int hflip, int vflip)
{
    int i;
    const int flip[] = { 1 - 2 * (!!hflip), 1 - 2 * (!!vflip), 1 };

    if (hflip || vflip)
        for (i = 0; i < 9; i++)
            matrix[i] *= flip[i % 3];
}

int ff_args_to_display_matrix(void *class, AVBufferRef **out,
                              const AVDictionary *args)
{
    double angle = 0.0f;
    int hflip = 0;
    int vflip = 0;

    if (!args || !out)
        return AVERROR(EINVAL);

    // Parse options (maybe these should be AVOptions,
    // but they require AVClass etc and this is a proof-of-concept...)
    {
        AVDictionaryEntry *en = av_dict_get(args, "angle", en, 0);
        if (!en || !en->value || !*en->value) {
            av_log(class, AV_LOG_ERROR,
                   "%s angle set when creating display matrix!\n",
                   !en ? "No" : "Empty");
            return AVERROR(EINVAL);
        }

        angle = av_strtod(en->value, NULL);

        if ((en = av_dict_get(args, "hflip", en, 0))) {
            if (!en->value || !*en->value) {
                av_log(class, AV_LOG_ERROR,
                       "Empty hflip set for display matrix!\n");
                return AVERROR(EINVAL);
            }
            hflip = !!atoi(en->value);
        }

        if ((en = av_dict_get(args, "vflip", en, 0))) {
            if (!en->value || !*en->value) {
                av_log(class, AV_LOG_ERROR,
                       "Empty vflip set for display matrix!\n");
                return AVERROR(EINVAL);
            }
            vflip = !!atoi(en->value);
        }
    }

    // Actually create the AVBufferRef
    {
        AVBufferRef *buf = av_buffer_allocz(sizeof(int32_t) * 9);
        if (!buf) {
            return AVERROR(ENOMEM);
        }

        av_display_rotation_set((int32_t *)buf->data, angle);
        av_display_matrix_flip((int32_t *)buf->data, hflip, vflip);

        *out = buf;
    }

    return 0;
}
