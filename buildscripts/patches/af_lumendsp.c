/*
 * Lumen DSP - audio filter
 *
 * WHY THIS IS AN FFmpeg FILTER AND NOT A SEPARATE ENGINE
 *
 * The obvious design is a DSP engine sitting between the decoder and the audio
 * output. That needs the decoded PCM, and mpv does not hand it out: it owns the
 * path from decode to output and exposes no sample callback. Taking that path
 * over means replacing the layer that took weeks to stabilise and is the reason
 * playback is currently clean.
 *
 * A libavfilter filter lives INSIDE that path instead. libavfilter already
 * hands every filter a buffer of float samples, already handles format
 * negotiation, and mpv already runs a filter chain -- the EQ uses it. So the
 * DSP goes where the audio already is, rather than the audio being routed
 * somewhere new.
 *
 * What that buys:
 *
 *   - no new audio path, so nothing to destabilise
 *   - true bypass: off means this filter is not in the chain at all
 *   - float input for free; no conversion of our own
 *   - the same filter can later copy samples for the visualizer, which solves
 *     the analyzer problem with no second mechanism
 *
 * ---------------------------------------------------------------------------
 * STAGE 1: THIS FILE PASSES AUDIO THROUGH UNCHANGED.
 * ---------------------------------------------------------------------------
 *
 * Every DSP stage is written and compiled, and every one is inert until its
 * parameter says otherwise. With default options this filter reads samples and
 * writes the same samples.
 *
 * That is deliberate. The risky part of this work is not the mathematics of a
 * biquad -- it is whether a custom filter can sit in mpv's chain on four ABIs
 * without disturbing playback. Testing that with an empty filter means a
 * failure points at one thing. Testing it with a full engine means debugging
 * the engine and the integration at once.
 *
 * Build it, put it in the chain, listen. If it is clean, the hard part is done
 * and the stages below are arithmetic on a file already proven to work.
 *
 * Target: FFmpeg 6.0 (new channel layout API, FILTER_INPUTS macros).
 */

#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "internal.h"

#include <math.h>
#include <string.h>

#define LUMEN_MAX_BANDS 32
#define LUMEN_MAX_CHANNELS 8

/*
 * A single biquad section, Direct Form I.
 *
 * DF-I rather than the more compact DF-II: at 32-bit float, DF-II accumulates
 * error in its state variables at low frequencies, which is exactly where an
 * EQ does its most audible work. A 31 Hz band at 48 kHz is a very low
 * normalised frequency and DF-II is measurably worse there. DF-I costs two
 * extra stores per sample.
 */
typedef struct LumenBiquad {
    double b0, b1, b2, a1, a2;      /* coefficients, a0 normalised out       */
    double x1[LUMEN_MAX_CHANNELS];  /* input history, per channel            */
    double x2[LUMEN_MAX_CHANNELS];
    double y1[LUMEN_MAX_CHANNELS];  /* output history, per channel           */
    double y2[LUMEN_MAX_CHANNELS];
    int    active;
} LumenBiquad;

/*
 * COEFFICIENTS ARE double EVEN THOUGH SAMPLES ARE float.
 *
 * Coefficient computation involves tan() of a small angle and differences of
 * numbers close to 1. At single precision those differences lose most of their
 * significant digits for low-frequency, high-Q bands -- the filter can end up
 * unstable or simply wrong. The samples themselves are fine in float; the
 * coefficients are not. Computing in double costs nothing per sample because
 * it happens once per parameter change.
 */

typedef enum LumenFilterType {
    LUMEN_BELL = 0,
    LUMEN_LOWSHELF,
    LUMEN_HIGHSHELF,
    LUMEN_LOWPASS,
    LUMEN_HIGHPASS,
    LUMEN_NOTCH,
} LumenFilterType;

