# Day 3 results — Gate 2 passed (2026-11-06)

DMA ping-pong data path on hardware: XADC / test pattern → `decim_avg` → AXI-Stream FIFO (1024) → AXI DMA (S2MM, simple mode) → DDR, with the block-complete interrupt re-arming the DMA into the other buffer and the ARM invalidating the data cache before reading.

Program: [`Vitis_Code/day3_dma_pingpong/main.c`](../../Vitis_Code/day3_dma_pingpong/main.c), hardware from [`Vivado_Scripts/day2_add_datapath.tcl`](../../Vivado_Scripts/day2_add_datapath.tcl) (implemented at 100 MHz, WNS = +2.56 ns).

## Gate 2: no lost samples

Test pattern ramp (`phase_inc = 1024`): every decimated sample must be exactly 16 above the previous one, across block boundaries, with the ramp wrap handled exactly. Any lost, repeated or stale sample breaks this.

| Counter | After > 15 min (t = 905 s, 1660 blocks, ≈ 850,000 samples) |
|---|---|
| Ramp errors (lost / repeated / stale samples) | **0** |
| Overruns (CPU missed a block) | **0** |
| DMA errors | **0** |
| Hardware drop counter (`decim_avg.drop_count`) | **0** |
| Gate 2 verdict at 600 s | **PASS** (LD0 lit) |

Status line format: `blk 1656 t= 902s fs=939.002 Hz ramp_err=0 overruns=0 dma_err=0 drops=0 proc_max=36us`

## Measurements

| Measurement | Result | Method |
|---|---|---|
| Decimated sample rate | **939.002 Hz** | ARM global timer over hundreds of block interrupts; matches 100 MHz ÷ 4 ÷ 26 ÷ 1024 = 939.002 Hz exactly |
| Processing time per block (cache invalidate + ramp check, Debug build) | **36 µs** (UART); **≈ 36.6 µs** (busy pin JA2 on the scope, 3.66 div × 10 µs) | |
| CPU utilization | 36 µs / 545 ms ≈ **0.007 %** | |

## Notes

- The Vitis serial terminal froze once during the run (output resumed after reconnecting); the program itself never stopped.
- Scope captures of the busy pulse showed a differentiated shape (spikes, sag, undershoot) because the probe ground was not connected; the pulse **width** is still valid. Re-capture with the probe ground on the Arduino header GND, verified against the 3V3 pin.
