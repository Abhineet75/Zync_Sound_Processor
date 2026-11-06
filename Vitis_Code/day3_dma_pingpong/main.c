// =============================================================================
// Day 3 : DMA ping-pong + cache invalidation + Gate 2 (10 min, 0 lost samples)
// =============================================================================
//
// What it does
//   * The fabric (XADC or test pattern -> decim_avg -> FIFO -> AXI DMA) writes
//     512-sample blocks into DDR with no CPU copying.
//   * Two buffers, buf[0] and buf[1]. The DMA fills one while the ARM reads the
//     other. On every "block complete" interrupt the ISR immediately re-arms
//     the DMA into the other buffer, then hands the full one to main().
//   * main() invalidates the data cache for that buffer before reading it, so
//     it sees what the DMA wrote to DDR, not stale cached bytes.
//   * Scope pins (gpio_out ch2): bit0 busy (JA2) is high while main() works on
//     a block; bit1 buffer (JA3) toggles in the ISR. The hardware pin
//     blk_toggle (JA7) toggles in the decimator itself.
//
// Two modes, chosen by SW0 when the program starts
//   SW0 = 0 : TEST PATTERN. axis_test_pattern runs a ramp with phase_inc = 1024.
//             Then every decimated sample is exactly 16 more than the previous
//             one, across block boundaries too (one known exception where the
//             ramp wraps every ~4.4 s, handled below). Any lost, repeated or
//             stale sample breaks that rule and is counted.
//             After TEST_SECONDS the program prints the Gate 2 verdict.
//   SW0 = 1 : XADC (A0). No exact check; prints min / max / mean of each
//             block so you can see the generator signal arriving through DMA.
//             Decimated value = 16 x average ADC code.
//   To change mode: flip SW0, then Run As -> Launch on Hardware again.
//
// Gate 2 passes when, after 10 minutes:
//   ramp errors = 0, overruns = 0, DMA errors = 0, drop_count (hardware) = 0.
//
// Cache-bug demo (Day 5): set USE_CACHE_INVALIDATE to 0, rebuild, run in
// test-pattern mode -> ramp errors appear (stale / repeated data).
// =============================================================================

#include "xparameters.h"
#include "xaxidma.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xil_exception.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xtime_l.h"

#define N                     512          // samples per block (decim_avg BLOCK)
#define TEST_SECONDS          600          // Gate 2 run length
#define USE_CACHE_INVALIDATE  1            // 0 = deliberate cache-coherency bug
#define PRINT_EVERY_BLOCKS    4            // ~2 s between status lines

#define PATTERN_PHASE_INC     1024u        // ramp: +1 ADC code per 1024 raw samples
#define RAMP_STEP             16u          // -> +16 per decimated sample

// IRQ_F2P[0] is GIC interrupt 61 on Zynq-7000
#ifndef XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR
#define XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR 61U
#endif
#define DMA_IRQ_ID            XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR

// gpio_ctrl ch2 fields (see Vivado_Scripts/day2_add_datapath.tcl)
#define CTRL_ENABLE           0x1u         // decim_avg.enable
#define CTRL_SEL_PATTERN      0x2u         // axis_src_mux.sel: 1 = test pattern
#define CTRL_MODE(m)          (((m) & 3u) << 2)   // axis_test_pattern.mode

// gpio_out ch2 pins
#define PIN_BUSY              0x1u         // JA2
#define PIN_BUF               0x2u         // JA3
#define PIN_DET               0x4u         // JA4
#define PIN_LED               0x8u         // LD0

static XAxiDma dma;
static XGpio   gpio_out, gpio_ctrl, gpio_in;
static XScuGic gic;

// Two DMA buffers, each starting on its own 32-byte cache line
static u16 buf[2][N] __attribute__((aligned(32)));

