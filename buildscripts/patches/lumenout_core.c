/* Lumine music output stage. SPDX-License-Identifier: LGPL-2.1-or-later
 * Four-phase / 12 taps per phase: ITU-R BS.1770-4 Annex 2.
 * This is a development candidate, not yet a qualified shipping limiter.
 */
#include "lumenout_core.h"
#include "lumenout_bands.h"
#include "lumenout_clip.h"
#include <math.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 8
#define GUARD_TAPS 128
typedef struct DifferenceWindow {
    double value[GUARD_TAPS+1];
    int64_t time[GUARD_TAPS+1];
    int head,tail;
} DifferenceWindow;
static const double fir[12][4] = {
 { .001708984375,-.0291748046875,-.0189208984375,-.00830078125 },
 { .010986328125,.029296875,.0330810546875,.014892578125 },
 {-.0196533203125,-.0517578125,-.0582275390625,-.026611328125 },
 { .033203125,.089111328125,.1015625,.047607421875 },
 {-.0594482421875,-.16650390625,-.2003173828125,-.102294921875 },
 { .1373291015625,.465087890625,.77978515625,.97216796875 },
 { .97216796875,.77978515625,.465087890625,.1373291015625 },
 {-.102294921875,-.2003173828125,-.16650390625,-.0594482421875 },
 { .047607421875,.1015625,.089111328125,.033203125 },
 {-.026611328125,-.0582275390625,-.0517578125,-.0196533203125 },
 { .014892578125,.0330810546875,.029296875,.010986328125 },
 {-.00830078125,-.0189208984375,-.0291748046875,.001708984375 }
};

struct LumenOut {
    int rate, channels, delay, capacity, write, history_pos, guard_pos;
    int queue_capacity, head, tail, hold;
    int64_t clock;
    double *ring, *peaks;
    int64_t *times;
    /* Mirrored histories remove modulo/division from every FIR tap while
     * preserving exactly the same sample order and accumulation order. */
    double history[MAX_CHANNELS][24], output_history[MAX_CHANNELS][24];
    double guard_history[MAX_CHANNELS][GUARD_TAPS*2];
    double guard_fir[4][GUARD_TAPS];
#if defined(__aarch64__)
    double guard_phase[GUARD_TAPS][4];
#endif
    double guard_error_norm;
    double short_error_norm, short_dc;
    double guard_threshold, gain, fast_release, slow_release;
    double target_peak, target_db;
    DifferenceWindow *difference;
    double volume, volume_target, volume_step;
    double requested_volume;
    int protection, muted;
    int volume_left;
    double ceiling_db, release_ms, knee_db, gain_db, energy, attack;
    double lookahead_ms;
    double requested_ceiling, requested_knee;
    int full_guard_samples;
    int exact_run;
    LumenBands *bands;
    LumenClip *clip;
};

static double db_to_gain(double x) { return pow(10.0, x / 20.0); }
static double gain_to_db(double x) { return 20.0 * log10(fmax(x, 1e-30)); }
static int finite_range(double x, double low, double high) {
    return isfinite(x) && x >= low && x <= high;
}
static double bessel_i0(double x) {
    double sum = 1, term = 1;
    int n;
    for (n = 1; n < 40; n++) {
        term *= x*x/(4*n*n); sum += term;
        if (term < sum*1e-16) break;
    }
    return sum;
}
static void prepare_guard(LumenOut *s) {
    int phase, tap;
    const double pi = 3.14159265358979323846;
    for (phase = 0; phase < 4; phase++) {
        double norm = 0;
        for (tap = 0; tap < GUARD_TAPS; tap++) {
            double distance = tap-63.0-phase/4.0;
            double position = distance/64;
            double window = fabs(position) < 1 ? bessel_i0(9*sqrt(1-position*position))/bessel_i0(9) : 0;
            double sinc = fabs(distance) < 1e-12 ? 1 : sin(pi*distance)/(pi*distance);
            s->guard_fir[phase][tap] = sinc*window;
            norm += s->guard_fir[phase][tap];
        }
        for (tap = 0; tap < GUARD_TAPS; tap++) s->guard_fir[phase][tap] /= norm;
#if defined(__aarch64__)
        for (tap=0;tap<GUARD_TAPS;tap++) s->guard_phase[tap][phase]=s->guard_fir[phase][tap];
#endif
        /* Let h be the exact long kernel, a the two-point interpolator and
         * F[k]=sum_{j<=k}(h[j]-a[j]). Since sum(h-a)=0, its convolution error
         * is bounded by max|x[n]-x[n-1]| * sum|F[k]| (summation by parts).
         * This rigorous bound is cheap for smooth/quiet signals; full FIR is
         * still required whenever the bound could reach the knee threshold.
         */
        double cumulative=0, error_norm=0;
        for(tap=0;tap<GUARD_TAPS;tap++) {
            cumulative+=s->guard_fir[phase][tap];
            if(tap==63) cumulative-=1-phase/4.0;
            if(tap==64) cumulative-=phase/4.0;
            if(tap<GUARD_TAPS-1) error_norm+=fabs(cumulative);
        }
        s->guard_error_norm=fmax(s->guard_error_norm,error_norm);
        double dc=0, short_cumulative=0, short_norm=0;
        for(tap=0;tap<12;tap++) dc+=fir[tap][phase];
        for(tap=0;tap<12;tap++) {
            short_cumulative+=fir[tap][phase];
            if(tap==5) short_cumulative-=dc*(.125+phase*.25);
            if(tap==6) short_cumulative-=dc*(.875-phase*.25);
            if(tap<11) short_norm+=fabs(short_cumulative);
        }
        s->short_dc=fmax(s->short_dc,fabs(dc));
        s->short_error_norm=fmax(s->short_error_norm,short_norm);
    }
}

