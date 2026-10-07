/* Native-only offline CPU diagnostic; no audio device, files or app preferences. */
#include "lumenout_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double cpu_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)) return -1;
    return t.tv_sec*1000.0+t.tv_nsec/1e6;
}
int main(void) {
    const int rate=48000, frames=rate*6;
    double *input=calloc((size_t)frames*2,sizeof(double)), output[1024*2];
    double checksum=0;
    if(!input) return 2;
    for(int n=0;n<frames;n++) {
        double t=(double)n/rate;
        input[n*2]=2*sin(2*3.14159265358979323846*60*t)+.2*sin(2*3.14159265358979323846*1000*t);
        input[n*2+1]=input[n*2]*.7;
    }
    for(int round=0;round<2;round++) for(int bands=1;bands<=2;bands++) for(int clip=0;clip<=1;clip++) {
        LumenOut *s=lumenout_create(rate,2,3);
        LumenOutMeter m;
        double start,end;
        if(!s || lumenout_configure_bands(s,bands) || lumenout_configure_clip(s,clip)) return 3;
        start=cpu_ms(); if(start<0) return 4;
        for(int pos=0;pos<frames;pos+=1024) {
            int n=frames-pos<1024 ? frames-pos : 1024;
            lumenout_process(s,input+pos*2,output,n,&m);
            checksum+=output[0];
        }
        end=cpu_ms(); if(end<0) return 4;
        printf("{\"round\":%d,\"bands\":%d,\"clip\":%d,\"audio_ms\":6000,\"cpu_ms\":%.6f,\"one_core_percent\":%.6f,\"latency_samples\":%d}\n",
               round,bands,clip,end-start,(end-start)/60,lumenout_latency(s));
        lumenout_destroy(s);
    }
    free(input);
    if(!isfinite(checksum)) return 5;
    return 0;
}