// Shared between ISR and main()
static volatile int ready  = -1;           // buffer waiting for main(), or -1
static volatile int in_use = -1;           // buffer main() is reading, or -1
static int          filling = 0;           // buffer the DMA is writing (ISR only)
static volatile u32 irq_blocks = 0, overruns = 0, dma_errors = 0;
static volatile u32 pins = 0;
static XTime        t_first_irq = 0, t_last_irq = 0;

// =============================================================================
// Scope pins: main() and the ISR both write gpio_out ch2, so main() updates
// the shadow copy with interrupts off (otherwise the ISR's toggle can be lost)
// =============================================================================
static void pins_set(u32 mask, int on)
{
    Xil_ExceptionDisable();
    pins = on ? (pins | mask) : (pins & ~mask);
    XGpio_DiscreteWrite(&gpio_out, 2, pins);
    Xil_ExceptionEnable();
}

// =============================================================================
// DMA "block complete" interrupt: re-arm first, then hand the block over
// =============================================================================
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

    // Missed deadline: main() never took the previous block, or is still
    // reading the buffer the DMA has just been pointed at
    if (ready != -1 || in_use == filling)
        overruns++;
    ready = done;

    XTime_GetTime(&t_last_irq);
    if (irq_blocks == 0)
        t_first_irq = t_last_irq;
    irq_blocks++;

    pins ^= PIN_BUF;                       // ISR cannot be interrupted by main()
    XGpio_DiscreteWrite(&gpio_out, 2, pins);
}

// =============================================================================
// Helpers
// =============================================================================
static u32 ticks_to_us(XTime t) { return (u32)((t * 1000000ULL) / COUNTS_PER_SECOND); }

static u32 drop_count(void) { return XGpio_DiscreteRead(&gpio_in, 1); }

static u32 dma_status(void)
{
    return XAxiDma_ReadReg(dma.RegBase + XAXIDMA_RX_OFFSET, XAXIDMA_SR_OFFSET);
}

// Measured decimated sample rate in milli-Hz, from the interrupt timestamps
static u32 sample_rate_mhz(void)
{
    Xil_ExceptionDisable();
    u32   n  = irq_blocks;
    XTime dt = t_last_irq - t_first_irq;
    Xil_ExceptionEnable();
    if (n < 2 || dt == 0)
        return 0;
    return (u32)(((u64)(n - 1) * N * COUNTS_PER_SECOND * 1000ULL) / dt);
}

static int init_gpio(XGpio *g, u16 id, const char *name)
{
    if (XGpio_Initialize(g, id) != XST_SUCCESS) {
        xil_printf("%s init failed\r\n", name);
        return -1;
    }
    return 0;
}

