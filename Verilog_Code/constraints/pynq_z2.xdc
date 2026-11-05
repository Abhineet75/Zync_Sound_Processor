## =============================================================================
## pynq_z2.xdc : pin constraints for the Low-Frequency Signal Processor
## =============================================================================
##
## Pins checked against the official TUL master XDC "PYNQ-Z2 v1.0.xdc"
## (2026-11-03): A0 = AD1 (VAUX1) on E17/D18, Pmod JA, LED0, SW0/SW1 all match.
##
## The port names on the right (get_ports {...}) must match the EXTERNAL port
## names of your block design wrapper. The README tells you which name to give
## each port when you "Make External" in IP integrator.
##
## Clocks: none here. Everything in the fabric runs on FCLK_CLK0 (100 MHz)
## from the Zynq PS, which Vivado constrains automatically.
## =============================================================================

## ---- Analog input: Arduino header A0 -> XADC VAUX1 (onboard 0-3.3 V divider)
## The XADC Wizard exposes this as the Vaux1 interface; rename the external
## interface to "Vaux1" so the ports become Vaux1_v_p / Vaux1_v_n.
## IOSTANDARD is required by DRC even though these pins are used as analog.
set_property -dict { PACKAGE_PIN E17  IOSTANDARD LVCMOS33 } [get_ports { Vaux1_v_p }]
set_property -dict { PACKAGE_PIN D18  IOSTANDARD LVCMOS33 } [get_ports { Vaux1_v_n }]

## ---- Pmod JA (top row): signals for the oscilloscope / RC filter ---------
##  JA1  pwm_out      -> 1 kOhm + 1 uF RC low-pass -> scope CH2
##  JA2  dbg_pins[0]  busy pin        (software, main loop)  -> scope CH3
##  JA3  dbg_pins[1]  buffer pin      (software, DMA ISR)    -> scope CH4
##  JA4  dbg_pins[2]  detection pin   (software)
set_property -dict { PACKAGE_PIN Y18  IOSTANDARD LVCMOS33 } [get_ports { pwm_out }]
set_property -dict { PACKAGE_PIN Y19  IOSTANDARD LVCMOS33 } [get_ports { dbg_pins[0] }]
set_property -dict { PACKAGE_PIN Y16  IOSTANDARD LVCMOS33 } [get_ports { dbg_pins[1] }]
set_property -dict { PACKAGE_PIN Y17  IOSTANDARD LVCMOS33 } [get_ports { dbg_pins[2] }]

## ---- Pmod JA (bottom row) ------------------------------------------------
##  JA7  blk_toggle   hardware block toggle from decim_avg (true sample rate)
set_property -dict { PACKAGE_PIN U18  IOSTANDARD LVCMOS33 } [get_ports { blk_toggle }]

## ---- LED0: detection indicator (dbg_pins[3]) -------------------------------
set_property -dict { PACKAGE_PIN R14  IOSTANDARD LVCMOS33 } [get_ports { dbg_pins[3] }]

## ---- Slide switches SW0, SW1: build/mode select read by software ----------
set_property -dict { PACKAGE_PIN M20  IOSTANDARD LVCMOS33 } [get_ports { sw[0] }]
set_property -dict { PACKAGE_PIN M19  IOSTANDARD LVCMOS33 } [get_ports { sw[1] }]

## Pins that only drive the scope are slow; tell timing analysis not to care.
set_false_path -to [get_ports { pwm_out dbg_pins[*] blk_toggle }]
set_false_path -from [get_ports { sw[*] }]
