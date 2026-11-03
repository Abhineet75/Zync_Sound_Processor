`timescale 1ns / 1ps
// =============================================================================
// axis_test_pattern : fake XADC — a known digital waveform on an AXI-Stream port
// =============================================================================
//
// Why it exists
//   The riskiest part of the project is the DMA data path (Days 2-3). If the
//   numbers in DDR look wrong, it is hard to tell whether the analog input,
//   the XADC setup, the decimator, the FIFO, the DMA or the cache is at
//   fault. This block produces samples that look exactly like the XADC's
//   (same format, same rate, same TID), but whose values are known in
//   advance. Select it with axis_src_mux and every value the ARM reads can
//   be checked against a formula — no function generator needed.
//
// How it works (a tiny DDS — direct digital synthesis)
//   * A divider produces one sample tick every CLKS_PER_SAMPLE clocks.
//     104 clocks at 100 MHz = 961.538 kSPS, the same rate the XADC runs at
//     with a 100 MHz DCLK (ADCCLK = 25 MHz, 26 ADCCLKs per conversion).
//   * On each tick a 32-bit phase accumulator adds phase_inc. The phase wraps
//     at 2^32, so the waveform frequency is
//
//         f_out = phase_inc * f_sample / 2^32
//         phase_inc = round(f_out * 2^32 / f_sample)
//
//     e.g. 20 Hz at 961,538 S/s -> phase_inc = 89,335.
//   * The top bits of the phase become a 12-bit code according to `mode`:
//
//       mode 0  RAMP      code = phase[31:20]            (sawtooth 0 -> 4095)
//       mode 1  TRIANGLE  rises 0 -> 4095, falls back    (fundamental + weak odd harmonics)
//       mode 2  SQUARE    0 or 4095                      (worst-case full-scale input)
//       mode 3  CONSTANT  2048                           (decimated value must be exactly 0x8000)
//
//   * Output format copies the XADC: code in tdata[15:4], tdata[3:0] = 0,
//     tid = CHANNEL.
//
// AXI-Stream
//   Honours tready: a sample waits while tvalid && !tready. If the next tick
//   arrives while a sample is still waiting, that tick is skipped (the phase
//   still advances, so the frequency stays correct). The decimator always
//   has tready = 1, so in practice nothing is ever skipped.
//
// Parameters
//   CLKS_PER_SAMPLE : clocks between samples (>= 1). Use 1-4 in simulation.
//   CHANNEL         : TID to stamp on each sample (17 = VAUX1, same as XADC).
// =============================================================================

module axis_test_pattern #(
    parameter integer CLKS_PER_SAMPLE = 104,
    parameter integer CHANNEL         = 17
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF m_axis, ASSOCIATED_RESET aresetn" *)
    input  wire        aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        aresetn,
    input  wire [1:0]  mode,         // from AXI GPIO
    input  wire [31:0] phase_inc,    // from AXI GPIO

    // ---- AXI-Stream master: same shape as the XADC Wizard's output ----------
    output reg  [15:0] m_axis_tdata,
    output wire [4:0]  m_axis_tid,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready
);

    localparam integer DIV_W = (CLKS_PER_SAMPLE > 1) ? $clog2(CLKS_PER_SAMPLE) : 1;

    localparam [1:0] MODE_RAMP     = 2'd0,
                     MODE_TRIANGLE = 2'd1,
                     MODE_SQUARE   = 2'd2,
                     MODE_CONST    = 2'd3;

    assign m_axis_tid = CHANNEL;

    // =========================================================================
    // Sample-rate divider
    // =========================================================================
    reg  [DIV_W-1:0] div;
    wire             tick = (div == CLKS_PER_SAMPLE - 1);

    // =========================================================================
    // Phase accumulator and waveform shaping
    // =========================================================================
    reg  [31:0] phase;
    wire [31:0] phase_next = phase + phase_inc;

    reg  [11:0] code;
    always @(*) begin
        case (mode)
            MODE_RAMP:     code = phase_next[31:20];
            MODE_TRIANGLE: code = phase_next[31] ? ~phase_next[30:19] : phase_next[30:19];
            MODE_SQUARE:   code = phase_next[31] ? 12'hFFF : 12'h000;
            default:       code = 12'h800;
        endcase
    end

    // =========================================================================
    // Main process
    // =========================================================================
    always @(posedge aclk) begin
        if (!aresetn) begin
            div           <= {DIV_W{1'b0}};
            phase         <= 32'd0;
            m_axis_tdata  <= 16'd0;
            m_axis_tvalid <= 1'b0;
        end else begin
            div <= tick ? {DIV_W{1'b0}} : div + 1'b1;

            if (m_axis_tvalid && m_axis_tready)
                m_axis_tvalid <= 1'b0;

            if (tick) begin
                phase <= phase_next;
                if (!m_axis_tvalid || m_axis_tready) begin
                    m_axis_tdata  <= {code, 4'b0000};   // MSB-aligned like the XADC
                    m_axis_tvalid <= 1'b1;
                end
            end
        end
    end

endmodule
