// =============================================================================
// host_dsp.c : run the board's DSP code (Vitis_Code/common/lfsp_dsp.c) on a PC
// =============================================================================
//
// Reads decimated samples (one unsigned 16-bit value per line, 512 per block)
// and prints one CSV line per block and per path (q15 / float):
//
//   block,path,rumble_e,mains_e,total_e,rumble_pm,mains_pm,rms,classify,detect
//
// Build and run (from the repo root):
//   cc -O2 -Wall -Wextra -IVitis_Code/common -o host_dsp \
//      Signal_Processing/host_test/host_dsp.c Vitis_Code/common/lfsp_dsp.c -lm
//   ./host_dsp blocks.txt > host_out.csv
//
// Normally run_pipeline_test.py does this for you.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include "lfsp_dsp.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s blocks.txt\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }

    dsp_cfg_t cfg;
    dsp_init(&cfg, DSP_FS_HZ);
    fprintf(stderr, "bins: rumble k=%d..%d, mains k=%d..%d (%d Goertzel filters)\n",
            cfg.rumble_lo, cfg.rumble_hi, cfg.mains_lo, cfg.mains_hi, cfg.nbins);

    det_state_t st_q, st_f;
    dsp_detect_init(&st_q);
    dsp_detect_init(&st_f);

    printf("block,path,rumble_e,mains_e,total_e,rumble_pm,mains_pm,rms,classify,detect\n");

    uint16_t blk[DSP_N];
    unsigned v;
    int n = 0, b = 0;
    while (fscanf(f, "%u", &v) == 1) {
        blk[n++] = (uint16_t)v;
        if (n < DSP_N)
            continue;
        n = 0;

        dsp_result_t rq, rf;
        dsp_block_q15(&cfg, blk, &rq);
        dsp_block_float(&cfg, blk, &rf);
        det_label_t dq = dsp_detect(&st_q, &rq);
        det_label_t df = dsp_detect(&st_f, &rf);

        printf("%d,q15,%.6e,%.6e,%.6e,%u,%u,%u,%s,%s\n", b, rq.rumble_e, rq.mains_e, rq.total_e,
               rq.rumble_pm, rq.mains_pm, rq.rms, dsp_label_name(dsp_classify(&rq)), dsp_label_name(dq));
        printf("%d,float,%.6e,%.6e,%.6e,%u,%u,%u,%s,%s\n", b, rf.rumble_e, rf.mains_e, rf.total_e,
               rf.rumble_pm, rf.mains_pm, rf.rms, dsp_label_name(dsp_classify(&rf)), dsp_label_name(df));
        b++;
    }
    fclose(f);
    if (n != 0)
        fprintf(stderr, "warning: %d trailing samples ignored (not a full block)\n", n);
    return 0;
}
