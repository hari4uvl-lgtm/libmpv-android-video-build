/* Optional configuration-time LR4 music output stage. LGPL-2.1-or-later. */
#ifndef LUMENOUT_BANDS_H
#define LUMENOUT_BANDS_H
typedef struct LumenBands LumenBands;
LumenBands *lumenbands_create(int rate, int channels, double lookahead_ms);
void lumenbands_destroy(LumenBands *s);
int lumenbands_latency(const LumenBands *s);
void lumenbands_settings(LumenBands *s, double ceiling_db, double knee_db, double release_ms);
/* One stereo-linked sample frame; no allocation, no shared state. */
void lumenbands_process(LumenBands *s, const double *input, double *output,
                        double *low_gr, double *high_gr);
#endif
