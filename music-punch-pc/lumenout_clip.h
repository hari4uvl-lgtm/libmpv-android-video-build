/* Optional 4x tiny soft-clip stage. LGPL-2.1-or-later. */
#ifndef LUMENOUT_CLIP_H
#define LUMENOUT_CLIP_H
typedef struct LumenClip LumenClip;
LumenClip *lumenclip_create(int rate, int channels, double ceiling_db);
void lumenclip_destroy(LumenClip *s);
int lumenclip_latency(const LumenClip *s);
void lumenclip_ceiling(LumenClip *s, double ceiling_db);
void lumenclip_process(LumenClip *s, const double *input, double *output, double *clip_db);
#endif
