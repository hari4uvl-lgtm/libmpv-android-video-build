/* Lumine music output stage. SPDX-License-Identifier: LGPL-2.1-or-later
 * Four-phase / 12 taps per phase: ITU-R BS.1770-4 Annex 2.
 * This is a development candidate, not yet a qualified shipping limiter.
 */
#include "lumenout_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 8
#define GUARD_TAPS 128
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
    double history[MAX_CHANNELS][12], output_history[MAX_CHANNELS][12];
    double guard_history[MAX_CHANNELS][GUARD_TAPS];
    double guard_fir[4][GUARD_TAPS];
    double volume, volume_target, volume_step;
    int volume_left;
    double ceiling_db, release_ms, knee_db, gain_db, energy, attack;
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
    }
}

LumenOut *lumenout_create(int rate, int channels, double lookahead_ms) {
    LumenOut *s;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > MAX_CHANNELS ||
        !finite_range(lookahead_ms, 1, 8)) return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate; s->channels = channels;
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
    if (!s->ring || !s->peaks || !s->times) { lumenout_destroy(s); return NULL; }
    s->volume = s->volume_target = 1;
    s->ceiling_db = -1; s->release_ms = 500; s->knee_db = 1;
    s->attack = exp(-8.0 / (rate * lookahead_ms / 1000.0));
    prepare_guard(s);
    return s;
}
void lumenout_destroy(LumenOut *s) {
    if (!s) return;
    free(s->ring); free(s->peaks); free(s->times); free(s);
}
int lumenout_latency(const LumenOut *s) { return s->delay; }
int lumenout_prime_volume(LumenOut *s, double db) {
    if (s->clock != 0 || !finite_range(db, -96, 12)) return -1;
    s->volume = s->volume_target = db_to_gain(db);
    s->volume_left = 0; s->volume_step = 0; return 0;
}
int lumenout_set_volume(LumenOut *s, double db) {
    if (!finite_range(db, -96, 12)) return -1;
    s->volume_target = db_to_gain(db);
    s->volume_left = (int)ceil(s->rate * .025);
    s->volume_step = (s->volume_target - s->volume) / s->volume_left;
    return 0;
}
int lumenout_set_ceiling(LumenOut *s, double db) {
    if (!finite_range(db, -24, 0)) return -1;
    s->ceiling_db = db; return 0;
}
int lumenout_set_release(LumenOut *s, double ms) {
    if (!finite_range(ms, 300, 600)) return -1;
    s->release_ms = ms; return 0;
}
int lumenout_set_knee(LumenOut *s, double db) {
    if (!finite_range(db, 0, 6)) return -1;
    s->knee_db = db; return 0;
}
static double interpolated_peak(double history[12], int pos) {
    double peak = 0;
    int phase, tap;
    for (phase = 0; phase < 4; phase++) {
        double sum = 0;
        for (tap = 0; tap < 12; tap++)
            sum += fir[tap][phase] * history[(pos + 12 - tap) % 12];
        peak = fmax(peak, fabs(sum));
    }
    return peak;
}
static double target_reduction(const LumenOut *s, double peak) {
    /* 4x can under-read high-frequency reconstructed peaks. Reserve 0.75 dB
     * initially; independent 8x tests must qualify any reduction of this margin.
     * No auto-makeup: never conceal the limiter's actual attenuation.
     */
    double over = gain_to_db(peak) - (s->ceiling_db - .75);
    double k = s->knee_db;
    if (k > 0 && over > -k / 2 && over < k / 2)
        return -(over + k / 2) * (over + k / 2) / (2 * k);
    return -fmax(over, 0);
}
void lumenout_process(LumenOut *s, const double *input, double *output,
                      int frames, LumenOutMeter *meter) {
    int i, c;
    double gr_sum = 0, gr_max = 0, tp_max = 0;
    const double energy_a = exp(-1.0 / (s->rate * .1));
    for (i = 0; i < frames; i++, s->clock++) {
        double peak = 0, energy = 0, desired, gain, crest, release, coeff;
        int read = (s->write + 1) % s->capacity;
        if (s->volume_left > 0) {
            s->volume += s->volume_step;
            if (--s->volume_left == 0) s->volume = s->volume_target;
        }
        for (c = 0; c < s->channels; c++) {
            double x = input[(size_t)i * s->channels + c];
            /* Invalid decoded samples must not poison all future filter state. */
            if (!isfinite(x)) x = 0;
            x *= s->volume;
            s->ring[(size_t)s->write * s->channels + c] = x;
            s->history[c][s->history_pos] = x;
            s->guard_history[c][s->guard_pos] = x;
            peak = fmax(peak, fabs(x));
            peak = fmax(peak, interpolated_peak(s->history[c], s->history_pos));
            for (int phase = 0; phase < 4; phase++) {
                double reconstructed = 0;
                for (int tap = 0; tap < GUARD_TAPS; tap++)
                    reconstructed += s->guard_fir[phase][tap] *
                        s->guard_history[c][(s->guard_pos+GUARD_TAPS-tap) % GUARD_TAPS];
                peak = fmax(peak, fabs(reconstructed));
            }
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
        desired = target_reduction(s, peak);
        crest = peak / sqrt(fmax(s->energy, 1e-30));
        /* Isolated transients release quickly; repeated bass uses slow release. */
        release = crest > 4 ? 40 : s->release_ms;
        coeff = desired < s->gain_db ? s->attack : exp(-1.0 / (s->rate * release / 1000));
        s->gain_db = desired + coeff * (s->gain_db - desired);
        if (fabs(s->gain_db) < 1e-12) s->gain_db = 0;
        gain = db_to_gain(s->gain_db);
        for (c = 0; c < s->channels; c++) {
            double y = s->ring[(size_t)read * s->channels + c] * gain;
            if (fabs(y) < 1e-30) y = 0;
            output[(size_t)i * s->channels + c] = y;
            s->output_history[c][s->history_pos] = y;
            tp_max = fmax(tp_max, fabs(y));
            tp_max = fmax(tp_max, interpolated_peak(s->output_history[c], s->history_pos));
        }
        gr_max = fmax(gr_max, -s->gain_db); gr_sum -= s->gain_db;
        s->write = read; s->history_pos = (s->history_pos + 1) % 12;
        s->guard_pos = (s->guard_pos + 1) % GUARD_TAPS;
    }
    if (meter) {
        meter->gr_db = gr_max; meter->gr_avg = frames ? gr_sum / frames : 0;
        meter->tp_out = gain_to_db(tp_max); meter->samples = frames;
    }
}