LumenOut *lumenout_create(int rate, int channels, double lookahead_ms) {
    LumenOut *s;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > MAX_CHANNELS ||
        !finite_range(lookahead_ms, 1, 8)) return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate; s->channels = channels;
    s->lookahead_ms = lookahead_ms;
    /* The short standard meter under-reads finite near-Nyquist boundaries.
     * Supplement it with a longer full-band interpolator; never globally
     * remove those frequencies from the actual audio signal.
     */
    s->delay = (int)ceil(rate * lookahead_ms / 1000.0) + GUARD_TAPS/2 + 6;
    s->capacity = s->delay + 1;
    /* Hold a complete 40 Hz period: avoid release between bass crests. */
    s->hold = (int)ceil(rate * .025);
    s->queue_capacity = s->delay + s->hold + 32;
    s->ring = calloc((size_t)s->capacity * channels, sizeof(double));
    s->peaks = calloc(s->queue_capacity, sizeof(double));
    s->times = calloc(s->queue_capacity, sizeof(int64_t));
    s->difference=calloc(channels,sizeof(*s->difference));
    if (!s->ring || !s->peaks || !s->times || !s->difference) { lumenout_destroy(s); return NULL; }
    s->volume = s->volume_target = 1;
    s->requested_volume=1; s->protection=1;
    s->ceiling_db = -1; s->release_ms = 500; s->knee_db = 1;
    s->requested_ceiling=-1; s->requested_knee=1;
    s->guard_threshold=db_to_gain(-1-.75-.5);
    s->gain=1;
    s->target_peak=NAN;
    s->fast_release=exp(-1.0/(rate*40.0/1000));
    s->slow_release=exp(-1.0/(rate*500.0/1000));
    s->attack = exp(-8.0 / (rate * lookahead_ms / 1000.0));
    prepare_guard(s);
    return s;
}
void lumenout_destroy(LumenOut *s) {
    if (!s) return;
    lumenbands_destroy(s->bands);
    lumenclip_destroy(s->clip);
    free(s->ring); free(s->peaks); free(s->times); free(s->difference); free(s);
}
int lumenout_latency(const LumenOut *s) {
    return s->delay + (s->bands ? lumenbands_latency(s->bands) : 0) +
           (s->clip ? lumenclip_latency(s->clip) : 0);
}
int lumenout_configure_bands(LumenOut *s, int bands) {
    LumenBands *next = NULL;
    if (s->clock != 0 || (bands != 1 && bands != 2)) return -1;
    if (bands == 2) {
        next = lumenbands_create(s->rate, s->channels, s->lookahead_ms);
        if (!next) return -1;
        lumenbands_settings(next,s->ceiling_db-.75,s->knee_db,s->release_ms);
    }
    lumenbands_destroy(s->bands); s->bands = next;
    return 0;
}
int lumenout_configure_clip(LumenOut *s, int enabled) {
    LumenClip *next = NULL;
    if(s->clock != 0 || (enabled != 0 && enabled != 1)) return -1;
    if(enabled) {
        next=lumenclip_create(s->rate,s->channels,s->ceiling_db);
        if(!next) return -1;
    }
    lumenclip_destroy(s->clip); s->clip=next; return 0;
}
int lumenout_prime_volume(LumenOut *s, double db) {
    if (s->clock != 0 || !finite_range(db, -96, 12)) return -1;
    s->requested_volume=db_to_gain(db);
    s->volume = s->volume_target = s->muted ? 0 : s->requested_volume;
    s->volume_left = 0; s->volume_step = 0; return 0;
}
int lumenout_set_volume(LumenOut *s, double db) {
    if (!finite_range(db, -96, 12)) return -1;
    s->requested_volume=db_to_gain(db);
    s->volume_target = s->muted ? 0 : s->requested_volume;
    s->volume_left = (int)ceil(s->rate * .025);
    s->volume_step = (s->volume_target - s->volume) / s->volume_left;
    return 0;
}
int lumenout_set_muted(LumenOut *s,int muted) {
    if(muted!=0 && muted!=1) return -1;
    s->muted=muted; s->volume_target=muted ? 0 : s->requested_volume;
    if(s->clock==0) {
        s->volume=s->volume_target; s->volume_left=0; s->volume_step=0;
    } else {
        s->volume_left=(int)ceil(s->rate*.025);
        s->volume_step=(s->volume_target-s->volume)/s->volume_left;
    }
    return 0;
}
int lumenout_set_protection(LumenOut *s,int enabled) {
    if(enabled!=0 && enabled!=1) return -1;
    if(enabled && !s->protection) {
        // Re-prime only the detector, never the audio/FIR/volume histories.
        // Old unprotected audio remains unchanged until newly protected data
        // traverses the existing lookahead; all boosting data is protected.
        s->head=s->tail=0;
        s->exact_run=0;
        s->full_guard_samples=s->delay+s->hold+1;
    }
    s->protection=enabled; return 0;
}
static void apply_requested(LumenOut *s) {
    s->ceiling_db=s->requested_ceiling; s->knee_db=s->requested_knee;
    s->guard_threshold=db_to_gain(s->ceiling_db-.75-s->knee_db*.5);
    s->target_peak=NAN;
    if(s->clip) lumenclip_ceiling(s->clip,s->ceiling_db);
    if(s->bands) lumenbands_settings(s->bands,s->ceiling_db-.75,s->knee_db,s->release_ms);
}
static void schedule_requested(LumenOut *s) {
    double old_floor=s->ceiling_db-.75-s->knee_db*.5;
    double new_floor=s->requested_ceiling-.75-s->requested_knee*.5;
    if(s->clock==0 || !s->protection || new_floor>=old_floor ||
       s->exact_run>=s->delay+s->hold+1) {
        apply_requested(s);
    } else if(s->full_guard_samples==0) {
        // A lower threshold could turn a previous safe upper bound into a
        // false gain cut. Compute exact peaks for one complete detector window
        // before applying it. About 30 ms at48kHz; no history/audio reset or
        // conservative phantom attenuation. Repeated commands coalesce.
        s->full_guard_samples=s->delay+s->hold+1;
    }
}
int lumenout_set_ceiling(LumenOut *s, double db) {
    if (!finite_range(db, -24, 0)) return -1;
    s->requested_ceiling = db;
    schedule_requested(s);
    return 0;
}
int lumenout_set_release(LumenOut *s, double ms) {
    if (!finite_range(ms, 300, 600)) return -1;
    s->release_ms = ms;
    s->slow_release=exp(-1.0/(s->rate*ms/1000));
    if(s->bands) lumenbands_settings(s->bands,s->ceiling_db-.75,s->knee_db,s->release_ms);
    return 0;
}
int lumenout_set_knee(LumenOut *s, double db) {
    if (!finite_range(db, 0, 6)) return -1;
    s->requested_knee = db;
    schedule_requested(s);
    return 0;
}
#if defined(__aarch64__)
/* SIMD lanes are independent interpolation phases. Each phase retains its
 * original tap order; no precision reduction or fast-math reassociation. */