// =============================================================================
// main
// =============================================================================
int main(void)
{
    xil_printf("\r\n=== Day 3: DMA ping-pong (N=%d) ===\r\n", N);

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

    XGpio_DiscreteWrite(&gpio_ctrl, 2, 0);          // decimator disabled
    XGpio_DiscreteWrite(&gpio_out, 1, 0);           // PWM duty 0
    XGpio_DiscreteWrite(&gpio_out, 2, 0);           // pins low

    int use_xadc = XGpio_DiscreteRead(&gpio_in, 2) & 0x1;    // SW0
    xil_printf("Source: %s\r\n", use_xadc ? "XADC (A0)  [SW0 = 1]"
                                          : "TEST PATTERN ramp  [SW0 = 0]");
    xil_printf("Cache invalidate: %s\r\n", USE_CACHE_INVALIDATE ? "ON" : "OFF (bug demo)");

    // ---- AXI DMA (simple mode, S2MM only) --------------------------------------
    XAxiDma_Config *dcfg = XAxiDma_LookupConfig(XPAR_AXIDMA_0_DEVICE_ID);
    if (dcfg == NULL || XAxiDma_CfgInitialize(&dma, dcfg) != XST_SUCCESS) {
        xil_printf("DMA init failed\r\n");
        return -1;
    }
    if (XAxiDma_HasSg(&dma)) {
        xil_printf("DMA is in scatter-gather mode; expected simple mode\r\n");
        return -1;
    }
    XAxiDma_IntrDisable(&dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

    // ---- Interrupt controller ---------------------------------------------------
    XScuGic_Config *gcfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    if (gcfg == NULL ||
        XScuGic_CfgInitialize(&gic, gcfg, gcfg->CpuBaseAddress) != XST_SUCCESS) {
        xil_printf("GIC init failed\r\n");
        return -1;
    }
    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler, &gic);
    XScuGic_SetPriorityTriggerType(&gic, DMA_IRQ_ID, 0xA0, 0x3);   // rising edge
    if (XScuGic_Connect(&gic, DMA_IRQ_ID, (Xil_InterruptHandler)dma_isr, &dma) != XST_SUCCESS) {
        xil_printf("GIC connect failed\r\n");
        return -1;
    }
    XScuGic_Enable(&gic, DMA_IRQ_ID);
    Xil_ExceptionEnable();
    XAxiDma_IntrEnable(&dma, XAXIDMA_IRQ_IOC_MASK | XAXIDMA_IRQ_ERROR_MASK,
                       XAXIDMA_DEVICE_TO_DMA);

    // ---- Start: clean cache lines, choose source, arm DMA, THEN enable ---------
    // Startup code zeroed buf through the cache; flush so no dirty line can be
    // written back over data the DMA puts in DDR later.
    Xil_DCacheFlushRange((UINTPTR)buf, sizeof(buf));

    u32 ctrl = use_xadc ? 0 : (CTRL_SEL_PATTERN | CTRL_MODE(0));   // mode 0 = ramp
    XGpio_DiscreteWrite(&gpio_ctrl, 1, PATTERN_PHASE_INC);
    XGpio_DiscreteWrite(&gpio_ctrl, 2, ctrl);

    filling = 0;
    if (XAxiDma_SimpleTransfer(&dma, (UINTPTR)buf[0], sizeof(buf[0]),
                               XAXIDMA_DEVICE_TO_DMA) != XST_SUCCESS) {
        xil_printf("First DMA transfer could not be started\r\n");
        return -1;
    }
    XGpio_DiscreteWrite(&gpio_ctrl, 2, ctrl | CTRL_ENABLE);

    XTime t_start, t_now, t_wait;
    XTime_GetTime(&t_start);
    t_wait = t_start;
    xil_printf("Running. Expect a status line every ~2 s.\r\n\r\n");

    // ---- Main loop ---------------------------------------------------------------
    u32 blocks = 0, ramp_errors = 0, err_prints = 0;
    u32 proc_max_us = 0;
    u16 prev = 0, wrap_anchor = 0;
    int have_prev = 0, wrap_state = 0, verdict_done = 0;
    u32 bmin = 0, bmax = 0, bmean = 0;

    while (1) {
        if (ready < 0) {
            // Nothing arriving? Say so every 3 s instead of hanging silently
            XTime_GetTime(&t_now);
            if (t_now - t_wait > 3ULL * COUNTS_PER_SECOND) {
                xil_printf("No block for 3 s: irq_blocks=%d  DMA S2MM status=0x%08x  drop_count=%d\r\n",
                           irq_blocks, dma_status(), drop_count());
                t_wait = t_now;
            }
            continue;
        }

        // Take the block
        Xil_ExceptionDisable();
        int b  = ready;
        ready  = -1;
        in_use = b;
        Xil_ExceptionEnable();

        pins_set(PIN_BUSY, 1);                                   // scope CH: busy
        XTime tp0, tp1;
        XTime_GetTime(&tp0);

#if USE_CACHE_INVALIDATE
        Xil_DCacheInvalidateRange((UINTPTR)buf[b], sizeof(buf[b]));
#endif
        const u16 *x = buf[b];

        if (!use_xadc) {
            // Ramp check: every sample = previous + 16 (mod 65536), except at
            // the wrap. The ADC code steps once per 1024-sample group, at the
            // same offset k in every group, so a group holding codes c and c+1
            // averages to 16c + k/64. When c = 4095, c+1 wraps to 0: that one
            // group has no exact value, so it is skipped, and the group after
            // it must equal (last value before the wrap) + 32.
            for (int i = 0; i < N; i++) {
                u16 expected = 0;
                int check = have_prev;
                if (wrap_state == 1) {                 // the wrap group itself
                    check = 0;
                    wrap_state = 2;
                } else if (wrap_state == 2) {          // first group after the wrap
                    expected = (u16)(wrap_anchor + 2 * RAMP_STEP);
                    wrap_state = 0;
                } else {
                    expected = (u16)(prev + RAMP_STEP);
                }
                if (check && x[i] != expected) {
                    ramp_errors++;
                    if (err_prints < 5) {
                        xil_printf("  ramp error: block %d index %d got 0x%04x expected 0x%04x\r\n",
                                   blocks, i, x[i], expected);
                        err_prints++;
                    }
                }
                if ((x[i] >> 4) == 0xFFE && wrap_state == 0) {   // code 4094: wrap is next
                    wrap_anchor = x[i];
                    wrap_state = 1;
                }
                prev = x[i];
                have_prev = 1;
            }
        } else {
            u32 mn = 0xFFFF, mx = 0, sum = 0;
            for (int i = 0; i < N; i++) {
                if (x[i] < mn) mn = x[i];
                if (x[i] > mx) mx = x[i];
                sum += x[i];
            }
            bmin = mn; bmax = mx; bmean = sum / N;
        }

        XTime_GetTime(&tp1);
        in_use = -1;
        pins_set(PIN_BUSY, 0);

        u32 us = ticks_to_us(tp1 - tp0);
        if (us > proc_max_us) proc_max_us = us;
        blocks++;
        XTime_GetTime(&t_wait);

        // ---- Status line (outside the busy window) -------------------------------
        if (blocks % PRINT_EVERY_BLOCKS == 0) {
            u32 fs  = sample_rate_mhz();
            u32 sec = (u32)((t_wait - t_start) / COUNTS_PER_SECOND);
            if (!use_xadc)
                xil_printf("blk %5d  t=%4ds  fs=%d.%03d Hz  ramp_err=%d  overruns=%d  dma_err=%d  drops=%d  proc_max=%dus\r\n",
                           blocks, sec, fs / 1000, fs % 1000, ramp_errors,
                           overruns, dma_errors, drop_count(), proc_max_us);
            else
                xil_printf("blk %5d  t=%4ds  fs=%d.%03d Hz  min=%5d max=%5d mean=%5d (ADC code = value/16)  overruns=%d drops=%d\r\n",
                           blocks, sec, fs / 1000, fs % 1000, bmin, bmax, bmean,
                           overruns, drop_count());
        }

        // ---- Gate 2 verdict --------------------------------------------------------
        if (!use_xadc && !verdict_done &&
            (t_wait - t_start) >= (XTime)TEST_SECONDS * COUNTS_PER_SECOND) {
            u32 fs    = sample_rate_mhz();
            u32 drops = drop_count();
            int pass  = (ramp_errors == 0 && overruns == 0 && dma_errors == 0 && drops == 0);
            xil_printf("\r\n========== GATE 2 after %d s ==========\r\n", TEST_SECONDS);
            xil_printf("blocks          : %d (%d samples)\r\n", blocks, blocks * N);
            xil_printf("sample rate     : %d.%03d Hz\r\n", fs / 1000, fs % 1000);
            xil_printf("ramp errors     : %d\r\n", ramp_errors);
            xil_printf("overruns        : %d\r\n", overruns);
            xil_printf("DMA errors      : %d\r\n", dma_errors);
            xil_printf("drop_count (HW) : %d\r\n", drops);
            xil_printf("max proc time   : %d us per block\r\n", proc_max_us);
            xil_printf("RESULT          : %s\r\n", pass ? "PASS" : "FAIL");
            xil_printf("========================================\r\n\r\n");
            pins_set(PIN_LED, pass);
            verdict_done = 1;
        }
    }

    return 0;
}
