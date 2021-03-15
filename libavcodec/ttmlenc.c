/*
 * TTML subtitle encoder
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
 * TTML subtitle encoder
 * @see https://www.w3.org/TR/ttml1/
 * @see https://www.w3.org/TR/ttml2/
 * @see https://www.w3.org/TR/ttml-imsc/rec
 */

#include "avcodec.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/internal.h"
#include "ass_split.h"
#include "ass.h"
#include "ttmlenc.h"

typedef struct {
    AVCodecContext *avctx;
    ASSSplitContext *ass_ctx;
    AVBPrint buffer;
    ASSStyle *default_style;
} TTMLContext;

static void ttml_text_cb(void *priv, const char *text, int len)
{
    TTMLContext *s = priv;
    AVBPrint cur_line = { 0 };
    AVBPrint *buffer = &s->buffer;

    av_bprint_init(&cur_line, len, AV_BPRINT_SIZE_UNLIMITED);

    av_bprint_append_data(&cur_line, text, len);
    if (!av_bprint_is_complete(&cur_line)) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Failed to move the current subtitle dialog to AVBPrint!\n");
        av_bprint_finalize(&cur_line, NULL);
        return;
    }


    av_bprint_escape(buffer, cur_line.str, NULL, AV_ESCAPE_MODE_XML,
                     0);

    av_bprint_finalize(&cur_line, NULL);
}

static void ttml_new_line_cb(void *priv, int forced)
{
    TTMLContext *s = priv;

    av_bprintf(&s->buffer, "<br/>");
}

static const ASSCodesCallbacks ttml_callbacks = {
    .text             = ttml_text_cb,
    .new_line         = ttml_new_line_cb,
};

static int ttml_encode_frame(AVCodecContext *avctx, uint8_t *buf,
                             int bufsize, const AVSubtitle *sub)
{
    TTMLContext *s = avctx->priv_data;
    ASSDialog *dialog;
    int i;

    av_bprint_clear(&s->buffer);

    for (i=0; i<sub->num_rects; i++) {
        const char *ass = sub->rects[i]->ass;

        if (sub->rects[i]->type != SUBTITLE_ASS) {
            av_log(avctx, AV_LOG_ERROR, "Only SUBTITLE_ASS type supported.\n");
            return AVERROR(EINVAL);
        }

#if FF_API_ASS_TIMING
        if (!strncmp(ass, "Dialogue: ", 10)) {
            int num;
            dialog = ff_ass_split_dialog(s->ass_ctx, ass, 0, &num);

            for (; dialog && num--; dialog++) {
                int ret = ff_ass_split_override_codes(&ttml_callbacks, s,
                                                      dialog->text);
                int log_level = (ret != AVERROR_INVALIDDATA ||
                                 avctx->err_recognition & AV_EF_EXPLODE) ?
                                AV_LOG_ERROR : AV_LOG_WARNING;

                if (ret < 0) {
                    av_log(avctx, log_level,
                           "Splitting received ASS dialog failed: %s\n",
                           av_err2str(ret));

                    if (log_level == AV_LOG_ERROR)
                        return ret;
                }
            }
        } else {
#endif
            dialog = ff_ass_split_dialog2(s->ass_ctx, ass);
            if (!dialog)
                return AVERROR(ENOMEM);

            {
                av_bprintf(&s->buffer, "<span region=\"%s\">",
                           dialog->style ? dialog->style : "Default");
                int ret = ff_ass_split_override_codes(&ttml_callbacks, s,
                                                      dialog->text);
                int log_level = (ret != AVERROR_INVALIDDATA ||
                                 avctx->err_recognition & AV_EF_EXPLODE) ?
                                AV_LOG_ERROR : AV_LOG_WARNING;

                if (ret < 0) {
                    av_log(avctx, log_level,
                           "Splitting received ASS dialog text %s failed: %s\n",
                           dialog->text,
                           av_err2str(ret));

                    if (log_level == AV_LOG_ERROR) {
                        ff_ass_free_dialog(&dialog);
                        return ret;
                    }
                }

                av_bprintf(&s->buffer, "</span>");

                ff_ass_free_dialog(&dialog);
            }
#if FF_API_ASS_TIMING
        }
#endif
    }

    if (!av_bprint_is_complete(&s->buffer))
        return AVERROR(ENOMEM);
    if (!s->buffer.len)
        return 0;

    // force null-termination, so in case our destination buffer is
    // too small, the return value is larger than bufsize minus null.
    if (av_strlcpy(buf, s->buffer.str, bufsize) > bufsize - 1) {
        av_log(avctx, AV_LOG_ERROR, "Buffer too small for TTML event.\n");
        return AVERROR_BUFFER_TOO_SMALL;
    }

    return s->buffer.len;
}

