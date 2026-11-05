# =============================================================================
# day2_add_datapath.tcl : turn the Day 1 block design into the Day 2 design
# =============================================================================
#
# Run this in the Vivado Tcl Console with a COPY of the working Day 1 project
# open (File -> Project -> Save As...). Do not run it twice on the same project.
#
#     source C:/path/to/Zync_Sound_Processor/Vivado_Scripts/day2_add_datapath.tcl
#
# What it expects from Day 1 (found by type, so instance names don't matter):
#   ZYNQ7 Processing System, XADC Wizard (AXI4Lite, single channel, VAUX1),
#   Processor System Reset, gpio_out + pwm_dac (left untouched).
#
# What it adds:
#   XADC AXI4-Stream --> axis_src_mux <-- axis_test_pattern
#                             |
#                         decim_avg  --> AXIS Data FIFO (1024) --> AXI DMA (S2MM)
#                             |                                     |      |
#                    drop_count -> gpio_in ch1          HP0 -> DDR   IRQ_F2P
#                    blk_toggle -> external port
#   gpio_ctrl ch1 (32 out) -> test pattern phase_inc
#   gpio_ctrl ch2 (4 out)  -> [0] decim enable, [1] mux sel, [3:2] pattern mode
#   gpio_in   ch2 (2 in)   <- external port sw[1:0]
#
# Every step prints what it does; if Vivado stops with an error, the last
# "[day2]" line shows which step failed.
# =============================================================================

set script_dir [file dirname [file normalize [info script]]]
set rtl_dir    [file normalize [file join $script_dir .. Verilog_Code rtl]]

proc say {msg} { puts "\[day2\] $msg" }

# Latest version of a Xilinx IP in this Vivado's catalog, e.g. xilinx.com:ip:axi_dma:7.1
proc ipdef {name} {
    set defs [lsort [get_ipdefs -all "xilinx.com:ip:${name}:*"]]
    if {[llength $defs] == 0} { error "IP '$name' not found in the IP catalog" }
    return [lindex $defs end]
}

# The single existing cell of a given IP type
proc one_cell {pattern what} {
    set c [get_bd_cells -quiet -filter "VLNV =~ $pattern"]
    if {[llength $c] != 1} { error "expected exactly one $what in the block design, found [llength $c]: $c" }
    return $c
}

# =============================================================================
# 0. Open the block design
# =============================================================================
set bd_file [get_files -quiet *.bd]
if {[llength $bd_file] != 1} { error "expected exactly one .bd file in the project, found: $bd_file" }
open_bd_design $bd_file
say "opened [file tail $bd_file]"

if {[llength [get_bd_cells -quiet decim_0]] != 0} {
    error "decim_0 already exists: this script has already been run on this project"
}

# =============================================================================
# 1. Add the RTL sources (by reference, so edits in the repo are picked up)
# =============================================================================
foreach f {decim_avg.v axis_test_pattern.v axis_src_mux.v} {
    set p [file join $rtl_dir $f]
    if {![file exists $p]} { error "missing source file $p" }
    if {[llength [get_files -quiet -of_objects [get_filesets sources_1] "*$f"]] == 0} {
        add_files -norecurse -fileset sources_1 $p
        say "added $f"
    }
}
update_compile_order -fileset sources_1

# =============================================================================
# 2. Existing Day 1 cells
# =============================================================================
set ps   [one_cell "*processing_system7*" "ZYNQ7 Processing System"]
set xadc [one_cell "*xadc_wiz*"           "XADC Wizard"]
set rst  [one_cell "*proc_sys_reset*"     "Processor System Reset"]
set clk  [get_bd_pins $ps/FCLK_CLK0]
set rstn [get_bd_pins $rst/peripheral_aresetn]
say "found $ps, $xadc, $rst"

# =============================================================================
# 3. PS: HP0 slave port for the DMA, fabric interrupt input
# =============================================================================
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0       {1} \
    CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
    CONFIG.PCW_IRQ_F2P_INTR        {1} \
] $ps
say "PS: HP0 + IRQ_F2P enabled"

# =============================================================================
# 4. XADC: add the AXI4-Stream output (keeps AXI-Lite for the XSysMon driver)
# =============================================================================
set_property CONFIG.ENABLE_AXI4STREAM {true} $xadc
say "XADC: AXI4-Stream enabled"

# =============================================================================
# 5. New cells
# =============================================================================
create_bd_cell -type module -reference axis_test_pattern pattern_0
create_bd_cell -type module -reference axis_src_mux      src_mux_0
create_bd_cell -type module -reference decim_avg         decim_0
say "module references added"

create_bd_cell -type ip -vlnv [ipdef axis_data_fifo] axis_fifo_0
set_property CONFIG.FIFO_DEPTH {1024} [get_bd_cells axis_fifo_0]

create_bd_cell -type ip -vlnv [ipdef axi_dma] axi_dma_0
set_property -dict [list \
    CONFIG.c_include_sg             {0}  \
    CONFIG.c_include_mm2s           {0}  \
    CONFIG.c_include_s2mm           {1}  \
    CONFIG.c_sg_length_width        {14} \
    CONFIG.c_m_axi_s2mm_data_width  {32} \
    CONFIG.c_s_axis_s2mm_tdata_width {16} \
    CONFIG.c_s2mm_burst_size        {16} \
] [get_bd_cells axi_dma_0]
say "FIFO (1024) and AXI DMA (S2MM only, simple mode) added"

