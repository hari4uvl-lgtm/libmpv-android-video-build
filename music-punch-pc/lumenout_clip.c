/* Four-times interpolated nonlinear processing, anti-alias FIR, decimation.
 * Clip contribution is deliberately <0.5 dB; final limiter handles larger overs.
 * Optional and off by default; no allocations/work when absent.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "lumenout_clip.h"
#include <math.h>
#include <stdlib.h>
#define UP_TAPS 32
#define DOWN_TAPS 65
struct LumenClip {
    int channels, up_pos, down_pos;
    double up_fir[4][UP_TAPS], down_fir[DOWN_TAPS];
    double up_history[8][UP_TAPS], down_history[8][DOWN_TAPS];
    double threshold, delta;
    double energy[8], energy_a;
};
static double i0(double x) {
    double sum=1, term=1;
    for(int n=1;n<40;n++) {
        term*=x*x/(4*n*n); sum+=term;
        if(term<sum*1e-16) break;
    }
    return sum;
}
static double sinc(double x) {
    const double pi=3.14159265358979323846;
    return fabs(x)<1e-12 ? 1 : sin(pi*x)/(pi*x);
}
static double window(double x) {
    return fabs(x)<1 ? i0(9*sqrt(1-x*x))/i0(9) : 0;
}
LumenClip *lumenclip_create(int rate,int channels,double ceiling_db) {
    LumenClip *s;
    if(rate<8000 || rate>192000 || channels<1 || channels>8 || !isfinite(ceiling_db)) return NULL;
    s=calloc(1,sizeof(*s)); if(!s) return NULL;
    s->channels=channels;
    s->energy_a=exp(-1.0/(rate*.1));
    for(int phase=0;phase<4;phase++) {
        double norm=0;
        for(int tap=0;tap<UP_TAPS;tap++) {
            double distance=tap-15.0+phase/4.0;
            s->up_fir[phase][tap]=sinc(distance)*window(distance/16);
            norm+=s->up_fir[phase][tap];
        }
        for(int tap=0;tap<UP_TAPS;tap++) s->up_fir[phase][tap]/=norm;
    }
    double norm=0;
    for(int tap=0;tap<DOWN_TAPS;tap++) {
        double distance=tap-32.0;
        s->down_fir[tap]=.25*sinc(distance*.25)*window(distance/33);
        norm+=s->down_fir[tap];
    }
    for(int tap=0;tap<DOWN_TAPS;tap++) s->down_fir[tap]/=norm;
    lumenclip_ceiling(s,ceiling_db);
    return s;
}
void lumenclip_destroy(LumenClip *s) { free(s); }
int lumenclip_latency(const LumenClip *s) { (void)s; return 23; }
void lumenclip_ceiling(LumenClip *s,double ceiling_db) {
    double ceiling=pow(10,ceiling_db/20);
    s->threshold=ceiling*pow(10,-.5/20);
    s->delta=ceiling-s->threshold;
}
void lumenclip_process(LumenClip *s,const double *input,double *output,double *clip_db) {
    double max_gr=0;
    for(int c=0;c<s->channels;c++) {
        s->up_history[c][s->up_pos]=input[c];
        s->energy[c]=s->energy_a*s->energy[c]+(1-s->energy_a)*input[c]*input[c];
    }
    for(int phase=0;phase<4;phase++) {
        for(int c=0;c<s->channels;c++) {
            double x=0, y, a, difference=0;
            for(int tap=0;tap<UP_TAPS;tap++)
                x+=s->up_fir[phase][tap]*s->up_history[c][(s->up_pos+UP_TAPS-tap)%UP_TAPS];
            a=fabs(x);
            if(a>s->threshold) {
                double z=(a-s->threshold)/s->delta;
                /* Reserve this small nonlinear contribution for isolated
                 * spikes, not sustained boosted bass or treble. Smooth gating
                 * avoids creating a hard on/off boundary at the crest threshold.
                 */
                double crest=a/sqrt(fmax(s->energy[c],1e-30));
                double activation=fmax(0,fmin(1,(crest-3)/2));
                difference=activation*s->delta*(z*z)/(1+z*z);
                max_gr=fmax(max_gr,20*log10(a/(a-difference)));
            }
            y=copysign(a-difference,x);
            s->down_history[c][s->down_pos]=y;
            if(phase==0) {
                double filtered=0;
                for(int tap=0;tap<DOWN_TAPS;tap++)
                    filtered+=s->down_fir[tap]*s->down_history[c][(s->down_pos+DOWN_TAPS-tap)%DOWN_TAPS];
                output[c]=fabs(filtered)<1e-30 ? 0 : filtered;
            }
        }
        s->down_pos=(s->down_pos+1)%DOWN_TAPS;
    }
    s->up_pos=(s->up_pos+1)%UP_TAPS;
    *clip_db=max_gr;
}