static av_cold int ttml_encode_close(AVCodecContext *avctx)
{
    TTMLContext *s = avctx->priv_data;

    ff_ass_split_free(s->ass_ctx);

    av_bprint_finalize(&s->buffer, NULL);

    return 0;
}

static const char *ttml_get_display_alignment(int alignment)
{
    switch (alignment) {
    case 1:
    case 2:
    case 3:
        return "after";
    case 4:
    case 5:
    case 6:
        return "center";
    case 7:
    case 8:
    case 9:
        return "before";
    default:
        return NULL;
    }
}

static const char *ttml_get_text_alignment(int alignment)
{
    switch (alignment) {
    case 1:
    case 4:
    case 7:
        return "left";
    case 2:
    case 5:
    case 8:
        return "center";
    case 3:
    case 6:
    case 9:
        return "right";
    default:
        return NULL;
    }
}

static int ttml_get_origin(ASSScriptInfo script_info, ASSStyle *style,
                           double *origin_left, double *origin_top)
{
    if (!style)
        return AVERROR_INVALIDDATA;

    if (!script_info.play_res_x || !script_info.play_res_y)
        return AVERROR_INVALIDDATA;

    *origin_left = (style->margin_l / script_info.play_res_x);
    *origin_top = style->alignment >= 7 ?
                  (style->margin_v / script_info.play_res_y) :
                  0;

    return 0;
}

static int ttml_get_extent(ASSScriptInfo script_info, ASSStyle *style,
                           double *width, double *height)
{
    if (!style)
        return AVERROR_INVALIDDATA;

    if (!script_info.play_res_x || !script_info.play_res_y)
        return AVERROR_INVALIDDATA;

    *width = 100.0 - (style->margin_r / script_info.play_res_x);
    *height = (style->alignment <= 3) ?
              100.0 - (style->margin_v / script_info.play_res_y) :
              100.0;

    return 0;
}

static const char ttml_region_template[] =
"      <region xml:id=\"%s\"\n"
"        tts:origin=\"%.3f%% %.3f%%\"\n"
"        tts:extent=\"%.3f%% %.3f%%\"\n"
"        tts:displayAlign=\"%s\"\n"
"        tts:textAlign=\"%s\"\n"
"        tts:overflow=\"visible\" />\n";

static int ttml_write_region(AVCodecContext *avctx, AVBPrint *buf,
                             ASSScriptInfo script_info,
                             ASSStyle *style, unsigned int is_default)
{
    if (!style)
        return AVERROR_INVALIDDATA;

    const char *display_alignment =
        ttml_get_display_alignment(style->alignment);
    const char *text_alignment =
        ttml_get_text_alignment(style->alignment);
    double origin_left = 0;
    double origin_top = 0;
    double width = 0;
    double height = 0;
    int ret = AVERROR_BUG;

    if (!display_alignment || !text_alignment) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to convert ASS style alignment %d of style %s to TTML display and "
               "text alignment!\n", style->alignment, style->name);
        return AVERROR_INVALIDDATA;
    }

    if ((ret = ttml_get_origin(script_info, style, &origin_left, &origin_top)) < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to convert ASS style %s's margins (l: %d, v: %d) and "
               "play resolution (%dx%d) to TTML origin information!\n",
               style->name, style->margin_l, style->margin_v,
               script_info.play_res_x, script_info.play_res_y);
        return ret;
    }

    if ((ret = ttml_get_extent(script_info, style, &width, &height)) < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to convert ASS style %s's margins (r: %d, v: %d) and "
               "play resolution (%dx%d) to TTML origin information!\n",
               style->name, style->margin_r, style->margin_v,
               script_info.play_res_x, script_info.play_res_y);
        return ret;
    }

    av_bprintf(buf, ttml_region_template, style->name,
               origin_left, origin_top,
               width, height,
               display_alignment,
               text_alignment);

    return 0;
}

