# Hardware exports from the lab PC (Vivado 2020.1, PYNQ-Z2)

| File | What it is | Use it to |
|---|---|---|
| `day1/system_wrapper.xsa` | Day 1 hardware (PS, XADC over AXI-Lite, `gpio_out`, `pwm_dac`) with bitstream | Create a Vitis platform for `Vitis_Code/day1_passthrough` |
| `day1/day1_block_design.tcl` | `write_bd_tcl` export of the Day 1 block design | Rebuild the Day 1 block design in a new project (`source` it after adding `Verilog_Code/rtl/pwm_dac.v`), then run `Vivado_Scripts/day2_add_datapath.tcl` on a copy to get Day 2 |
| `day2/system_wrapper_day2.xsa` | Day 2 hardware (full data path: XADC stream, decimator, FIFO, AXI DMA, interrupt, control/status GPIOs) with bitstream | Create the Vitis platform for `Vitis_Code/day3_dma_pingpong` and `Vitis_Code/day45_detect` — no Vivado needed |

To run the project on a fresh PC with only Vitis: File → New → Application Project → *Create a new platform from hardware (XSA)* → `day2/system_wrapper_day2.xsa` → Empty Application → import `Vitis_Code/day45_detect/main.c` and `Vitis_Code/common/lfsp_dsp.[ch]`.