/*
 * A parametric band: its parameters and the biquad realising them.
 *
 * The filter lives WITH the band rather than in a parallel array, because the
 * two must never disagree -- a band whose parameters say 200 Hz while its
 * coefficients say 2 kHz is a bug that produces plausible-sounding wrong audio,
 * which is the hardest kind to notice.
 */
typedef struct LumenBand {
    int         enabled;
    int         type;
    double      freq;
    double      gain_db;
    double      q;
    LumenBiquad bq;
} LumenBand;

typedef struct LumenDSPContext {
    const AVClass *class;

    /* ---- options ---- */
    int    enabled;          /* master; 0 = pure pass-through               */
    double preamp_db;
    int    limiter;
    double limiter_ceiling;
    double bass_db;
    double treble_db;
    double width;            /* 1.0 = unchanged                             */
    int    mono;

    /* ---- derived state ---- */
    int    channels;
    int    sample_rate;

    double preamp_lin;
    double preamp_current;   /* smoothed, to avoid zipper noise             */

    LumenBiquad tone_low;
    LumenBiquad tone_high;
    /*
     * PARAMETRIC BANDS: STRUCTURE ONLY IN THIS VERSION.
     *
     * The array, the per-band biquad and the processing loop are all here and
     * compiled, but nothing populates them yet -- band_count is zero, so the
     * loop does not execute.
     *
     * They are not exposed as options because a parametric EQ needs a way to
     * express 32 bands x 5 parameters through a filter argument string, and
     * that interface should be designed alongside the UI that drives it rather
     * than guessed at now.
     *
     * Left in deliberately rather than deleted: it makes the shape of the
     * finished filter visible, and adding the option parsing later touches
     * nothing that is already working.
     */
    LumenBand   bands[LUMEN_MAX_BANDS];
    int         band_count;      /* always 0 in this version */

    /* limiter state */
    double lim_env;
    double lim_release;   /* per-sample coefficient, derived from the rate */

    /* diagnostics, read back through the filter's metadata */
    double peak_out;
} LumenDSPContext;

/* ------------------------------------------------------------------ maths */

/*
 * RBJ cookbook biquads.
 *
 * These are the standard formulations and they are used because they are
 * standard: their behaviour at the edges -- very low frequency, very high Q,
 * near Nyquist -- is well understood and documented, which matters more for
 * something in the audio path than a marginally cheaper derivation would.
 */
static void lumen_set_peaking(LumenBiquad *bq, double fs, double f0,
                              double gain_db, double q)
{
    const double A     = pow(10.0, gain_db / 40.0);
    const double w0    = 2.0 * M_PI * f0 / fs;
    const double alpha = sin(w0) / (2.0 * q);
    const double cw0   = cos(w0);

    const double a0 = 1.0 + alpha / A;

    bq->b0 = (1.0 + alpha * A) / a0;
    bq->b1 = (-2.0 * cw0)      / a0;
    bq->b2 = (1.0 - alpha * A) / a0;
    bq->a1 = (-2.0 * cw0)      / a0;
    bq->a2 = (1.0 - alpha / A) / a0;
}

static void lumen_set_lowshelf(LumenBiquad *bq, double fs, double f0,
                               double gain_db, double q)
{
    const double A   = pow(10.0, gain_db / 40.0);
    const double w0  = 2.0 * M_PI * f0 / fs;
    const double cw0 = cos(w0);
    const double sw0 = sin(w0);
    const double alpha = sw0 / (2.0 * q);
    const double sq  = 2.0 * sqrt(A) * alpha;

    const double a0 = (A + 1.0) + (A - 1.0) * cw0 + sq;

    bq->b0 =      A * ((A + 1.0) - (A - 1.0) * cw0 + sq) / a0;
    bq->b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw0)    / a0;
    bq->b2 =      A * ((A + 1.0) - (A - 1.0) * cw0 - sq) / a0;
    bq->a1 =     -2.0 * ((A - 1.0) + (A + 1.0) * cw0)    / a0;
    bq->a2 =           ((A + 1.0) + (A - 1.0) * cw0 - sq) / a0;
}