static int ttml_write_header_content(AVCodecContext *avctx)
{
    TTMLContext *s = avctx->priv_data;
    ASS *ass = (ASS *)s->ass_ctx;
    const size_t base_extradata_size = TTMLENC_EXTRADATA_SIGNATURE_SIZE + 1 +
                                       AV_INPUT_BUFFER_PADDING_SIZE;
    size_t ttml_head_size = 0;

    // pick default style by either name or due to being the first one
    ASSStyle *style = ff_ass_style_get(s->ass_ctx, "Default");
    if (!style && ass->styles_count && ass->styles) {
        style = &ass->styles[0];
    }

    if (!style)
        goto write_signature;

    s->default_style = style;

    av_bprintf(&s->buffer, "  <head>\n");
    av_bprintf(&s->buffer, "    <layout>\n");

    {
        // first, write the default style
        int ret = ttml_write_region(avctx, &s->buffer, ass->script_info,
                                    style, 1);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < ass->styles_count; i++) {
        int ret = AVERROR_BUG;
        style = &ass->styles[i];
        if (style == s->default_style)
            continue;

        if ((ret = ttml_write_region(avctx, &s->buffer, ass->script_info,
                                     style, 0)) < 0)
            return ret;
    }

    av_bprintf(&s->buffer, "    </layout>\n");
    av_bprintf(&s->buffer, "  </head>\n");

    if (!av_bprint_is_complete(&s->buffer)) {
        return AVERROR(ENOMEM);
    }

    ttml_head_size = s->buffer.len;

write_signature:
    if (!(avctx->extradata = av_mallocz(base_extradata_size + ttml_head_size))) {
        return AVERROR(ENOMEM);
    }

    avctx->extradata_size = TTMLENC_EXTRADATA_SIGNATURE_SIZE + ttml_head_size;
    memcpy(avctx->extradata, TTMLENC_EXTRADATA_SIGNATURE,
           TTMLENC_EXTRADATA_SIGNATURE_SIZE);

    if (ttml_head_size)
        memcpy(avctx->extradata + TTMLENC_EXTRADATA_SIGNATURE_SIZE,
               s->buffer.str,
               avctx->extradata_size - TTMLENC_EXTRADATA_SIGNATURE_SIZE);

    av_bprint_clear(&s->buffer);

    return 0;
}

static av_cold int ttml_encode_init(AVCodecContext *avctx)
{
    TTMLContext *s = avctx->priv_data;
    s->avctx   = avctx;
    int ret = AVERROR_BUG;

    av_bprint_init(&s->buffer, 0, AV_BPRINT_SIZE_UNLIMITED);

    if (!(s->ass_ctx = ff_ass_split(avctx->subtitle_header))) {
        return AVERROR_INVALIDDATA;
    }

    if ((ret = ttml_write_header_content(avctx)) < 0) {
        return ret;
    }

    return 0;
}

AVCodec ff_ttml_encoder = {
    .name           = "ttml",
    .long_name      = NULL_IF_CONFIG_SMALL("TTML subtitle"),
    .type           = AVMEDIA_TYPE_SUBTITLE,
    .id             = AV_CODEC_ID_TTML,
    .priv_data_size = sizeof(TTMLContext),
    .init           = ttml_encode_init,
    .encode_sub     = ttml_encode_frame,
    .close          = ttml_encode_close,
    .capabilities   = FF_CODEC_CAP_INIT_CLEANUP,
};
