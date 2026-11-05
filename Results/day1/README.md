# Day 1 results — Gate 1 passed (2026-11-05)

Signal path proven end to end: function generator → A0 → XADC → ARM (polling over AXI-Lite) → AXI GPIO → `pwm_dac` → RC filter → scope.

## Setup

| Item | Value |
|---|---|
| Board | PYNQ-Z2, JTAG boot (JP1), USB power (J9) |
| Tools | Vivado / Vitis 2020.1, bare-metal standalone |
| Hardware | Zynq PS (FCLK0 = 100 MHz), XADC Wizard (AXI4Lite, single channel, continuous, VAUX1 unipolar), AXI GPIO `gpio_out`, `pwm_dac` |
| Software | [`Vitis_Code/day1_passthrough/main.c`](../../Vitis_Code/day1_passthrough/main.c) |
| Input | Tektronix AFG, sine, 1 Vpp, +1.65 V offset, load **High Z**; 1.2 kΩ series resistor (2 × 2.4 kΩ in parallel) into A0 |
| Output filter | JA1 → 1.2 kΩ → node → 3.3 µF + 4.7 µF in series (≈ 1.94 µF) → GND; f_c ≈ 68 Hz |
| Scope | Tektronix TDS1002, both probes 10X |

## Measurements

| Measurement | Result |
|---|---|
| ADC codes, A0 open (pulled to 0 V by onboard divider) | 3 – 14 (single-sample noise ≈ ±5 codes) |
| ADC codes with 1 Vpp sine | min ≈ 867, max ≈ 1743 (≈ 876 codes swing), stable to ±2 codes between prints |
| Voltage at A0 (after 1.2 kΩ) | 760 mV pk-pk (CH1) — 1.2 kΩ and the ≈ 3.3 kΩ onboard divider form a ≈ 0.73 divider |
| Scale at A0 | ≈ 1150 codes per volt |
| RC output (CH2) | sine at the input frequency, centred ≈ 1.1 V; pk-pk reads 1.28 V at 2 V/div (inflated by PWM spikes; expected ≈ 0.7 V — re-measure with Acquire → Average at 500 mV/div) |
| Frequency sweep 5 → 100 Hz | CH2 follows the input throughout, shrinking above ~50 Hz (68 Hz RC filter) |

Note: at 20 Hz the UART `last` value repeats because prints are exactly 0.5 s = 10 signal periods apart (stroboscopic sampling), not a fault.

## Captures

- [`gate1_scope_ch1_ch2.jpg`](gate1_scope_ch1_ch2.jpg) — clean capture: CH1 = A0 input (1 V/div), CH2 = RC output (2 V/div), 50 ms/div
- [`gate1_scope_first_trigger.jpg`](gate1_scope_first_trigger.jpg) — first triggered capture (CH1 still at 10 V/div)

## Lessons from bring-up

- Boot jumper must be on JTAG **before** power-on; otherwise Vitis reports "Could not find ARM device".
- Vitis prints "Build Finished" even when compilation fails, and then runs the old `.elf` — always check the Console for `error:`.
- Workspace and project paths must not contain spaces; the lab account's home folder (`C:\Users\EMB LAB`) does.
- 68 Ω on the PWM pin would draw up to ~48 mA (pin default drive is 12 mA); keep the series resistor ≥ 680 Ω.
