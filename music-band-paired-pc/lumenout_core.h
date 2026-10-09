/* Lumine music output stage. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LUMENOUT_CORE_H
#define LUMENOUT_CORE_H
#include <stdint.h>

typedef struct LumenOut LumenOut;
typedef struct LumenOutMeter {
    double gr_db, gr_avg, tp_out, low_gr_db, high_gr_db, clip_db;
    uint64_t samples;
} LumenOutMeter;

/* Configuration-time allocations only. Runtime setters preserve audio history.
 * Candidate latency: lookahead + 70 samples (long guard plus safety overlap).
 * The additional guard is required by failed near-Nyquist boundary tests.
 * Core is single-thread-owned; serialize commands with processing.
 */
LumenOut *lumenout_create(int rate, int channels, double lookahead_ms);
void lumenout_destroy(LumenOut *s);
int lumenout_latency(const LumenOut *s);
/* Optional LR4 mode changes phase/latency; only before processing starts. */
int lumenout_configure_bands(LumenOut *s, int bands);
int lumenout_configure_clip(LumenOut *s, int enabled);
/* Initial graph level must not ramp from unity, especially for quiet DVC. */
int lumenout_prime_volume(LumenOut *s, double db);
int lumenout_set_volume(LumenOut *s, double db);
int lumenout_set_ceiling(LumenOut *s, double db);
int lumenout_set_release(LumenOut *s, double ms);
int lumenout_set_knee(LumenOut *s, double db);
int lumenout_set_protection(LumenOut *s, int enabled);
int lumenout_set_muted(LumenOut *s, int muted);
void lumenout_process(LumenOut *s, const double *input, double *output,
                      int frames, LumenOutMeter *meter);
#endif
