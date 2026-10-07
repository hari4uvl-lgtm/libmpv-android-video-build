/* Lumine isolated music output filter. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "audio.h"
#include "avfilter.h"
#include "internal.h"
#include "lumenout_core.h"
#include <stdio.h>
#include <string.h>

typedef struct LumenOutContext {
    const AVClass *class;
    double volume, ceiling, lookahead, release, knee;
    LumenOut *core;
    int trim, pad, draining, started;
    int64_t next_pts;
} LumenOutContext;

#define OFFSET(x) offsetof(LumenOutContext, x)
#define AF (AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
#define LIVE (AF | AV_OPT_FLAG_RUNTIME_PARAM)
static const AVOption lumenout_options[] = {
    {"volume", "post-EQ level in dB; 25 ms ramp", OFFSET(volume), AV_OPT_TYPE_DOUBLE, {.dbl=0}, -96, 12, LIVE},
    {"ceiling", "ceiling in dBTP (conservative detector margin)", OFFSET(ceiling), AV_OPT_TYPE_DOUBLE, {.dbl=-1}, -24, 0, LIVE},
    {"lookahead", "lookahead in ms; configuration only", OFFSET(lookahead), AV_OPT_TYPE_DOUBLE, {.dbl=3}, 1, 8, AF},
    {"release", "sustained release in ms", OFFSET(release), AV_OPT_TYPE_DOUBLE, {.dbl=500}, 300, 600, LIVE},
    {"knee", "soft knee width in dB", OFFSET(knee), AV_OPT_TYPE_DOUBLE, {.dbl=1}, 0, 6, LIVE},
    {NULL}
};
AVFILTER_DEFINE_CLASS(lumenout);

static int config_input(AVFilterLink *inlink) {
    LumenOutContext *s = inlink->dst->priv;
    lumenout_destroy(s->core);
    s->core = lumenout_create(inlink->sample_rate, inlink->ch_layout.nb_channels, s->lookahead);
    if (!s->core) return AVERROR(ENOMEM);
    lumenout_prime_volume(s->core, s->volume);
    lumenout_set_ceiling(s->core, s->ceiling);
    lumenout_set_release(s->core, s->release);
    lumenout_set_knee(s->core, s->knee);
    s->trim = lumenout_latency(s->core); s->pad = 0;
    s->draining = s->started = 0; s->next_pts = AV_NOPTS_VALUE;
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    LumenOutContext *s = ctx->priv;
    LumenOutMeter m;
    int ret, trim, channels = in->ch_layout.nb_channels;
    char value[64];
    if (!s->started && !s->draining) {
        s->next_pts = in->pts;
        s->started = 1; s->pad = lumenout_latency(s->core);
    }
    ret = av_frame_make_writable(in);
    if (ret < 0) { av_frame_free(&in); return ret; }
    lumenout_process(s->core, (const double *)in->extended_data[0],
                     (double *)in->extended_data[0], in->nb_samples, &m);
    trim = FFMIN(s->trim, in->nb_samples); s->trim -= trim;
    if (trim == in->nb_samples) { av_frame_free(&in); return 0; }
    if (trim) {
        memmove(in->extended_data[0], in->extended_data[0] + (size_t)trim * channels * sizeof(double),
                (size_t)(in->nb_samples-trim) * channels * sizeof(double));
        in->nb_samples -= trim;
    }
    /* Continuous output starts at original PTS, not delayed input PTS.
     * A newly configured graph (seek/new file) creates fresh timeline state.
     */
    in->pts = s->next_pts;
    if (s->next_pts != AV_NOPTS_VALUE)
        s->next_pts += av_rescale_q(in->nb_samples, (AVRational){1, inlink->sample_rate}, outlink->time_base);
    snprintf(value, sizeof(value), "%.9f", m.gr_db);
    av_dict_set(&in->metadata, "lavfi.lumenout.gr_db", value, 0);
    snprintf(value, sizeof(value), "%.9f", m.gr_avg);
    av_dict_set(&in->metadata, "lavfi.lumenout.gr_avg", value, 0);
    /* Estimated 4x TP, not a certified independent output measurement. */
    snprintf(value, sizeof(value), "%.9f", m.tp_out);
    av_dict_set(&in->metadata, "lavfi.lumenout.tp_out", value, 0);
    return ff_filter_frame(outlink, in);
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    LumenOutContext *s = ctx->priv;
    int ret = ff_request_frame(ctx->inputs[0]);
    if (ret == AVERROR_EOF && s->pad > 0) {
        AVFrame *frame = ff_get_audio_buffer(outlink, FFMIN(1024, s->pad));
        if (!frame) return AVERROR(ENOMEM);
        memset(frame->extended_data[0], 0, (size_t)frame->nb_samples * frame->ch_layout.nb_channels * sizeof(double));
        s->pad -= frame->nb_samples; s->draining = 1;
        return filter_frame(ctx->inputs[0], frame);
    }
    return ret;
}
static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags) {
    LumenOutContext *s = ctx->priv;
    int ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0 || !s->core) return ret;
    if (!strcmp(cmd, "volume")) return lumenout_set_volume(s->core, s->volume);
    if (!strcmp(cmd, "ceiling")) return lumenout_set_ceiling(s->core, s->ceiling);
    if (!strcmp(cmd, "release")) return lumenout_set_release(s->core, s->release);
    if (!strcmp(cmd, "knee")) return lumenout_set_knee(s->core, s->knee);
    return ret;
}
static av_cold void uninit(AVFilterContext *ctx) {
    LumenOutContext *s = ctx->priv;
    lumenout_destroy(s->core); s->core = NULL;
}
static const AVFilterPad lumenout_inputs[] = {
    {.name="default", .type=AVMEDIA_TYPE_AUDIO, .filter_frame=filter_frame, .config_props=config_input}
};
static const AVFilterPad lumenout_outputs[] = {
    {.name="default", .type=AVMEDIA_TYPE_AUDIO, .request_frame=request_frame}
};
const AVFilter ff_af_lumenout = {
    .name="lumenout", .description=NULL_IF_CONFIG_SMALL("Lumine linked true-peak music output stage."),
    .priv_size=sizeof(LumenOutContext), .priv_class=&lumenout_class, .uninit=uninit,
    FILTER_INPUTS(lumenout_inputs), FILTER_OUTPUTS(lumenout_outputs),
    FILTER_SINGLE_SAMPLEFMT(AV_SAMPLE_FMT_DBL), .process_command=process_command
};
