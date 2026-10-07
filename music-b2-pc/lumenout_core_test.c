/* Meaningful runtime-state checks for the isolated portable core. */
#include "lumenout_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    LumenOut *s = lumenout_create(48000, 2, 3);
    LumenOutMeter m;
    const int total = 5000, command = 2000, ramp = 1200;
    double *input = calloc(total * 2, sizeof(double));
    double *output = calloc(total * 2, sizeof(double));
    int i, delay;
    double target = pow(10, -6.0 / 20);
    CHECK(s && input && output);
    CHECK(!lumenout_create(0, 2, 3));
    CHECK(!lumenout_create(48000, 9, 3));
    CHECK(!lumenout_create(48000, 2, NAN));
    CHECK(lumenout_set_volume(s, NAN) < 0);
    CHECK(lumenout_set_volume(s, 13) < 0);
    CHECK(lumenout_set_ceiling(s, 1) < 0);
    CHECK(lumenout_set_release(s, 299) < 0);
    CHECK(lumenout_set_knee(s, -1) < 0);
    CHECK(lumenout_set_protection(s,2)<0);
    CHECK(lumenout_set_muted(s,-1)<0);
    CHECK(lumenout_configure_bands(s,3)<0);
    CHECK(lumenout_configure_clip(s,2)<0);
    CHECK(lumenout_prime_volume(s,-6)==0);
    CHECK(lumenout_prime_volume(s,0)==0);
    delay = lumenout_latency(s);
    for (i = 0; i < total; i++) { input[2*i] = .1; input[2*i+1] = -.07; }
    lumenout_process(s, input, output, command, &m);
    CHECK(lumenout_prime_volume(s,0)<0);
    CHECK(lumenout_configure_bands(s,2)<0);
    CHECK(lumenout_configure_clip(s,1)<0);
    CHECK(m.gr_db == 0 && m.gr_avg == 0);
    CHECK(lumenout_set_volume(s, -6) == 0);
    /* Changing derived release/knee state must not reset the delay line. */
    CHECK(lumenout_set_release(s, 600) == 0);
    CHECK(lumenout_set_knee(s, 2) == 0);
    lumenout_process(s, input + 2*command, output + 2*command, total-command, &m);
    for (i = 0; i < total; i++) {
        int source = i-delay;
        double v = 1, expected;
        if (source >= command) {
            int position = source-command+1;
            v = position < ramp ? 1+(target-1)*position/ramp : target;
        }
        expected = source < 0 ? 0 : .1*v;
        CHECK(fabs(output[2*i]-expected) < 1e-13);
        CHECK(fabs(output[2*i+1]+.7*expected) < 1e-13);
    }
    lumenout_destroy(s); free(input); free(output);
    /* Neutral processing must preserve a hot master, not lower flat EQ/DSP. */
    s=lumenout_create(48000,2,3); CHECK(s);
    CHECK(lumenout_set_protection(s,0)==0);
    double neutral_in[5000*2], neutral_out[5000*2];
    for(int n=0;n<5000;n++) {
        neutral_in[n*2]=.999*sin(2*3.14159265358979323846*1000*n/48000);
        neutral_in[n*2+1]=-neutral_in[n*2];
    }
    delay=lumenout_latency(s);
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    for(int n=delay;n<5000;n++) {
        CHECK(neutral_out[n*2]==neutral_in[(n-delay)*2]);
        CHECK(neutral_out[n*2+1]==neutral_in[(n-delay)*2+1]);
    }
    CHECK(m.gr_db==0);
    lumenout_destroy(s);
    /* Mute is exactly zero; volume commands while muted cannot unmute it. */
    s=lumenout_create(48000,2,3); CHECK(s);
    CHECK(lumenout_set_protection(s,0)==0);
    CHECK(lumenout_prime_volume(s,-6)==0);
    CHECK(lumenout_set_muted(s,1)==0);
    for(int n=0;n<5000*2;n++) neutral_in[n]=.1;
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    for(int n=0;n<5000*2;n++) CHECK(neutral_out[n]==0);
    CHECK(lumenout_set_volume(s,-12)==0);
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    for(int n=0;n<5000*2;n++) CHECK(neutral_out[n]==0);
    CHECK(lumenout_set_muted(s,0)==0);
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    for(int n=4500*2;n<5000*2;n++) CHECK(fabs(neutral_out[n]-.1*pow(10,-12.0/20))<1e-13);
    CHECK(lumenout_set_muted(s,1)==0);
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    for(int n=4500*2;n<5000*2;n++) CHECK(neutral_out[n]==0);
    lumenout_destroy(s);
    /* A lowered runtime ceiling must not interpret conservative quiet-input
     * bounds as actual peaks, nor reset the signal history. */
    s=lumenout_create(48000,2,3); CHECK(s);
    for(int n=0;n<5000;n++) {
        neutral_in[2*n]=.1*sin(2*3.14159265358979323846*1000*n/48000);
        neutral_in[2*n+1]=-neutral_in[2*n];
    }
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    CHECK(lumenout_set_ceiling(s,-6)==0);
    CHECK(lumenout_set_knee(s,2)==0);
    for(int block=0;block<4;block++) {
        lumenout_process(s,neutral_in,neutral_out,5000,&m);
        CHECK(m.gr_db==0 && m.gr_avg==0);
        for(int n=0;n<5000*2;n++) CHECK(fabs(neutral_out[n])<=.100000000001);
    }
    /* Enable protection before a boost: after settling, hot input is limited;
     * disabling protection must release back to the original level. */
    CHECK(lumenout_set_protection(s,0)==0);
    lumenout_process(s,neutral_in,neutral_out,5000,&m);
    CHECK(lumenout_set_protection(s,1)==0);
    for(int n=0;n<5000;n++) {
        neutral_in[2*n]=2*sin(2*3.14159265358979323846*1000*n/48000);
        neutral_in[2*n+1]=-neutral_in[2*n];
    }
    for(int block=0;block<10;block++) lumenout_process(s,neutral_in,neutral_out,5000,&m);
    CHECK(m.gr_db>6);
    for(int n=0;n<5000*2;n++) CHECK(fabs(neutral_out[n])<pow(10,-6.0/20));
    CHECK(lumenout_set_protection(s,0)==0);
    for(int block=0;block<20;block++) lumenout_process(s,neutral_in,neutral_out,5000,&m);
    CHECK(m.gr_db<.001);
    double maximum_neutral=0;
    for(int n=0;n<5000*2;n++) if(fabs(neutral_out[n])>maximum_neutral) maximum_neutral=fabs(neutral_out[n]);
    CHECK(maximum_neutral>1.99);
    lumenout_destroy(s);
    /* Mute must also silence already-delayed PCM and crossover histories,
     * not just prevent new input. Exercise every optional configuration. */
    for (int bands=1;bands<=2;bands++) for(int clip=0;clip<=1;clip++) {
        s=lumenout_create(48000,2,3); CHECK(s);
        CHECK(lumenout_configure_bands(s,bands)==0);
        CHECK(lumenout_configure_clip(s,clip)==0);
        for(int n=0;n<5000;n++) {
            neutral_in[2*n]=.1*sin(2*3.14159265358979323846*997*n/48000);
            neutral_in[2*n+1]=-.7*neutral_in[2*n];
        }
        lumenout_process(s,neutral_in,neutral_out,5000,&m);
        CHECK(lumenout_set_muted(s,1)==0);
        lumenout_process(s,neutral_in,neutral_out,5000,&m);
        for(int n=0;n<10000;n++) CHECK(neutral_out[n]==0);
        CHECK(lumenout_set_volume(s,-12)==0);
        lumenout_process(s,neutral_in,neutral_out,5000,&m);
        for(int n=0;n<10000;n++) CHECK(neutral_out[n]==0);
        CHECK(lumenout_set_muted(s,0)==0);
        lumenout_process(s,neutral_in,neutral_out,5000,&m);
        for(int n=0;n<10000;n++) CHECK(isfinite(neutral_out[n]));
        double unmute_peak=0;
        for(int n=5000;n<10000;n++) unmute_peak=fmax(unmute_peak,fabs(neutral_out[n]));
        CHECK(unmute_peak>.01 && unmute_peak<.04);
        lumenout_destroy(s);
    }
    /* Every supported rate and channel count, silence and invalid samples. */
    for (i = 0; i < 4; i++) for(int bands=1;bands<=2;bands++) for(int clip=0;clip<=1;clip++) {
        int rates[] = {8000, 44100, 48000, 192000};
        double x[8] = {NAN, INFINITY, -INFINITY, 0, 0, 0, 0, 0}, y[8];
        int n;
        s = lumenout_create(rates[i], 8, 8); CHECK(s);
        CHECK(lumenout_configure_bands(s,bands)==0);
        CHECK(lumenout_configure_clip(s,clip)==0);
        for (n = 0; n < lumenout_latency(s)+32; n++) {
            lumenout_process(s, x, y, 1, &m);
            for (int c = 0; c < 8; c++) CHECK(y[c] == 0);
            CHECK(isfinite(m.tp_out) && isfinite(m.gr_db));
        }
        lumenout_destroy(s);
    }
    puts("PASS: volume ramp, stereo linking, runtime history, invalid commands, finite output, rates/channels");
    return 0;
}