static double phase_max(float64x2_t a,float64x2_t b) {
    double peak=0;
    peak=fmax(peak,fabs(vgetq_lane_f64(a,0)));
    peak=fmax(peak,fabs(vgetq_lane_f64(a,1)));
    peak=fmax(peak,fabs(vgetq_lane_f64(b,0)));
    return fmax(peak,fabs(vgetq_lane_f64(b,1)));
}
static double interpolated_peak(const double *history,int pos) {
    float64x2_t a=vdupq_n_f64(0),b=vdupq_n_f64(0);
    for(int tap=0;tap<12;tap++) {
        double x=history[pos+12-tap];
        a=vfmaq_n_f64(a,vld1q_f64(&fir[tap][0]),x);
        b=vfmaq_n_f64(b,vld1q_f64(&fir[tap][2]),x);
    }
    return phase_max(a,b);
}
static double long_peak(const LumenOut *s,const double *history,int pos) {
    float64x2_t a=vdupq_n_f64(0),b=vdupq_n_f64(0);
    for(int tap=0;tap<GUARD_TAPS;tap++) {
        double x=history[pos+GUARD_TAPS-tap];
        a=vfmaq_n_f64(a,vld1q_f64(&s->guard_phase[tap][0]),x);
        b=vfmaq_n_f64(b,vld1q_f64(&s->guard_phase[tap][2]),x);
    }
    return phase_max(a,b);
}
#else
static double interpolated_peak(const double *history, int pos) {
    double peak = 0;
    int phase, tap;
    for (phase = 0; phase < 4; phase++) {
        double sum = 0;
        for (tap = 0; tap < 12; tap++)
            sum += fir[tap][phase] * history[pos + 12 - tap];
        peak = fmax(peak, fabs(sum));
    }
    return peak;
}
#endif
static double maximum_difference(DifferenceWindow *s,int64_t clock,double value) {
    const int capacity=GUARD_TAPS+1;
    while(s->head!=s->tail && s->time[s->head]<clock-(GUARD_TAPS-2))
        s->head=(s->head+1)%capacity;
    while(s->head!=s->tail) {
        int last=(s->tail+capacity-1)%capacity;
        if(s->value[last]>value) break;
        s->tail=last;
    }
    s->value[s->tail]=value; s->time[s->tail]=clock;
    s->tail=(s->tail+1)%capacity;
    return s->value[s->head];
}
static double target_reduction(LumenOut *s, double peak) {
    /* 4x can under-read high-frequency reconstructed peaks. Reserve 0.75 dB
     * initially; independent 8x tests must qualify any reduction of this margin.
     * No auto-makeup: never conceal the limiter's actual attenuation.
     */
    if(peak<=s->guard_threshold) return 0;
    if(peak==s->target_peak) return s->target_db;
    double over = gain_to_db(peak) - (s->ceiling_db - .75);
    double k = s->knee_db;
    s->target_peak=peak;
    s->target_db=(k > 0 && over > -k / 2 && over < k / 2)
        ? -(over + k / 2) * (over + k / 2) / (2 * k) : -fmax(over, 0);
    return s->target_db;
}
void lumenout_process(LumenOut *s, const double *input, double *output,
                      int frames, LumenOutMeter *meter) {
    int i, c;
    double gr_sum = 0, gr_max = 0, tp_max = 0;
    double low_gr_max = 0, high_gr_max = 0;
    double clip_max = 0;
    const double energy_a = exp(-1.0 / (s->rate * .1));
    for (i = 0; i < frames; i++, s->clock++) {
        int all_exact=s->protection && (s->gain_db<0 || s->full_guard_samples>0);
        if(s->full_guard_samples==0 &&
            (s->ceiling_db!=s->requested_ceiling || s->knee_db!=s->requested_knee))
            apply_requested(s);
        double peak = 0, energy = 0, desired, gain, crest, coeff;
        int read = (s->write + 1) % s->capacity;
        if (s->volume_left > 0) {
            s->volume += s->volume_step;
            if (--s->volume_left == 0) s->volume = s->volume_target;
        }
        double samples[MAX_CHANNELS];
        for (c = 0; c < s->channels; c++) {
            double x = input[(size_t)i * s->channels + c];
            /* Invalid decoded samples must not poison all future filter state. */
            if (!isfinite(x)) x = 0;
            /* Bound malformed float PCM far beyond any legitimate audio level;
             * prevents finite-overflow from poisoning RMS and FIR histories.
             */
            x=fmax(-1e6,fmin(1e6,x));
            samples[c] = x*s->volume;
        }
        if(s->bands) {
            double low_gr, high_gr;
            lumenbands_process(s->bands,samples,samples,&low_gr,&high_gr);
            low_gr_max=fmax(low_gr_max,low_gr); high_gr_max=fmax(high_gr_max,high_gr);
        }
        if(s->clip) {
            double clip_db;
            lumenclip_process(s->clip,samples,samples,&clip_db);
            clip_max=fmax(clip_max,clip_db);
        }
        for (c = 0; c < s->channels; c++) {
            double x = samples[c];
            s->ring[(size_t)s->write * s->channels + c] = x;
            s->history[c][s->history_pos] = x;
            s->history[c][s->history_pos+12] = x;
            double previous=s->guard_history[c][s->guard_pos+GUARD_TAPS-1];
            double difference=maximum_difference(&s->difference[c],s->clock,fabs(x-previous));
            s->guard_history[c][s->guard_pos] = x;
            s->guard_history[c][s->guard_pos+GUARD_TAPS] = x;
            peak = fmax(peak, fabs(x));
            double short_centre=fmax(fabs(s->history[c][s->history_pos+12-5]),
                                     fabs(s->history[c][s->history_pos+12-6]));
            double short_bound=s->short_dc*short_centre+difference*s->short_error_norm+
                               1e-12*(1+short_centre+difference);
            if(s->protection && (s->gain_db<0 || s->full_guard_samples>0 || short_bound>=s->guard_threshold))
                peak = fmax(peak, interpolated_peak(s->history[c], s->history_pos));
            else peak=fmax(peak,short_bound);
            double centre=fmax(fabs(s->guard_history[c][s->guard_pos+GUARD_TAPS-63]),
                               fabs(s->guard_history[c][s->guard_pos+GUARD_TAPS-64]));
            double bound=centre+difference*s->guard_error_norm+1e-12*(1+centre+difference);
            // Exact peaks throughout gain reduction/recovery preserve the
            // original adaptive-release crest decision. Pre-limiting bounds
            // are strictly below threshold and expire before the first exact
            // limiting crest leaves the window, so they cannot change release.
            int exact=s->protection && (s->gain_db<0 || s->full_guard_samples>0 || bound>=s->guard_threshold);
            if(!exact) peak=fmax(peak,bound);
#if defined(__aarch64__)
            if(exact) peak=fmax(peak,long_peak(s,s->guard_history[c],s->guard_pos));
#else
            for (int phase = 0; exact && phase < 4; phase++) {
                double reconstructed = 0;
                for (int tap = 0; tap < GUARD_TAPS; tap++)
                    reconstructed += s->guard_fir[phase][tap] *
                        s->guard_history[c][s->guard_pos+GUARD_TAPS-tap];
                peak = fmax(peak, fabs(reconstructed));
            }
#endif
            energy += x * x;
        }
        s->energy = energy_a * s->energy + (1 - energy_a) * energy / s->channels;
        /* Monotonic deque: bounded O(1) amortized work, no runtime allocations. */
        while (s->head != s->tail &&
               s->times[s->head] < s->clock - s->delay - s->hold)
            s->head = (s->head + 1) % s->queue_capacity;
        while (s->head != s->tail) {
            int last = (s->tail + s->queue_capacity - 1) % s->queue_capacity;
            if (s->peaks[last] > peak) break;
            s->tail = last;
        }
        s->peaks[s->tail] = peak; s->times[s->tail] = s->clock;
        s->tail = (s->tail + 1) % s->queue_capacity;
        peak = s->peaks[s->head];
        desired = s->protection ? target_reduction(s, peak) : 0;
        if(desired!=s->gain_db) {
            crest = peak / sqrt(fmax(s->energy, 1e-30));
            /* Isolated transients release quickly; repeated bass uses slow release. */
            coeff = desired < s->gain_db ? s->attack :
                    (crest>4 || !s->protection ? s->fast_release : s->slow_release);
            s->gain_db = desired + coeff * (s->gain_db - desired);
            if (fabs(s->gain_db) < 1e-12) s->gain_db = 0;
            s->gain = db_to_gain(s->gain_db);
        }
        gain=s->gain;
        for (c = 0; c < s->channels; c++) {
            double y = s->ring[(size_t)read * s->channels + c] * gain;
            /* The output mute gate also silences delayed samples and the
             * optional crossover's numerical tail. Keep histories running so
             * unmute retains the existing ramp and continuous filter state. */
            if (s->muted || fabs(y) < 1e-30) y = 0;
            output[(size_t)i * s->channels + c] = y;
            s->output_history[c][s->history_pos] = y;
            s->output_history[c][s->history_pos+12] = y;
            tp_max = fmax(tp_max, fabs(y));
            tp_max = fmax(tp_max, interpolated_peak(s->output_history[c], s->history_pos));
        }
        gr_max = fmax(gr_max, -s->gain_db); gr_sum -= s->gain_db;
        s->write = read; s->history_pos = (s->history_pos + 1) % 12;
        s->guard_pos = (s->guard_pos + 1) % GUARD_TAPS;
        if(s->full_guard_samples>0) s->full_guard_samples--;
        s->exact_run=all_exact ? (s->exact_run<s->delay+s->hold+1 ? s->exact_run+1 : s->exact_run) : 0;
    }
    if (meter) {
        meter->gr_db = gr_max; meter->gr_avg = frames ? gr_sum / frames : 0;
        meter->tp_out = gain_to_db(tp_max); meter->samples = frames;
        meter->low_gr_db=low_gr_max; meter->high_gr_db=high_gr_max;
        meter->clip_db=clip_max;
    }
}