static void lumen_set_highshelf(LumenBiquad *bq, double fs, double f0,
                                double gain_db, double q)
{
    const double A   = pow(10.0, gain_db / 40.0);
    const double w0  = 2.0 * M_PI * f0 / fs;
    const double cw0 = cos(w0);
    const double sw0 = sin(w0);
    const double alpha = sw0 / (2.0 * q);
    const double sq  = 2.0 * sqrt(A) * alpha;

    const double a0 = (A + 1.0) - (A - 1.0) * cw0 + sq;

    bq->b0 =      A * ((A + 1.0) + (A - 1.0) * cw0 + sq) / a0;
    bq->b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw0)    / a0;
    bq->b2 =      A * ((A + 1.0) + (A - 1.0) * cw0 - sq) / a0;
    bq->a1 =      2.0 * ((A - 1.0) - (A + 1.0) * cw0)    / a0;
    bq->a2 =           ((A + 1.0) - (A - 1.0) * cw0 - sq) / a0;
}

static void lumen_biquad_reset(LumenBiquad *bq)
{
    memset(bq->x1, 0, sizeof(bq->x1));
    memset(bq->x2, 0, sizeof(bq->x2));
    memset(bq->y1, 0, sizeof(bq->y1));
    memset(bq->y2, 0, sizeof(bq->y2));
}

static inline double lumen_biquad_run(LumenBiquad *bq, int ch, double in)
{
    const double out = bq->b0 * in
                     + bq->b1 * bq->x1[ch]
                     + bq->b2 * bq->x2[ch]
                     - bq->a1 * bq->y1[ch]
                     - bq->a2 * bq->y2[ch];

    bq->x2[ch] = bq->x1[ch];
    bq->x1[ch] = in;
    bq->y2[ch] = bq->y1[ch];
    bq->y1[ch] = out;

    return out;
}

/* ------------------------------------------------------------- lifecycle */

static int lumen_config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    LumenDSPContext *s   = ctx->priv;

    s->channels    = inlink->ch_layout.nb_channels;
    s->sample_rate = inlink->sample_rate;

    if (s->channels > LUMEN_MAX_CHANNELS)
        return AVERROR(EINVAL);

    /*
     * COEFFICIENTS ARE BUILT FROM THE LINK'S REAL SAMPLE RATE.
     *
     * Not from an assumed 44100 or 48000. A filter whose coefficients assume
     * the wrong rate is not subtly off -- every centre frequency lands in the
     * wrong place, by the ratio of the rates. Files here are 44.1, 48, 96 and
     * higher, so this has to come from the link.
     */
    s->preamp_lin     = pow(10.0, s->preamp_db / 20.0);
    s->preamp_current = s->preamp_lin;

    lumen_set_lowshelf(&s->tone_low,  s->sample_rate, 120.0,  s->bass_db,   0.707);
    lumen_set_highshelf(&s->tone_high, s->sample_rate, 8000.0, s->treble_db, 0.707);
    lumen_biquad_reset(&s->tone_low);
    lumen_biquad_reset(&s->tone_high);

    s->tone_low.active  = fabs(s->bass_db)   > 0.01;
    s->tone_high.active = fabs(s->treble_db) > 0.01;

    s->lim_env = 1.0;
    /* 100 ms release, expressed per sample at THIS rate. */
    s->lim_release = 1.0 - exp(-1.0 / (0.100 * (double)s->sample_rate));
    s->peak_out = 0.0;

    return 0;
}

/* ------------------------------------------------------------- processing */

