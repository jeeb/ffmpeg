#include "libavutil/frame.h"
#include "vp9shared.h"
#include "videotoolbox_vp9.h"

AVFrame *ff_videotoolbox_get_vp9_frame(AVCodecContext *avctx)
{
    const VP9SharedContext *h = avctx->priv_data;
    return h->frames[CUR_FRAME].tf.f;
}
