// =============================================================================
// Day 1 : XADC -> ARM -> PWM passthrough (Gate 1)
// =============================================================================
//
// What it does
//   * Reads the latest XADC conversion of VAUX1 (Arduino A0) over AXI-Lite.
//   * Writes the top 8 bits of it to the PWM duty register (gpio_out ch1),
//     so the RC filter output on JA1 follows the input on A0.
//   * Toggles dbg_pins[0] (JA2) every loop, so the square wave on that pin
//     has a frequency of half the loop rate (one period = two loops).
//   * Every 0.5 s prints the min / max / last raw code seen, so you can check
//     the input span over UART (115200 baud) without stalling the loop much.
//
// Hardware it expects (Day 1 block design)
//   * XADC Wizard, AXI4Lite, single channel, continuous, VAUX1 unipolar
//   * AXI GPIO "gpio_out": ch1 = 8-bit output -> pwm_dac/duty
//                          ch2 = 4-bit output -> dbg_pins[3:0]
//
// Expected numbers
//   XADC raw value is 16 bits with the 12-bit code in [15:4].
//   0 V on A0 -> code ~0, 3.3 V -> code ~4095 (A0 driven directly).
//   With the 1 kOhm series protection resistor the onboard divider reads
//   about a quarter lower: 1.65 V offset, 1 Vpp sine -> roughly 1100 .. 2050.
// =============================================================================

#include "xparameters.h"
#include "xsysmon.h"
#include "xgpio.h"
#include "xil_printf.h"
#include "xtime_l.h"

#define GPIO_CH_DUTY   1
#define GPIO_CH_PINS   2
#define VAUX1_CHANNEL  (XSM_CH_AUX_MIN + 1)    // channel address 17 = 0x11

static XSysMon sysmon;
static XGpio   gpio_out;

int main(void)
{
    // Progress prints: if the ARM hangs on an AXI access, the last one shows where
    xil_printf("\r\n[1] main started\r\n");

    // ---- XADC ----------------------------------------------------------------
    XSysMon_Config *sm_cfg = XSysMon_LookupConfig(XPAR_SYSMON_0_DEVICE_ID);
    xil_printf("[2] XADC config %s\r\n", sm_cfg ? "found" : "NULL");
    if (sm_cfg == NULL ||
        XSysMon_CfgInitialize(&sysmon, sm_cfg, sm_cfg->BaseAddress) != XST_SUCCESS) {
        xil_printf("XADC init failed\r\n");
        return -1;
    }
    xil_printf("[3] XADC ready\r\n");

    // ---- GPIO: both channels are outputs ---------------------------------------
    if (XGpio_Initialize(&gpio_out, XPAR_GPIO_0_DEVICE_ID) != XST_SUCCESS) {
        xil_printf("GPIO init failed\r\n");
        return -1;
    }
    xil_printf("[4] GPIO ready\r\n");
    XGpio_SetDataDirection(&gpio_out, GPIO_CH_DUTY, 0x00);   // 0 = output
    XGpio_SetDataDirection(&gpio_out, GPIO_CH_PINS, 0x0);

    xil_printf("\r\nDay 1 passthrough: A0 (VAUX1) -> PWM on JA1\r\n");

    // ---- Main loop ------------------------------------------------------------
    XTime    t_now, t_last;
    uint32_t pins = 0;
    uint16_t code = 0, cmin = 0xFFFF, cmax = 0;

    XTime_GetTime(&t_last);

    while (1) {
        u16 raw = XSysMon_GetAdcData(&sysmon, VAUX1_CHANNEL);
        code = raw >> 4;                                     // 12-bit code
        XGpio_DiscreteWrite(&gpio_out, GPIO_CH_DUTY, raw >> 8);   // top 8 bits

        if (code < cmin) cmin = code;
        if (code > cmax) cmax = code;

        pins ^= 0x1;                                         // loop-rate pin (JA2)
        XGpio_DiscreteWrite(&gpio_out, GPIO_CH_PINS, pins);

        XTime_GetTime(&t_now);
        if (t_now - t_last >= COUNTS_PER_SECOND / 2) {
            // 1 V / 4096 at the XADC, x3.3 for the onboard A0 divider
            // -> ~0.81 mV per code at the A0 pin
            xil_printf("code last=%4d  min=%4d  max=%4d   (~%d mV at A0)\r\n",
                       code, cmin, cmax, (int)((code * 3300) / 4095));
            cmin = 0xFFFF;
            cmax = 0;
            t_last = t_now;
        }
    }

    return 0;
}
