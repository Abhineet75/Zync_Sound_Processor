`timescale 1ns / 1ps
// =============================================================================
// axis_src_mux : 2-to-1 AXI-Stream selector (real XADC  or  test pattern)
// =============================================================================
//
// What it does
//   sel = 0 -> source 0 (XADC Wizard) drives the output
//   sel = 1 -> source 1 (axis_test_pattern) drives the output
//
//   The source that is NOT selected sees tready = 1, so it keeps flowing and
//   its samples are simply discarded. This matters for the XADC Wizard: if
//   its stream were stalled, its internal FIFO would fill up and it would
//   start losing samples on its own.
//
// Purely combinational
//   No registers, no latency: the output is wires from the chosen input.
//   aclk / aresetn are not used by the logic; they are here only so Vivado's
//   IP integrator knows which clock the AXI-Stream interfaces belong to.
//
// When to change sel
//   Only while decim_avg.enable = 0. Switching mid-block is not harmful to
//   the hardware, but the block in progress would contain both sources.
// =============================================================================

module axis_src_mux (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s0_axis:s1_axis:m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire        aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,
    input  wire        sel,            // from AXI GPIO

    // ---- Source 0: XADC Wizard ---------------------------------------------
    input  wire [15:0] s0_axis_tdata,
    input  wire [4:0]  s0_axis_tid,
    input  wire        s0_axis_tvalid,
    output wire        s0_axis_tready,

    // ---- Source 1: test pattern --------------------------------------------
    input  wire [15:0] s1_axis_tdata,
    input  wire [4:0]  s1_axis_tid,
    input  wire        s1_axis_tvalid,
    output wire        s1_axis_tready,

    // ---- Output: to decim_avg ----------------------------------------------
    output wire [15:0] m_axis_tdata,
    output wire [4:0]  m_axis_tid,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready
);

    assign m_axis_tdata   = sel ? s1_axis_tdata  : s0_axis_tdata;
    assign m_axis_tid     = sel ? s1_axis_tid    : s0_axis_tid;
    assign m_axis_tvalid  = sel ? s1_axis_tvalid : s0_axis_tvalid;

    // Selected source gets the real tready; the other one is drained.
    assign s0_axis_tready = sel ? 1'b1          : m_axis_tready;
    assign s1_axis_tready = sel ? m_axis_tready : 1'b1;

endmodule