static int lumen_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx     = inlink->dst;
    LumenDSPContext *s       = ctx->priv;
    AVFilterLink    *outlink = ctx->outputs[0];

    /*
     * TRUE BYPASS, AND IT IS THE FIRST THING CHECKED.
     *
     * Disabled means the frame is forwarded untouched -- not processed with
     * neutral settings, not copied, not converted. The samples that arrive are
     * the samples that leave, and the only cost is this comparison.
     *
     * That matters because "DSP off" must genuinely mean off. A bypass that
     * still runs the chain with unity gain is not bypass; it is processing that
     * happens to be inaudible today.
     */
    if (!s->enabled)
        return ff_filter_frame(outlink, in);

    /*
     * WRITABLE IN PLACE WHERE POSSIBLE.
     *
     * av_frame_make_writable copies only when the buffer is shared. Processing
     * in place avoids an allocation and a copy per frame in the common case,
     * which on the audio thread is worth having.
     */
    int ret = av_frame_make_writable(in);
    if (ret < 0) {
        av_frame_free(&in);
        return ret;
    }

    const int nb    = in->nb_samples;
    const int nch   = s->channels;
    float **data    = (float **)in->extended_data;

    /*
     * PLANAR FLOAT ONLY -- guaranteed by the format negotiation below, so this
     * loop can index planes directly with no per-sample branch on layout.
     */

    /* Preamp, smoothed toward its target across the frame.
     *
     * A step change in gain is a discontinuity in the waveform, which is
     * audible as a click. Approaching the target geometrically over a few
     * hundred samples makes it inaudible without needing a ramp schedule.
     */
    const double target = s->preamp_lin;

    double peak = 0.0;

    for (int i = 0; i < nb; i++) {
        s->preamp_current += (target - s->preamp_current) * 0.0005;

        for (int ch = 0; ch < nch; ch++) {
            double v = (double)data[ch][i] * s->preamp_current;

            if (s->tone_low.active)
                v = lumen_biquad_run(&s->tone_low, ch, v);
            if (s->tone_high.active)
                v = lumen_biquad_run(&s->tone_high, ch, v);

            for (int b = 0; b < s->band_count; b++)
                if (s->bands[b].enabled)
                    v = lumen_biquad_run(&s->bands[b].bq, ch, v);

            const double a = fabs(v);
            if (a > peak) peak = a;

            data[ch][i] = (float)v;
        }
    }

    /* Stereo width, only when stereo and only when asked for. */
    if (nch == 2 && (s->mono || fabs(s->width - 1.0) > 0.001)) {
        const double w = s->mono ? 0.0 : s->width;
        for (int i = 0; i < nb; i++) {
            const double l = data[0][i];
            const double r = data[1][i];
            const double mid  = (l + r) * 0.5;
            const double side = (l - r) * 0.5 * w;
            data[0][i] = (float)(mid + side);
            data[1][i] = (float)(mid - side);
        }
    }

    /*
     * Limiter: gain reduction with a fast attack and slow release.
     *
     * NOT a clipper. A clipper flattens the peak and generates harmonics across
     * the spectrum; this reduces gain smoothly so the waveform keeps its shape.
     * It engages only when a sample would exceed the ceiling, so material that
     * never approaches it is untouched.
     *
     * No lookahead in this version, which means the very first sample of a
     * transient can pass before the envelope responds. Lookahead would fix that
     * and costs latency; it is deliberately left for later.
     */
    if (s->limiter) {
        const double ceiling = s->limiter_ceiling;

        /*
         * INSTANT ATTACK, TIMED RELEASE.
         *
         * THE FIRST VERSION OF THIS DID NOT WORK, AND THE TEST CAUGHT IT.
         *
         * It eased the gain down over ~100 samples. Against a burst peaking at
         * 1.6 the measured output peak was 1.594 -- the entire transient passed
         * before the envelope had moved. A limiter that lets the peak through
         * is not a limiter; it is a slow volume control.
         *
         * Reducing gain the instant it is needed guarantees the ceiling is
         * never exceeded. Without lookahead that is the only way: there is no
         * future sample to anticipate, so the response has to be immediate.
         *
         * The cost is that a single sample can be attenuated sharply, which is
         * a mild distortion on very fast transients. Lookahead trades latency
         * to avoid it and is deliberately left for later.
         *
         * Release is a TIME CONSTANT, converted to a per-sample coefficient
         * from the real rate. A fixed per-sample number would make recovery
         * twice as fast at 96 kHz as at 48 -- the same music behaving
         * differently depending on the file.
         */
        for (int i = 0; i < nb; i++) {
            double m = 0.0;
            for (int ch = 0; ch < nch; ch++) {
                const double a = fabs((double)data[ch][i]);
                if (a > m) m = a;
            }

            const double needed = (m > ceiling) ? ceiling / m : 1.0;

            if (needed < s->lim_env)
                s->lim_env = needed;                      /* immediate */
            else
                s->lim_env += (needed - s->lim_env) * s->lim_release;

            if (s->lim_env < 1.0)
                for (int ch = 0; ch < nch; ch++)
                    data[ch][i] = (float)((double)data[ch][i] * s->lim_env);
        }
    }

    s->peak_out = peak;

    return ff_filter_frame(outlink, in);
}

