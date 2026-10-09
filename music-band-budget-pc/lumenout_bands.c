/* Optional 120 Hz LR4 split, independent linked gains, final TP stage follows.
 * The summed unattenuated crossover is all-pass, not phase-identical to input.
 * No runtime mode switch: configure a new graph to avoid phase discontinuities.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "lumenout_bands.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* Host-only experiment: stop an unrelated high-band peak from leaving bass
 * only three percent of the ceiling. Keep the existing high-band priority,
 * but reserve a bounded low-band allowance. The unchanged final true-peak
 * stage owns sum safety; this does not promise that high-band ducking vanishes.
 * No audio history reset, extra filter, buffer or allocation is introduced.
 */
#ifndef LUMEN_BUDGET_FLOOR
#define LUMEN_BUDGET_FLOOR .25
#endif

typedef struct Biquad { double b0, b1, b2, a1, a2, z1, z2; } Biquad;
typedef struct PeakWindow {
    double *peak;
    int64_t *time;
    int head, tail, capacity, window;
} PeakWindow;
struct LumenBands {
    int rate, channels, delay, capacity, write;
    int64_t clock;
    Biquad low[8][2], high[8][2];
    double *ring;
    PeakWindow peaks[2];
    double gain_db[2], energy[2], attack, fast, slow, energy_a, ceiling;
};
static double biquad(Biquad *s, double x) {
    double y = s->b0*x+s->z1;
    s->z1 = s->b1*x-s->a1*y+s->z2;
    s->z2 = s->b2*x-s->a2*y;
    if (fabs(s->z1)<1e-30) s->z1=0;
    if (fabs(s->z2)<1e-30) s->z2=0;
    return y;
}
static double peak_window(PeakWindow *s, int64_t clock, double value) {
    while (s->head != s->tail && s->time[s->head] < clock-s->window)
        s->head = (s->head+1)%s->capacity;
    while (s->head != s->tail) {
        int last = (s->tail+s->capacity-1)%s->capacity;
        if (s->peak[last] > value) break;
        s->tail = last;
    }
    s->time[s->tail]=clock; s->peak[s->tail]=value;
    s->tail=(s->tail+1)%s->capacity;
    return s->peak[s->head];
}
LumenBands *lumenbands_create(int rate, int channels, double lookahead_ms) {
    LumenBands *s;
    double k, n;
    int c, j;
    if (rate<8000 || rate>192000 || channels<1 || channels>8 ||
        !isfinite(lookahead_ms) || lookahead_ms<1 || lookahead_ms>8) return NULL;
    s=calloc(1,sizeof(*s)); if (!s) return NULL;
    s->rate=rate; s->channels=channels;
    s->delay=(int)ceil(rate*lookahead_ms/1000);
    s->capacity=s->delay+1;
    s->ring=calloc((size_t)s->capacity*channels*2,sizeof(double));
    for (j=0;j<2;j++) {
        s->peaks[j].window=s->delay+(int)ceil(rate*.025);
        s->peaks[j].capacity=s->peaks[j].window+32;
        s->peaks[j].peak=calloc(s->peaks[j].capacity,sizeof(double));
        s->peaks[j].time=calloc(s->peaks[j].capacity,sizeof(int64_t));
        if (!s->peaks[j].peak || !s->peaks[j].time) { lumenbands_destroy(s); return NULL; }
    }
    if (!s->ring) { lumenbands_destroy(s); return NULL; }
    k=tan(3.14159265358979323846*120/rate);
    n=1/(1+sqrt(2)*k+k*k);
    for(c=0;c<channels;c++) for(j=0;j<2;j++) {
        Biquad *low=&s->low[c][j], *high=&s->high[c][j];
        low->b0=k*k*n; low->b1=2*low->b0; low->b2=low->b0;
        high->b0=n; high->b1=-2*n; high->b2=n;
        low->a1=high->a1=2*(k*k-1)*n;
        low->a2=high->a2=(1-sqrt(2)*k+k*k)*n;
    }
    s->attack=exp(-8.0/s->delay);
    s->fast=exp(-1.0/(rate*.04));
    s->energy_a=exp(-1.0/(rate*.1));
    lumenbands_settings(s,-1.75,1,500);
    return s;
}
void lumenbands_destroy(LumenBands *s) {
    if (!s) return;
    for(int j=0;j<2;j++) { free(s->peaks[j].peak); free(s->peaks[j].time); }
    free(s->ring); free(s);
}
int lumenbands_latency(const LumenBands *s) { return s->delay; }
void lumenbands_settings(LumenBands *s,double ceiling_db,double knee_db,double release_ms) {
    /* Reserve the final stage's knee so the band sum doesn't repeatedly duck
     * the vocal band. High-band peaks get priority; unused budget goes to bass.
     */
    s->ceiling=pow(10,(ceiling_db-knee_db*.5)/20);
    s->slow=exp(-1.0/(s->rate*release_ms/1000));
}
void lumenbands_process(LumenBands *s,const double *input,double *output,
                        double *low_gr,double *high_gr) {
    double peak[2]={0}, energy[2]={0}, gain[2], budget[2];
    int read=(s->write+1)%s->capacity;
    for(int c=0;c<s->channels;c++) {
        double low=biquad(&s->low[c][1],biquad(&s->low[c][0],input[c]));
        double high=biquad(&s->high[c][1],biquad(&s->high[c][0],input[c]));
        s->ring[((size_t)s->write*s->channels+c)*2]=low;
        s->ring[((size_t)s->write*s->channels+c)*2+1]=high;
        peak[0]=fmax(peak[0],fabs(low)); peak[1]=fmax(peak[1],fabs(high));
        energy[0]+=low*low; energy[1]+=high*high;
    }
    for(int j=0;j<2;j++) {
        peak[j]=peak_window(&s->peaks[j],s->clock,peak[j]);
        s->energy[j]=s->energy_a*s->energy[j]+(1-s->energy_a)*energy[j]/s->channels;
    }
    budget[1]=s->ceiling;
    budget[0]=fmax(s->ceiling-fmin(peak[1],s->ceiling*.97),
                   s->ceiling*LUMEN_BUDGET_FLOOR);
    for(int j=0;j<2;j++) {
        double required=-fmax(20*log10(fmax(peak[j],1e-30)/budget[j]),0);
        double crest=peak[j]/sqrt(fmax(s->energy[j],1e-30));
        /* Bass uses the sustained stage. The high band holds complete cycles
         * in its peak window, then releases quickly so a startup transient
         * does not leave a long level drift on an otherwise steady vocal tone.
         */
        double excess=required-s->gain_db[j];
        /* First undo excessive attenuation from an isolated crossover startup
         * spike quickly, then use the slow stage for the final half-dB settle.
         * Keeping the whole recovery slow made bass swell for several seconds.
         */
        double coeff=required<s->gain_db[j] ? s->attack :
                     (crest>4 || j==1 || excess>.5 ? s->fast : s->slow);
        s->gain_db[j]=required+coeff*(s->gain_db[j]-required);
        gain[j]=pow(10,s->gain_db[j]/20);
    }
    for(int c=0;c<s->channels;c++)
        output[c]=s->ring[((size_t)read*s->channels+c)*2]*gain[0]+
                  s->ring[((size_t)read*s->channels+c)*2+1]*gain[1];
    *low_gr=-s->gain_db[0]; *high_gr=-s->gain_db[1];
    s->write=read; s->clock++;
}