create_bd_cell -type ip -vlnv [ipdef axi_gpio] gpio_ctrl
set_property -dict [list \
    CONFIG.C_GPIO_WIDTH   {32} CONFIG.C_ALL_OUTPUTS   {1} \
    CONFIG.C_IS_DUAL      {1}                             \
    CONFIG.C_GPIO2_WIDTH  {4}  CONFIG.C_ALL_OUTPUTS_2 {1} \
] [get_bd_cells gpio_ctrl]

create_bd_cell -type ip -vlnv [ipdef axi_gpio] gpio_in
set_property -dict [list \
    CONFIG.C_GPIO_WIDTH   {32} CONFIG.C_ALL_INPUTS   {1} \
    CONFIG.C_IS_DUAL      {1}                            \
    CONFIG.C_GPIO2_WIDTH  {2}  CONFIG.C_ALL_INPUTS_2 {1} \
] [get_bd_cells gpio_in]
say "gpio_ctrl and gpio_in added"

# gpio_ctrl ch2 split into fields: [0] enable, [1] sel, [3:2] mode
foreach {name from to} {slice_en 0 0  slice_sel 1 1  slice_mode 3 2} {
    create_bd_cell -type ip -vlnv [ipdef xlslice] $name
    set_property -dict [list CONFIG.DIN_WIDTH {4} CONFIG.DIN_FROM $from CONFIG.DIN_TO $to] [get_bd_cells $name]
    connect_bd_net [get_bd_pins gpio_ctrl/gpio2_io_o] [get_bd_pins $name/Din]
}
say "control slices added"

# =============================================================================
# 6. Stream path
# =============================================================================
connect_bd_intf_net [get_bd_intf_pins $xadc/M_AXIS]        [get_bd_intf_pins src_mux_0/s0_axis]
connect_bd_intf_net [get_bd_intf_pins pattern_0/m_axis]    [get_bd_intf_pins src_mux_0/s1_axis]
connect_bd_intf_net [get_bd_intf_pins src_mux_0/m_axis]    [get_bd_intf_pins decim_0/s_axis]
connect_bd_intf_net [get_bd_intf_pins decim_0/m_axis]      [get_bd_intf_pins axis_fifo_0/S_AXIS]
connect_bd_intf_net [get_bd_intf_pins axis_fifo_0/M_AXIS]  [get_bd_intf_pins axi_dma_0/S_AXIS_S2MM]
say "stream path connected: XADC/pattern -> mux -> decim -> FIFO -> DMA"

# =============================================================================
# 7. Control, status, interrupt, external ports
# =============================================================================
connect_bd_net [get_bd_pins slice_en/Dout]       [get_bd_pins decim_0/enable]
connect_bd_net [get_bd_pins slice_sel/Dout]      [get_bd_pins src_mux_0/sel]
connect_bd_net [get_bd_pins slice_mode/Dout]     [get_bd_pins pattern_0/mode]
connect_bd_net [get_bd_pins gpio_ctrl/gpio_io_o] [get_bd_pins pattern_0/phase_inc]
connect_bd_net [get_bd_pins decim_0/drop_count]  [get_bd_pins gpio_in/gpio_io_i]

create_bd_port -dir I -from 1 -to 0 sw
connect_bd_net [get_bd_ports sw] [get_bd_pins gpio_in/gpio2_io_i]
create_bd_port -dir O blk_toggle
connect_bd_net [get_bd_pins decim_0/blk_toggle] [get_bd_ports blk_toggle]

connect_bd_net [get_bd_pins axi_dma_0/s2mm_introut] [get_bd_pins $ps/IRQ_F2P]
say "control, status, interrupt and ports connected"

# =============================================================================
# 8. Memory-mapped connections (Vivado's own connection automation)
# =============================================================================
foreach slave {gpio_ctrl/S_AXI gpio_in/S_AXI axi_dma_0/S_AXI_LITE} {
    apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
        Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
        Master [get_bd_intf_pins $ps/M_AXI_GP0] Slave "/$slave" \
        ddr_seg {Auto} intc_ip {Auto} master_apm {0}] [get_bd_intf_pins $slave]
    say "AXI-Lite: $slave -> M_AXI_GP0"
}
apply_bd_automation -rule xilinx.com:bd_rule:axi4 -config [list \
    Clk_master {Auto} Clk_slave {Auto} Clk_xbar {Auto} \
    Master {/axi_dma_0/M_AXI_S2MM} Slave [get_bd_intf_pins $ps/S_AXI_HP0] \
    ddr_seg {Auto} intc_ip {Auto} master_apm {0}] [get_bd_intf_pins $ps/S_AXI_HP0]
say "DMA M_AXI_S2MM -> HP0 -> DDR"

# =============================================================================
# 9. Every remaining clock / reset input on the new cells -> FCLK_CLK0 / peripheral_aresetn
# =============================================================================
foreach cell [list pattern_0 src_mux_0 decim_0 axis_fifo_0 axi_dma_0 gpio_ctrl gpio_in $xadc] {
    foreach p [get_bd_pins -quiet -of_objects [get_bd_cells $cell] -filter {TYPE == clk && DIR == I}] {
        if {[llength [get_bd_nets -quiet -of_objects $p]] == 0} {
            connect_bd_net $clk $p
            say "clock -> $p"
        }
    }
    foreach p [get_bd_pins -quiet -of_objects [get_bd_cells $cell] -filter {TYPE == rst && DIR == I}] {
        if {[llength [get_bd_nets -quiet -of_objects $p]] == 0} {
            connect_bd_net $rstn $p
            say "reset -> $p"
        }
    }
}

# =============================================================================
# 10. Addresses, validate, save
# =============================================================================
assign_bd_address
validate_bd_design
save_bd_design
say "DONE. Next: Generate Bitstream, then File -> Export -> Export Hardware (include bitstream)."
