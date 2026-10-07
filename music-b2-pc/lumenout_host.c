/* Offline raw-double harness. Never deployed to Android. */
#include "lumenout_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    FILE *in, *out;
    LumenOut *s;
    LumenOutMeter m;
    double input[1024 * 8], output[1024 * 8], gr_max = 0, tp_max = -600;
    double clip_max=0, low_max=0, high_max=0;
    int rate, channels, block, latency, discard, n, used;
    size_t count;
    unsigned long long total = 0;
    if (argc < 7 || argc > 9) {
        fprintf(stderr, "usage: lumenout-host input.f64 output.f64 rate channels block lookahead-ms [bands] [clip]\n");
        return 2;
    }
    rate = atoi(argv[3]); channels = atoi(argv[4]); block = atoi(argv[5]);
    if (block < 1 || block > 1024) return 2;
    s = lumenout_create(rate, channels, atof(argv[6]));
    if (!s) return 2;
    if (argc >= 8 && lumenout_configure_bands(s,atoi(argv[7]))<0) return 2;
    if (argc == 9 && lumenout_configure_clip(s,atoi(argv[8]))<0) return 2;
    in = fopen(argv[1], "rb"); out = fopen(argv[2], "wb");
    if (!in || !out) return 3;
    latency = discard = lumenout_latency(s);
    while ((count = fread(input, sizeof(double), (size_t)block * channels, in)) > 0) {
        if (count % channels) return 4;
        n = (int)(count / channels); total += n;
        lumenout_process(s, input, output, n, &m);
        if (m.gr_db > gr_max) gr_max = m.gr_db;
        if (m.tp_out > tp_max) tp_max = m.tp_out;
        if (m.clip_db>clip_max) clip_max=m.clip_db;
        if (m.low_gr_db>low_max) low_max=m.low_gr_db;
        if (m.high_gr_db>high_max) high_max=m.high_gr_db;
        used = discard < n ? discard : n; discard -= used;
        if (fwrite(output + used * channels, sizeof(double), (n - used) * channels, out)
            != (size_t)(n - used) * channels) return 5;
    }
    /* Feed silence to drain every original sample, including short inputs. */
    memset(input, 0, sizeof(input));
    while (latency > 0) {
        n = latency < block ? latency : block; latency -= n;
        lumenout_process(s, input, output, n, &m);
        used = discard < n ? discard : n; discard -= used;
        if (fwrite(output + used * channels, sizeof(double), (n - used) * channels, out)
            != (size_t)(n - used) * channels) return 5;
    }
    fclose(in); if (fclose(out)) return 5;
    printf("{\"frames\":%llu,\"latency_samples\":%d,\"max_gr_db\":%.9f,\"tp_estimate_dbfs\":%.9f,\"max_clip_db\":%.9f,\"max_low_gr_db\":%.9f,\"max_high_gr_db\":%.9f}\n",
           total, lumenout_latency(s), gr_max, tp_max, clip_max, low_max, high_max);
    lumenout_destroy(s); return 0;
}
