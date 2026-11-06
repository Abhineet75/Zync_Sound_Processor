// =============================================================================
// Days 4-5 : real-time detection on the board
// =============================================================================
//
// Same data movement as Day 3 (DMA ping-pong, ISR re-arms first, cache
// invalidate before reading). Each 512-sample block now goes through
// lfsp_dsp.c: DC removal, Goertzel bank (rumble 9-41 Hz, mains 47-52 Hz),
// band shares and detection with a 2-block hold.
//
// Files in the Vitis src folder: main.c (this), lfsp_dsp.c, lfsp_dsp.h
//
// Switches (read every block, no restart needed)
//   SW1 = 0 : Q15 fixed-point processing     SW1 = 1 : float processing
// Switch read at start (re-run after changing it)
//   SW0 = 0 : SELF-TEST. The built-in test pattern plays a sequence of
//             triangle tones and silence; the program checks each detection
//             against the expected label and measures the latency.
//   SW0 = 1 : XADC. Real input on A0 from the function generator.
//
// Outputs
//   UART  one line per block: path, band shares, RMS, verdict, processing time
//   JA2   busy: high while a block is processed  (pulse width = processing time)
//   JA3   toggles in the DMA interrupt (one edge per block)
//   JA4   high while "RUMBLE" is detected
//   LD0   on while "RUMBLE" is detected
//   JA1   PWM duty = rumble share: after the RC filter the voltage rises
//         when the input is in the rumble band and falls outside it
//
// Compile-time options
//   USE_CACHE_INVALIDATE 0 : cache-coherency bug demo (stale blocks)
// =============================================================================

#include "xparameters.h"
#include "xaxidma.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xil_exception.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "lfsp_dsp.h"

#define N                     DSP_N
#define USE_CACHE_INVALIDATE  1

#ifndef XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR
#define XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR 61U
#endif
#define DMA_IRQ_ID            XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR

// gpio_ctrl ch2 fields
#define CTRL_ENABLE           0x1u
#define CTRL_SEL_PATTERN      0x2u
#define CTRL_MODE(m)          (((m) & 3u) << 2)
#define MODE_TRIANGLE         1u
#define MODE_CONST            3u

// gpio_out ch2 pins
#define PIN_BUSY              0x1u         // JA2
#define PIN_BUF               0x2u         // JA3
#define PIN_DET               0x4u         // JA4
#define PIN_LED               0x8u         // LD0

// gpio_in ch2 switches
#define SW_XADC               0x1u         // SW0
#define SW_FLOAT              0x2u         // SW1

// Test pattern raw sample rate: 100 MHz / 104 clocks, same as the XADC
#define PATTERN_FS_RAW        961538.46

// =============================================================================
// Self-test sequence
// =============================================================================
typedef struct {
    const char *name;
    u32         mode;                      // MODE_TRIANGLE or MODE_CONST
    u32         freq_hz;
    int         blocks;                    // how long to play it (~0.545 s each)
    det_label_t expect;
} step_t;

static const step_t steps[] = {
    { "triangle 20 Hz",  MODE_TRIANGLE,  20, 8, DET_RUMBLE },
    { "silence",         MODE_CONST,      0, 5, DET_QUIET  },
    { "triangle 50 Hz",  MODE_TRIANGLE,  50, 8, DET_MAINS  },
    { "triangle 100 Hz", MODE_TRIANGLE, 100, 8, DET_NONE   },
    { "triangle 10 Hz",  MODE_TRIANGLE,  10, 8, DET_RUMBLE },
    { "triangle 45 Hz",  MODE_TRIANGLE,  45, 8, DET_NONE   },
    { "triangle 35 Hz",  MODE_TRIANGLE,  35, 8, DET_RUMBLE },
    { "silence",         MODE_CONST,      0, 5, DET_QUIET  },
};
#define NSTEPS ((int)(sizeof(steps) / sizeof(steps[0])))

static u32 phase_inc_for(u32 f_hz)
{
    return (u32)((double)f_hz * 4294967296.0 / PATTERN_FS_RAW + 0.5);
}

// =============================================================================
// Hardware and ISR state (as in Day 3)
// =============================================================================
static XAxiDma dma;
static XGpio   gpio_out, gpio_ctrl, gpio_in;
static XScuGic gic;

static u16 buf[2][N] __attribute__((aligned(32)));

static volatile int ready  = -1;
static volatile int in_use = -1;
static int          filling = 0;
static volatile u32 irq_blocks = 0, overruns = 0, dma_errors = 0;
static volatile u32 pins = 0;
static XTime        t_irq_last = 0;        // timestamp of the latest block-complete IRQ

static void pins_set(u32 mask, int on)
{
    Xil_ExceptionDisable();
    pins = on ? (pins | mask) : (pins & ~mask);
    XGpio_DiscreteWrite(&gpio_out, 2, pins);
    Xil_ExceptionEnable();
}