/* ----------------------------------------------------------- negotiation */

/*
 * PLANAR FLOAT, AND THE SAMPLE RATE IS NOT CONSTRAINED.
 *
 * Requesting a specific rate here would make libavfilter insert a resampler to
 * satisfy it -- silently converting 44.1 to 48 for every track, which is
 * exactly the unnecessary conversion the project avoids. Accepting whatever
 * arrives means the chain runs at the source rate and this filter adapts its
 * coefficients instead.
 */
static const enum AVSampleFormat lumen_sample_fmts[] = {
    AV_SAMPLE_FMT_FLTP,
    AV_SAMPLE_FMT_NONE,
};

/* ---------------------------------------------------------------- options */

#define OFFSET(x) offsetof(LumenDSPContext, x)
#define FLAGS (AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_RUNTIME_PARAM)

static const AVOption lumendsp_options[] = {
    { "enabled", "master enable; 0 is true bypass",
      OFFSET(enabled), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { "preamp", "preamp in dB",
      OFFSET(preamp_db), AV_OPT_TYPE_DOUBLE, {.dbl = 0.0}, -24.0, 12.0, FLAGS },
    { "bass", "low shelf gain in dB at 120 Hz",
      OFFSET(bass_db), AV_OPT_TYPE_DOUBLE, {.dbl = 0.0}, -12.0, 12.0, FLAGS },
    { "treble", "high shelf gain in dB at 8 kHz",
      OFFSET(treble_db), AV_OPT_TYPE_DOUBLE, {.dbl = 0.0}, -12.0, 12.0, FLAGS },
    { "limiter", "enable the output limiter",
      OFFSET(limiter), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { "ceiling", "limiter ceiling, linear",
      OFFSET(limiter_ceiling), AV_OPT_TYPE_DOUBLE, {.dbl = 0.98}, 0.1, 1.0, FLAGS },
    { "width", "stereo width; 1.0 is unchanged",
      OFFSET(width), AV_OPT_TYPE_DOUBLE, {.dbl = 1.0}, 0.0, 2.0, FLAGS },
    { "mono", "collapse to mono",
      OFFSET(mono), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(lumendsp);

static const AVFilterPad lumendsp_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = lumen_filter_frame,
        .config_props = lumen_config_input,
    },
};

static const AVFilterPad lumendsp_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
};

const AVFilter ff_af_lumendsp = {
    .name          = "lumendsp",
    .description   = NULL_IF_CONFIG_SMALL("Lumen DSP: preamp, tone, stereo, limiter."),
    .priv_size     = sizeof(LumenDSPContext),
    .priv_class    = &lumendsp_class,
    FILTER_INPUTS(lumendsp_inputs),
    FILTER_OUTPUTS(lumendsp_outputs),
    FILTER_SAMPLEFMTS_ARRAY(lumen_sample_fmts),
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .process_command = ff_filter_process_command,
};