static void dma_isr(void *ref)
{
    (void)ref;
    u32 irq = XAxiDma_IntrGetIrq(&dma, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrAckIrq(&dma, irq, XAXIDMA_DEVICE_TO_DMA);

    if (irq & XAXIDMA_IRQ_ERROR_MASK) {
        dma_errors++;
        return;
    }
    if (!(irq & XAXIDMA_IRQ_IOC_MASK))
        return;

    int done = filling;
    filling ^= 1;
    XAxiDma_SimpleTransfer(&dma, (UINTPTR)buf[filling], sizeof(buf[0]),
                           XAXIDMA_DEVICE_TO_DMA);
    if (ready != -1 || in_use == filling)
        overruns++;
    ready = done;

    XTime_GetTime(&t_irq_last);
    irq_blocks++;

    pins ^= PIN_BUF;
    XGpio_DiscreteWrite(&gpio_out, 2, pins);
}

static u32 ticks_to_us(XTime t) { return (u32)((t * 1000000ULL) / COUNTS_PER_SECOND); }
static u32 ticks_to_ms(XTime t) { return (u32)((t * 1000ULL) / COUNTS_PER_SECOND); }

static int init_gpio(XGpio *g, u16 id, const char *name)
{
    if (XGpio_Initialize(g, id) != XST_SUCCESS) {
        xil_printf("%s init failed\r\n", name);
        return -1;
    }
    return 0;
}

// Point the test pattern at a new tone while everything keeps running
static void pattern_play(const step_t *s)
{
    u32 ctrl = CTRL_ENABLE | CTRL_SEL_PATTERN | CTRL_MODE(s->mode);
    XGpio_DiscreteWrite(&gpio_ctrl, 1, phase_inc_for(s->freq_hz));
    XGpio_DiscreteWrite(&gpio_ctrl, 2, ctrl);
}

// =============================================================================
// main
// =============================================================================
int main(void)
{
    xil_printf("\r\n=== Days 4-5: Goertzel detection (N=%d) ===\r\n", N);

    // ---- GPIO ------------------------------------------------------------------
    if (init_gpio(&gpio_out,  XPAR_GPIO_OUT_DEVICE_ID,  "gpio_out")  ||
        init_gpio(&gpio_ctrl, XPAR_GPIO_CTRL_DEVICE_ID, "gpio_ctrl") ||
        init_gpio(&gpio_in,   XPAR_GPIO_IN_DEVICE_ID,   "gpio_in"))
        return -1;
    XGpio_SetDataDirection(&gpio_out,  1, 0x0);
    XGpio_SetDataDirection(&gpio_out,  2, 0x0);
    XGpio_SetDataDirection(&gpio_ctrl, 1, 0x0);
    XGpio_SetDataDirection(&gpio_ctrl, 2, 0x0);
    XGpio_SetDataDirection(&gpio_in,   1, 0xFFFFFFFF);
    XGpio_SetDataDirection(&gpio_in,   2, 0xFFFFFFFF);
    XGpio_DiscreteWrite(&gpio_ctrl, 2, 0);
    XGpio_DiscreteWrite(&gpio_out, 1, 0);
    XGpio_DiscreteWrite(&gpio_out, 2, 0);

    int self_test = !(XGpio_DiscreteRead(&gpio_in, 2) & SW_XADC);
    xil_printf("Source : %s\r\n", self_test ? "SELF-TEST pattern sequence  [SW0 = 0]"
                                            : "XADC (A0, generator)  [SW0 = 1]");
    xil_printf("SW1 selects the processing path: 0 = Q15, 1 = float\r\n");
    xil_printf("Cache invalidate: %s\r\n", USE_CACHE_INVALIDATE ? "ON" : "OFF (bug demo)");

    // ---- DSP setup ---------------------------------------------------------------
    static dsp_cfg_t cfg;
    dsp_init(&cfg, DSP_FS_HZ);
    xil_printf("Bins: rumble k=%d..%d, mains k=%d..%d (%d Goertzel filters per block)\r\n",
               cfg.rumble_lo, cfg.rumble_hi, cfg.mains_lo, cfg.mains_hi, cfg.nbins);
    det_state_t det;
    dsp_detect_init(&det);

    // ---- AXI DMA ------------------------------------------------------------------
    XAxiDma_Config *dcfg = XAxiDma_LookupConfig(XPAR_AXIDMA_0_DEVICE_ID);
    if (dcfg == NULL || XAxiDma_CfgInitialize(&dma, dcfg) != XST_SUCCESS || XAxiDma_HasSg(&dma)) {
        xil_printf("DMA init failed\r\n");
        return -1;
    }
    XAxiDma_IntrDisable(&dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

    // ---- Interrupt controller -----------------------------------------------------
    XScuGic_Config *gcfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    if (gcfg == NULL ||
        XScuGic_CfgInitialize(&gic, gcfg, gcfg->CpuBaseAddress) != XST_SUCCESS) {
        xil_printf("GIC init failed\r\n");
        return -1;
    }
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler, &gic);
    XScuGic_SetPriorityTriggerType(&gic, DMA_IRQ_ID, 0xA0, 0x3);
    if (XScuGic_Connect(&gic, DMA_IRQ_ID, (Xil_InterruptHandler)dma_isr, &dma) != XST_SUCCESS) {
        xil_printf("GIC connect failed\r\n");
        return -1;
    }
    XScuGic_Enable(&gic, DMA_IRQ_ID);
    Xil_ExceptionEnable();
    XAxiDma_IntrEnable(&dma, (XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_ERROR_MASK),
                       XAXIDMA_DEVICE_TO_DMA);

    // ---- Start: arm DMA first, then enable the decimator ------------------------
    Xil_DCacheFlushRange((UINTPTR)buf, sizeof(buf));
    int step = 0, step_blocks = 0, step_detected = 0;
    XTime t_step = 0;

    if (self_test) {
        XGpio_DiscreteWrite(&gpio_ctrl, 1, phase_inc_for(steps[0].freq_hz));
        XGpio_DiscreteWrite(&gpio_ctrl, 2, CTRL_SEL_PATTERN | CTRL_MODE(steps[0].mode));
    }
    filling = 0;
    XAxiDma_SimpleTransfer(&dma, (UINTPTR)buf[0], sizeof(buf[0]), XAXIDMA_DEVICE_TO_DMA);
    if (self_test) {
        pattern_play(&steps[0]);
        xil_printf("\r\n--- step 1/%d: %s, expect %s ---\r\n", NSTEPS, steps[0].name,
                   dsp_label_name(steps[0].expect));
    } else {
        XGpio_DiscreteWrite(&gpio_ctrl, 2, CTRL_ENABLE);         // sel = 0: XADC
    }
    XTime_GetTime(&t_step);

    // ---- Main loop -------------------------------------------------------------------
    u32 blocks = 0, proc_max_us[2] = {0, 0};
    int pass_count = 0, round_no = 1;

    while (1) {
        if (ready < 0)
            continue;

        Xil_ExceptionDisable();
        int   b     = ready;
        XTime t_blk = t_irq_last;
        ready  = -1;
        in_use = b;
        Xil_ExceptionEnable();

        int use_float = (XGpio_DiscreteRead(&gpio_in, 2) & SW_FLOAT) != 0;

        // ---- Process: everything between busy high and busy low ------------------
        pins_set(PIN_BUSY, 1);
        XTime tp0, tp1;
        XTime_GetTime(&tp0);
#if USE_CACHE_INVALIDATE
        Xil_DCacheInvalidateRange((UINTPTR)buf[b], sizeof(buf[b]));
#endif
        dsp_result_t res;
        if (use_float) dsp_block_float(&cfg, buf[b], &res);
        else           dsp_block_q15(&cfg, buf[b], &res);
        det_label_t label = dsp_detect(&det, &res);

        XGpio_DiscreteWrite(&gpio_out, 1, (res.rumble_pm * 255u) / 1000u);   // PWM
        pins_set(PIN_DET | PIN_LED, label == DET_RUMBLE);
        XTime_GetTime(&tp1);
        in_use = -1;
        pins_set(PIN_BUSY, 0);

        u32 us = ticks_to_us(tp1 - tp0);
        if (us > proc_max_us[use_float]) proc_max_us[use_float] = us;
        blocks++;

        // ---- Report (outside the busy window) -------------------------------------
        xil_printf("blk %5d %-5s rumble %4d  mains %4d  rms %5d  -> %-9s  proc %5d us\r\n",
                   blocks, use_float ? "float" : "Q15", res.rumble_pm, res.mains_pm,
                   res.rms, dsp_label_name(label), us);

        if (blocks % 20 == 0)
            xil_printf("    [overruns %d  dma_err %d  drops %d  max proc Q15 %d us / float %d us]\r\n",
                       overruns, dma_errors, XGpio_DiscreteRead(&gpio_in, 1),
                       proc_max_us[0], proc_max_us[1]);

        if (!self_test)
            continue;

        // ---- Self-test bookkeeping ---------------------------------------------------
        step_blocks++;
        if (!step_detected && label == steps[step].expect) {
            step_detected = 1;
            xil_printf("    >> %s detected after %d ms (%d blocks)\r\n",
                       dsp_label_name(label), ticks_to_ms(t_blk - t_step), step_blocks);
        }
        if (step_blocks >= steps[step].blocks) {
            int ok = step_detected && label == steps[step].expect;
            pass_count += ok;
            xil_printf("    << step %d %s: %s\r\n", step + 1, steps[step].name, ok ? "PASS" : "FAIL");

            step++;
            if (step == NSTEPS) {
                xil_printf("\r\n===== SELF-TEST round %d: %d / %d steps PASS =====\r\n",
                           round_no, pass_count, NSTEPS);
                xil_printf("      max processing time: Q15 %d us, float %d us (flip SW1 to measure both)\r\n\r\n",
                           proc_max_us[0], proc_max_us[1]);
                step = 0;
                pass_count = 0;
                round_no++;
            }
            step_blocks = 0;
            step_detected = 0;
            pattern_play(&steps[step]);
            XTime_GetTime(&t_step);
            xil_printf("\r\n--- step %d/%d: %s, expect %s ---\r\n", step + 1, NSTEPS,
                       steps[step].name, dsp_label_name(steps[step].expect));
        }
    }

    return 0;
}
